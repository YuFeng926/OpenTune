
#include "CaptureSession.h"

#include "CaptureCompactor.h"
#include "CapturePersistence.h"
#include "../../Content/EditableContentSnapshot.h"
#include "../../Utils/AppLogger.h"
#include "../../Utils/ChannelLayoutLogger.h"
#include "../../Utils/PitchCurve.h"

#include <algorithm>

namespace OpenTune::Capture {

namespace {
    constexpr double kMaxSegmentSeconds = 600.0;  // 10 minutes per spec
    constexpr int kReclaimGraceTicks = 4;          // tick() iterations before destroying pending segments

    SegmentInfo makeSegmentInfo(const CaptureSegment& segment)
    {
        SegmentInfo info;
        info.contentKey = segment.contentKey;
        info.T_start = segment.T_start.load(std::memory_order_acquire);
        info.durationSeconds = segment.durationSeconds;
        info.state = segment.state.load(std::memory_order_acquire);
        return info;
    }

    CaptureSegment* findMutableSegmentByContentKey(
        std::vector<std::unique_ptr<CaptureSegment>>& segments,
        const ContentKey& key)
    {
        if (key.domainKind != DomainKind::RegularVST3Capture)
            return nullptr;

        for (auto& segment : segments) {
            if (segment != nullptr && segment->contentKey.objectId == key.objectId)
                return segment.get();
        }

        return nullptr;
    }
}

CaptureSession::CaptureSession(ProcessorBindings bindings)
    : bindings_(std::move(bindings))
{
    auto empty = std::make_shared<const SegmentsView>();
    std::atomic_store(&publishedSegments_, empty);
}

CaptureSession::~CaptureSession() = default;

// ─── Lifecycle ────────────────────────────────────────────────────────────

void CaptureSession::prepareToPlay(double sampleRate, int maxBlockSize, int hostInputChannels)
{
    currentSampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    currentMaxBlockSize_ = juce::jmax(0, maxBlockSize);

    // Clamp the declared host bus channel count to {1, 2}. This is the SOLE source
    // of truth for capture layout per channel-layout-policy spec.
    const int rawDecl = hostInputChannels;
    const int clamped = (hostInputChannels >= 2) ? 2 : 1;
    const int oldChannels = captureChannels_.exchange(clamped, std::memory_order_acq_rel);

    dryScratch_.setSize(clamped, juce::jmax(currentMaxBlockSize_, 512), false, true, false);

    ChannelLayoutLog::logEntry("vst3-prepare", rawDecl, clamped);
    if (oldChannels != 0 && oldChannels != clamped) {
        ChannelLayoutLog::logSessionReconfig(oldChannels, clamped);
    }
}

void CaptureSession::releaseResources()
{
    dryScratch_.setSize(0, 0);
}

// ─── User actions ─────────────────────────────────────────────────────────

bool CaptureSession::armNewCapture()
{
    bool needPublish = false;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);

        // Cleanup of stale empty Pending segments left behind by interrupted
        // stop sequences. Pending normally lasts only a few timer ticks.
        auto it = mutableSegments_.begin();
        while (it != mutableSegments_.end()) {
            auto& seg = **it;
            if (seg.state.load(std::memory_order_acquire) == SegmentState::Pending
                && seg.fifo.getTotalWrittenSamples() == 0
                && !seg.writerActive.load(std::memory_order_acquire)) {
                AppLogger::log("CaptureSession::armNewCapture: dropping stale empty Pending segment id="
                               + juce::String(static_cast<juce::int64>(seg.contentKey.objectId)));
                queueForReclaimLocked(std::move(*it));
                it = mutableSegments_.erase(it);
                needPublish = true;
            } else {
                ++it;
            }
        }

        // Reject if any Capturing, Pending, or Processing segment exists.
        for (const auto& seg : mutableSegments_) {
            const auto s = seg->state.load(std::memory_order_acquire);
            if (s == SegmentState::Capturing || s == SegmentState::Pending || s == SegmentState::Processing) {
                AppLogger::warn("CaptureSession::armNewCapture rejected: existing segment state="
                                + juce::String(static_cast<int>(s)));
                return false;
            }
        }

        auto seg = std::make_unique<CaptureSegment>();
        const int ch = captureChannels_.load(std::memory_order_acquire);
        const uint64_t id = nextId();
        seg->contentKey = ContentKey{DomainKind::RegularVST3Capture, id, 0};
        seg->creationOrder = id;
        seg->captureSampleRate = currentSampleRate_;
        seg->captureChannels = ch;
        seg->maxSamples = static_cast<int>(std::ceil(kMaxSegmentSeconds * currentSampleRate_));
        seg->fifo.reserve(seg->captureChannels, seg->maxSamples);
        // Pre-allocate against the full PCM budget. A host may submit blocks
        // smaller than its advertised maximum, so maxBlockSize cannot bound the
        // number of discontinuous spans without permitting metadata loss.
        const int spanCapacity = juce::jmax(1, seg->maxSamples);
        seg->spans.resize(spanCapacity);
        seg->content = std::make_unique<OpenTune::CaptureSegmentContent>(id);

        AppLogger::log("CaptureSession::armNewCapture id=" + juce::String(static_cast<juce::int64>(id))
                     + " ch=" + juce::String(ch) + " sr=" + juce::String(currentSampleRate_, 1));

        ChannelLayoutLog::logSegmentArm(static_cast<juce::int64>(id), seg->captureChannels);

        mutableSegments_.push_back(std::move(seg));
        needPublish = true;
    }
    if (needPublish)
        publishSegmentsView();
    return true;
}

void CaptureSession::stopCapture()
{
    CaptureSegment* capturing = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        for (auto& seg : mutableSegments_) {
            if (seg->state.load(std::memory_order_acquire) == SegmentState::Capturing) {
                capturing = seg.get();
                break;
            }
        }
    }
    if (capturing == nullptr)
        return;

    capturing->state.store(SegmentState::Pending, std::memory_order_release);

    AppLogger::log("CaptureSession::stopCapture id=" + juce::String(static_cast<juce::int64>(capturing->contentKey.objectId)));

    publishSegmentsView();
}

// ─── Audio thread ─────────────────────────────────────────────────────────

void CaptureSession::processBlock(juce::AudioBuffer<float>& buffer,
                                  int64_t hostAbsoluteSample,
                                  double hostSampleRate,
                                  bool isPlaying) noexcept
{
    auto view = std::atomic_load(&publishedSegments_);
    if (view == nullptr || view->snapshot.empty())
        return;  // No segments → dry pass-through

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return;

    // Invalid host position: skip both replace and capture.
    if (hostAbsoluteSample < 0)
        return;

    // Step 1: Backup dry input into the scratch buffer, but only up to the
    // session's declared capture channel count. Anything beyond that is host-side
    // bus padding we explicitly do not capture (per channel-layout-policy spec).
    const int sessionChannels = captureChannels_.load(std::memory_order_acquire);
    const int copyChannels = juce::jmin(numChannels, sessionChannels,
                                         dryScratch_.getNumChannels());
    if (copyChannels <= 0 || dryScratch_.getNumSamples() < numSamples) {
        return;
    }
    for (int ch = 0; ch < copyChannels; ++ch)
        dryScratch_.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    // Numerical safety only: replace NaN / +Inf / -Inf with 0. NOT a content sanitizer.
    for (int ch = 0; ch < copyChannels; ++ch) {
        float* p = dryScratch_.getWritePointer(ch);
        for (int s = 0; s < numSamples; ++s) {
            const float v = p[s];
            if (!std::isfinite(v)) {
                p[s] = 0.0f;
            }
        }
    }

    // Step 2/3: replace every overlapping Edited segment in creation order.
    // Later segments run last, so later reads overwrite earlier reads. Each call
    // covers only the intersection and leaves the rest of the host block intact.
    if (isPlaying && bindings_.replaceWithRendered) {
        const int64_t blockStart = hostAbsoluteSample;
        const int64_t blockEnd = hostAbsoluteSample + static_cast<int64_t>(numSamples);
        for (auto* seg : view->snapshot) {
            if (seg->state.load(std::memory_order_acquire) != SegmentState::Edited)
                continue;

            const int64_t segStart = seg->hostStartSample.load(std::memory_order_acquire);
            const int64_t segCount = seg->hostSampleCount.load(std::memory_order_acquire);
            const int64_t segEnd = segStart + segCount;
            const int64_t overlapStart = std::max(blockStart, segStart);
            const int64_t overlapEnd = std::min(blockEnd, segEnd);
            if (overlapEnd <= overlapStart)
                continue;

            const int destStart = static_cast<int>(overlapStart - blockStart);
            const int overlapNumSamples = static_cast<int>(overlapEnd - overlapStart);
            const int64_t readStartSample = overlapStart - segStart;
            bindings_.replaceWithRendered(buffer, destStart, overlapNumSamples,
                                          seg->contentKey, readStartSample, hostSampleRate);
        }
    }

    // Step 4: Capture dry into Capturing segment's fifo (isPlaying only).
    //
    // Writer-active handshake: CAS writerActive before FIFO write, release after.
    // State re-check after CAS ensures no write after stopCapture sets Pending.
    // fifo.write returns accepted count; span.sampleCount += accepted only.
    // No auto-stop: stopCapture is the only way to end capture.

    if (!isPlaying)
        return;

    for (auto* seg : view->snapshot) {
        if (seg->state.load(std::memory_order_acquire) != SegmentState::Capturing)
            continue;

        // CAS writer-active: only one writer (audio thread) at a time.
        {
            bool expected = false;
            if (!seg->writerActive.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                break;
        }

        // Re-check state after CAS: if stopCapture already set Pending, do not write.
        if (seg->state.load(std::memory_order_acquire) != SegmentState::Capturing) {
            seg->writerActive.store(false, std::memory_order_release);
            break;
        }

        // --- Capture write path. All exits must release writerActive. ---
        bool wroteBlock = false;
        do {
            // --- Span tracking ---
            const int curSpans = seg->numSpans.load(std::memory_order_relaxed);
            int activeSpanIndex = -1;
            int spanOffset = seg->fifo.getTotalWrittenSamples();
            if (curSpans == 0) {
                activeSpanIndex = 0;
            } else {
                auto& lastSpan = seg->spans[curSpans - 1];
                const int64_t expectedNext = lastSpan.hostStartSample
                                           + static_cast<int64_t>(lastSpan.sampleCount);
                if (hostAbsoluteSample != expectedNext) {
                    // Discontinuity (loop/seek): start new span within capacity.
                    if (curSpans < static_cast<int>(seg->spans.size())) {
                        activeSpanIndex = curSpans;
                        spanOffset = seg->fifo.getTotalWrittenSamples();
                    } else {
                        // Span capacity exhausted: do not write — metadata cannot track it.
                        break;
                    }
                } else {
                    activeSpanIndex = curSpans - 1;
                }
            }

            // --- Write PCM to FIFO, only count accepted samples ---
            const float* srcPtrs[32];
            const int chCount = juce::jmin(seg->captureChannels, copyChannels, 32);
            for (int ch = 0; ch < chCount; ++ch)
                srcPtrs[ch] = dryScratch_.getReadPointer(ch);

            const int accepted = seg->fifo.write(srcPtrs, chCount, numSamples);

            if (accepted > 0) {
                auto& span = seg->spans[activeSpanIndex];
                if (activeSpanIndex == curSpans) {
                    span.hostStartSample = hostAbsoluteSample;
                    span.pcmOffsetSamples = spanOffset;
                    span.sampleCount = accepted;
                    seg->numSpans.store(curSpans + 1, std::memory_order_release);
                    if (curSpans == 0) {
                        seg->hostStartSample.store(hostAbsoluteSample, std::memory_order_release);
                        seg->anchored.store(true, std::memory_order_release);
                    }
                } else {
                    span.sampleCount += accepted;
                }
                wroteBlock = true;
            }
        } while (false);

        // Release writer-active.
        seg->writerActive.store(false, std::memory_order_release);

        if (wroteBlock) {
            // Diagnostic: track peak level.
            float blockPeak = 0.0f;
            const int chCount = juce::jmin(seg->captureChannels, copyChannels, 32);
            for (int ch = 0; ch < chCount; ++ch) {
                const float* src = dryScratch_.getReadPointer(ch);
                for (int s = 0; s < numSamples; ++s) {
                    const float a = std::abs(src[s]);
                    if (a > blockPeak) blockPeak = a;
                }
            }
            const float prevPeak = seg->observedPeak.load(std::memory_order_relaxed);
            if (blockPeak > prevPeak)
                seg->observedPeak.store(blockPeak, std::memory_order_relaxed);
        }

        break;  // single-Capturing invariant
    }
}

void CaptureSession::tick()
{
    bool anyChange = false;

    // 1. Pending captures are no longer writable by the audio thread (writer-active
    //    handshake ensures FIFO is released before finalizePendingCapture drains).
    //    No auto-stop: stopCapture is the only way to enter Pending state.
    {
        std::vector<CaptureSegment*> pendingSegments;
        {
            std::lock_guard<std::mutex> lock(mutableMutex_);
            pendingSegments.reserve(mutableSegments_.size());
            for (auto& seg : mutableSegments_) {
                if (seg->state.load(std::memory_order_acquire) == SegmentState::Pending)
                    pendingSegments.push_back(seg.get());
            }
        }
        for (auto* seg : pendingSegments) {
            anyChange = finalizePendingCapture(*seg) || anyChange;
        }
    }

    // 3. Read Processing segments' F0 state directly from content owner.
    {
        std::vector<ContentKey> keysToRender;

        {
            std::lock_guard<std::mutex> lock(mutableMutex_);

            auto it = mutableSegments_.begin();
            while (it != mutableSegments_.end()) {
                auto& seg = **it;
                const auto state = seg.state.load(std::memory_order_acquire);
                if (state != SegmentState::Processing) {
                    ++it;
                    continue;
                }

                const OriginalF0State f0State = seg.content->editable().originalF0State;

                if (f0State == OriginalF0State::Ready) {
                    // F0 Ready 跃迁（含首次观察即 Ready）：提交一次全量渲染，
                    // 等待 onRenderComplete 回调。状态记录+跃迁检测（与插件 UI 侧
                    // lastObservedOriginalF0States_ 同一模式）：tick 每轮只观察比对，
                    // requestFullRender 仅出现在跃迁路径，无"每 tick 触发"残留。
                    // 重复提交会在渲染窗口内（>33ms）把 Running chunk 取消并重启，
                    // 30Hz tick 下渲染永远无法完成。
                    if (seg.lastObservedF0State != OriginalF0State::Ready)
                        keysToRender.push_back(seg.contentKey);
                    anyChange = true;
                }
                // 记录本次观察状态（NotRequested/Extracting/Ready），作为跃迁检测基线。
                // Failed segments transition lifecycle to Failed via commitSegmentF0Result
                // and won't reach here (not Processing).
                seg.lastObservedF0State = f0State;

                ++it;
            }
        }

        if (bindings_.requestFullRender) {
            for (const auto& key : keysToRender)
                bindings_.requestFullRender(key);
        }

    }

    // 4. Reclaim sweep: destroy segments parked in pendingReclaim_ after grace period.
    const int currentTick = tickCounter_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (currentTick % kReclaimGraceTicks == 0) {
        std::vector<std::unique_ptr<CaptureSegment>> toDestroy;
        {
            std::lock_guard<std::mutex> lock(mutableMutex_);
            auto it = pendingReclaim_.begin();
            while (it != pendingReclaim_.end()) {
                if (currentTick - it->queuedTick >= kReclaimGraceTicks) {
                    toDestroy.push_back(std::move(it->segment));
                    it = pendingReclaim_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        // toDestroy goes out of scope here, releasing the segments.
        // If retire callback is wired, retire segments removed by compaction.
        if (bindings_.retireSegment) {
            for (auto& seg : toDestroy) {
                if (seg)
                    bindings_.retireSegment(seg->contentKey);
            }
        }
    }

    if (anyChange)
        publishSegmentsView();
}

// ─── Render pipeline callback ──────────────────────────────────────────────

void CaptureSession::onRenderComplete(ContentKey segmentContentKey)
{
    CaptureSegment* edited = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        for (auto& seg : mutableSegments_) {
            if (seg->contentKey == segmentContentKey) {
                // Only transition Processing → Edited; guard against duplicate calls.
                if (seg->state.load(std::memory_order_acquire) != SegmentState::Processing)
                    return;
                seg->state.store(SegmentState::Edited, std::memory_order_release);
                activeDisplaySegmentId_ = seg->contentKey.objectId;
                edited = seg.get();
                break;
            }
        }
    }
    if (edited == nullptr)
        return;

    AppLogger::log("CaptureSession: segment render complete -> Edited id=" 
        + juce::String(static_cast<juce::int64>(segmentContentKey.objectId)));
    
    runCompaction(*edited);
    publishSegmentsView();
}

void CaptureSession::onRenderFailed(ContentKey segmentContentKey)
{
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        for (auto& seg : mutableSegments_) {
            if (seg->contentKey == segmentContentKey) {
                if (seg->state.load(std::memory_order_acquire) != SegmentState::Processing)
                    return;
                seg->state.store(SegmentState::Failed, std::memory_order_release);
                break;
            }
        }
    }

    AppLogger::warn("CaptureSession: segment render failed -> Failed id="
        + juce::String(static_cast<juce::int64>(segmentContentKey.objectId)));
    publishSegmentsView();
}

bool CaptureSession::commitSegmentF0Result(
    ContentKey segmentContentKey,
    std::shared_ptr<PitchCurve> pitchCurve,
    OriginalF0State state)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);

    CaptureSegment* seg = nullptr;
    for (auto& s : mutableSegments_) {
        if (s->contentKey == segmentContentKey) {
            seg = s.get();
            break;
        }
    }
    if (seg == nullptr || !seg->content)
        return false;

    if (pitchCurve)
        seg->content->applyPitchCurve(std::move(pitchCurve));

    seg->content->applyOriginalF0State(state);

    // F0 analysis failure: mark segment as Failed. Content is preserved (not
    // deleted) and the segment does not block the next capture.
    if (state == OriginalF0State::Failed) {
        seg->state.store(SegmentState::Failed, std::memory_order_release);
        return true;
    }

    return true;
}

bool CaptureSession::applyAutoTuneGeneratedNotes(ContentKey segmentContentKey,
                                                 std::vector<Note> notes,
                                                 std::shared_ptr<PitchCurve> pitchCurve)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto* seg = findMutableSegmentByContentKey(mutableSegments_, segmentContentKey);
    if (seg == nullptr || !seg->content)
        return false;

    seg->content->applyNotes(std::move(notes));
    if (pitchCurve)
        seg->content->applyPitchCurve(std::move(pitchCurve));
    return true;
}

bool CaptureSession::applyNotes(ContentKey segmentContentKey, std::vector<Note> notes)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto* seg = findMutableSegmentByContentKey(mutableSegments_, segmentContentKey);
    if (seg == nullptr || !seg->content)
        return false;

    seg->content->applyNotes(std::move(notes));
    return true;
}

bool CaptureSession::applyVolumeEnvelope(ContentKey segmentContentKey, AutomationLane envelope)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto* seg = findMutableSegmentByContentKey(mutableSegments_, segmentContentKey);
    if (seg == nullptr || !seg->content)
        return false;

    seg->content->applyVolumeEnvelope(std::move(envelope));
    return true;
}

bool CaptureSession::applyNotesAndPitchCurve(ContentKey segmentContentKey,
                                             std::vector<Note> notes,
                                             std::shared_ptr<PitchCurve> pitchCurve)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto* seg = findMutableSegmentByContentKey(mutableSegments_, segmentContentKey);
    if (seg == nullptr || !seg->content || pitchCurve == nullptr)
        return false;

    seg->content->applyNotes(std::move(notes));
    seg->content->applyPitchCurve(std::move(pitchCurve));
    return true;
}

bool CaptureSession::applyPitchCurve(ContentKey segmentContentKey, std::shared_ptr<PitchCurve> pitchCurve)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto* seg = findMutableSegmentByContentKey(mutableSegments_, segmentContentKey);
    if (seg == nullptr || !seg->content || pitchCurve == nullptr)
        return false;

    seg->content->applyPitchCurve(std::move(pitchCurve));
    return true;
}

bool CaptureSession::applyTimeGrid(ContentKey segmentContentKey, std::shared_ptr<const TimeGridSnapshot> grid)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto* seg = findMutableSegmentByContentKey(mutableSegments_, segmentContentKey);
    if (seg == nullptr || !seg->content)
        return false;

    seg->content->applyTimeGrid(std::move(grid));
    return true;
}

bool CaptureSession::applyDetectedKey(ContentKey segmentContentKey, const DetectedKey& detectedKey)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto* seg = findMutableSegmentByContentKey(mutableSegments_, segmentContentKey);
    if (seg == nullptr || !seg->content)
        return false;

    seg->content->applyDetectedKey(detectedKey);
    return true;
}

bool CaptureSession::applyPitchShiftState(ContentKey segmentContentKey, const PitchShiftEditState& state)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto* seg = findMutableSegmentByContentKey(mutableSegments_, segmentContentKey);
    if (seg == nullptr || !seg->content)
        return false;

    return seg->content->applyPitchShiftState(state);
}

// ─── Query ────────────────────────────────────────────────────────────────

SessionState CaptureSession::getGlobalState() const noexcept
{
    auto view = std::atomic_load(&publishedSegments_);

    bool hasCapturing = false, hasProcessing = false;
    for (auto* seg : view->snapshot) {
        const auto s = seg->state.load(std::memory_order_acquire);
        if (s == SegmentState::Capturing) hasCapturing = true;
        else if (s == SegmentState::Pending || s == SegmentState::Processing) hasProcessing = true;
        // Failed segments are intentionally ignored — they do not block next capture.
    }
    if (hasCapturing) return SessionState::HasCapturing;
    if (hasProcessing) return SessionState::HasProcessing;
    return SessionState::Idle;
}

double CaptureSession::getCurrentlyCapturedSeconds() const noexcept
{
    auto view = std::atomic_load(&publishedSegments_);
    for (auto* seg : view->snapshot) {
        if (seg->state.load(std::memory_order_acquire) == SegmentState::Capturing) {
            const int written = seg->fifo.getTotalWrittenSamples();
            return seg->captureSampleRate > 0.0 ? static_cast<double>(written) / seg->captureSampleRate : 0.0;
        }
    }
    return 0.0;
}

size_t CaptureSession::getTotalCapturedBytes() const noexcept
{
    size_t total = 0;
    auto view = std::atomic_load(&publishedSegments_);
    for (auto* seg : view->snapshot) {
        if (seg->content) {
            const auto& editable = seg->content->editable();
            if (editable.audioBuffer) {
                total += static_cast<size_t>(editable.audioBuffer->getNumChannels())
                       * static_cast<size_t>(editable.audioBuffer->getNumSamples())
                       * sizeof(float);
            }
        }
    }
    return total;
}

std::vector<SegmentInfo> CaptureSession::listSegments() const
{
    std::vector<SegmentInfo> result;
    std::lock_guard<std::mutex> lock(mutableMutex_);
    result.reserve(mutableSegments_.size());
    for (const auto& seg : mutableSegments_)
        result.push_back(makeSegmentInfo(*seg));
    return result;
}

std::vector<SegmentInfo> CaptureSession::listEditedSegments() const
{
    std::vector<SegmentInfo> result;
    std::lock_guard<std::mutex> lock(mutableMutex_);
    result.reserve(mutableSegments_.size());
    for (const auto& seg : mutableSegments_) {
        if (seg->state.load(std::memory_order_acquire) == SegmentState::Edited) {
            result.push_back(makeSegmentInfo(*seg));
        }
    }
    return result;
}

bool CaptureSession::resolveDisplaySegment(double hostTimeSeconds, SegmentInfo& out) const
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    for (auto it = mutableSegments_.rbegin(); it != mutableSegments_.rend(); ++it) {
        const auto& seg = **it;
        if (seg.state.load(std::memory_order_acquire) == SegmentState::Edited
            && seg.containsTime(hostTimeSeconds)) {
            out = makeSegmentInfo(seg);
            return true;
        }
    }

    for (const auto& seg : mutableSegments_) {
        if (seg->contentKey.objectId == activeDisplaySegmentId_
            && seg->state.load(std::memory_order_acquire) == SegmentState::Edited) {
            out = makeSegmentInfo(*seg);
            return true;
        }
    }

    return false;
}

// ─── Persistence (delegated to CapturePersistence) ─────────────────────────

juce::MemoryBlock CaptureSession::serialize() const
{
    return CapturePersistence::serialize(*this);
}

bool CaptureSession::deserialize(const juce::MemoryBlock& block)
{
    return CapturePersistence::deserialize(*this, block);
}

// ─── Test hooks ────────────────────────────────────────────────────────────

uint64_t CaptureSession::testInjectEditedSegment(double T_start,
                                                  double durationSeconds,
                                                  uint64_t segmentId,
                                                  std::shared_ptr<juce::AudioBuffer<float>> pcm)
{
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        auto seg = std::make_unique<CaptureSegment>();
        id = segmentId > 0 ? segmentId : nextId();
        seg->contentKey = ContentKey{DomainKind::RegularVST3Capture, id, 0};
        seg->creationOrder = id;
        seg->captureSampleRate = currentSampleRate_;
        seg->captureChannels = pcm ? pcm->getNumChannels() : 2;
        seg->T_start.store(T_start, std::memory_order_release);
        seg->anchored.store(true, std::memory_order_release);
        seg->hostStartSample.store(static_cast<int64_t>(T_start * currentSampleRate_), std::memory_order_release);
        seg->hostSampleCount.store(static_cast<int64_t>(durationSeconds * currentSampleRate_), std::memory_order_release);
        seg->durationSeconds = durationSeconds;
        seg->content = std::make_unique<CaptureSegmentContent>(id);
        if (pcm)
            seg->content->applyAudioBuffer(*pcm, currentSampleRate_);
        seg->state.store(SegmentState::Edited, std::memory_order_release);
        activeDisplaySegmentId_ = id;
        mutableSegments_.push_back(std::move(seg));
    }
    publishSegmentsView();
    return id;
}

// ─── Private helpers ───────────────────────────────────────────────────────

uint64_t CaptureSession::testInjectProcessingSegment(double T_start,
                                                      double durationSeconds,
                                                      uint64_t segmentId,
                                                      std::shared_ptr<juce::AudioBuffer<float>> pcm,
                                                      double sampleRate)
{
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        auto seg = std::make_unique<CaptureSegment>();
        id = segmentId > 0 ? segmentId : nextId();
        seg->contentKey = ContentKey{DomainKind::RegularVST3Capture, id, 0};
        seg->creationOrder = id;
        seg->captureSampleRate = sampleRate;
        seg->captureChannels = pcm ? pcm->getNumChannels() : 1;
        seg->T_start.store(T_start, std::memory_order_release);
        seg->anchored.store(true, std::memory_order_release);
        seg->hostStartSample.store(static_cast<int64_t>(T_start * sampleRate), std::memory_order_release);
        seg->hostSampleCount.store(static_cast<int64_t>(durationSeconds * sampleRate), std::memory_order_release);
        seg->durationSeconds = durationSeconds;
        seg->content = std::make_unique<CaptureSegmentContent>(id);
        if (pcm)
            seg->content->applyAudioBuffer(*pcm, sampleRate);
        seg->content->applyOriginalF0State(OriginalF0State::Extracting);
        seg->state.store(SegmentState::Processing, std::memory_order_release);
        mutableSegments_.push_back(std::move(seg));
    }
    publishSegmentsView();
    return id;
}

void CaptureSession::publishSegmentsView()
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto view = std::make_shared<SegmentsView>();
    view->snapshot.reserve(mutableSegments_.size());
    for (const auto& seg : mutableSegments_)
        view->snapshot.push_back(seg.get());
    std::atomic_store(&publishedSegments_, std::shared_ptr<const SegmentsView>(view));
}

void CaptureSession::queueForReclaimLocked(std::unique_ptr<CaptureSegment> segment)
{
    jassert(segment != nullptr);

    ReclaimEntry entry;
    entry.segment = std::move(segment);
    entry.queuedTick = tickCounter_.load(std::memory_order_acquire);
    pendingReclaim_.push_back(std::move(entry));
}

bool CaptureSession::finalizePendingCapture(CaptureSegment& pending)
{
    if (pending.state.load(std::memory_order_acquire) != SegmentState::Pending)
        return false;

    // Atomic writer-active handshake: only drain after audio thread has released.
    if (pending.writerActive.load(std::memory_order_acquire))
        return false;

    const int written = pending.fifo.getTotalWrittenSamples();
    const float observedPeak = pending.observedPeak.load(std::memory_order_relaxed);
    const int numSpans = pending.numSpans.load(std::memory_order_acquire);
    AppLogger::log("CaptureSession::finalizePendingCapture id="
                   + juce::String(static_cast<juce::int64>(pending.contentKey.objectId))
                   + " writtenSamples=" + juce::String(written)
                   + " spans=" + juce::String(numSpans)
                   + " durationSec=" + juce::String(written / juce::jmax(1.0, pending.captureSampleRate), 3)
                   + " observedPeak=" + juce::String(observedPeak, 6)
                   + (observedPeak < 1e-4f ? " [WARNING: near-silent buffer; host may not be routing audio to plugin]" : ""));

    auto dropPending = [this, id = pending.contentKey.objectId]() {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        auto it = std::find_if(mutableSegments_.begin(), mutableSegments_.end(),
                               [id](const std::unique_ptr<CaptureSegment>& seg) {
                                   return seg && seg->contentKey.objectId == id;
                               });
        if (it != mutableSegments_.end()) {
            queueForReclaimLocked(std::move(*it));
            mutableSegments_.erase(it);
        }
    };

    if (written <= 0 || numSpans <= 0) {
        dropPending();
        return true;
    }

    // Drain entire FIFO once into a contiguous buffer.
    auto fullPcm = std::make_shared<juce::AudioBuffer<float>>(pending.captureChannels, written);
    const int drained = pending.fifo.drainAll(*fullPcm);
    pending.fifo.release();
    if (drained <= 0) {
        dropPending();
        return true;
    }

    if (!pending.content || !bindings_.publishPlaybackSource) {
        dropPending();
        return true;
    }

    // Local helper: extract a sub-range from the drained PCM buffer.
    auto extractSubRange = [](const std::shared_ptr<juce::AudioBuffer<float>>& src,
                               int start, int count)
        -> std::shared_ptr<juce::AudioBuffer<float>>
    {
        if (!src || count <= 0)
            return {};
        const int clampedStart = juce::jmax(0, start);
        const int clampedCount = juce::jmin(count, src->getNumSamples() - clampedStart);
        if (clampedCount <= 0)
            return {};
        auto result = std::make_shared<juce::AudioBuffer<float>>(src->getNumChannels(), clampedCount);
        for (int ch = 0; ch < result->getNumChannels(); ++ch)
            result->copyFrom(ch, 0, *src, ch, clampedStart, clampedCount);
        return result;
    };

    bool anySpanPublished = false;
    bool reusedPendingSegment = false;

    for (int i = 0; i < numSpans; ++i) {
        const auto& span = pending.spans[i];
        if (span.sampleCount <= 0)
            continue;

        auto spanPcm = extractSubRange(fullPcm, span.pcmOffsetSamples, span.sampleCount);
        if (!spanPcm || spanPcm->getNumSamples() == 0)
            continue;

        if (!reusedPendingSegment) {
            // First non-empty span: reuse the pending segment (preserves original ContentKey).
            // Set authoritative absolute sample range from span metadata.
            pending.hostStartSample.store(span.hostStartSample, std::memory_order_release);
            pending.hostSampleCount.store(static_cast<int64_t>(span.sampleCount), std::memory_order_release);
            // T_start/durationSeconds: UI/presentation only, computed from absolute samples.
            pending.T_start.store(static_cast<double>(span.hostStartSample) / pending.captureSampleRate,
                                  std::memory_order_release);
            pending.durationSeconds = static_cast<double>(span.sampleCount) / pending.captureSampleRate;

            pending.content->applyAudioBuffer(*spanPcm, pending.captureSampleRate);
            pending.content->applyOriginalF0State(OriginalF0State::Extracting);

            const auto snap = pending.content->snapshotContent();
            bindings_.publishPlaybackSource(pending.contentKey, snap->audioBuffer, snap->audioSampleRate);
            pending.state.store(SegmentState::Processing, std::memory_order_release);

            if (bindings_.refreshSegment)
                bindings_.refreshSegment(pending.contentKey);

            reusedPendingSegment = true;
            anySpanPublished = true;
        } else {
            // Subsequent non-empty spans: create a new CaptureSegment with a new id.
            auto newSeg = std::make_unique<CaptureSegment>();
            const uint64_t newId = nextId();
            newSeg->contentKey = ContentKey{DomainKind::RegularVST3Capture, newId, 0};
            newSeg->creationOrder = newId;
            newSeg->captureSampleRate = pending.captureSampleRate;
            newSeg->captureChannels = pending.captureChannels;
            // Set authoritative absolute sample range from span metadata.
            newSeg->hostStartSample.store(span.hostStartSample, std::memory_order_release);
            newSeg->hostSampleCount.store(static_cast<int64_t>(span.sampleCount), std::memory_order_release);
            newSeg->anchored.store(true, std::memory_order_release);
            // T_start/durationSeconds: UI/presentation only, computed from absolute samples.
            newSeg->T_start.store(static_cast<double>(span.hostStartSample) / pending.captureSampleRate,
                                  std::memory_order_release);
            newSeg->durationSeconds = static_cast<double>(span.sampleCount) / pending.captureSampleRate;
            newSeg->content = std::make_unique<OpenTune::CaptureSegmentContent>(newId);
            newSeg->content->applyAudioBuffer(*spanPcm, pending.captureSampleRate);
            newSeg->content->applyOriginalF0State(OriginalF0State::Extracting);
            newSeg->state.store(SegmentState::Processing, std::memory_order_release);

            const auto snap = newSeg->content->snapshotContent();
            bindings_.publishPlaybackSource(newSeg->contentKey, snap->audioBuffer, snap->audioSampleRate);
            const ContentKey newKey = newSeg->contentKey;

            {
                std::lock_guard<std::mutex> lock(mutableMutex_);
                mutableSegments_.push_back(std::move(newSeg));
            }

            if (bindings_.refreshSegment)
                bindings_.refreshSegment(newKey);

            anySpanPublished = true;
        }
    }

    if (!anySpanPublished) {
        dropPending();
        return true;
    }

    return true;
}

void CaptureSession::runCompaction(const CaptureSegment& newlyEdited)
{
    std::vector<std::unique_ptr<CaptureSegment>> removed;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        removed = CaptureCompactor::removeFullyCovered(mutableSegments_, newlyEdited);

        // Move removed segments to pendingReclaim_ for grace-period reclaim.
        for (auto& r : removed)
            queueForReclaimLocked(std::move(r));
    }
}

uint64_t CaptureSession::nextId() noexcept
{
    return ++idCounter_;
}

void CaptureSession::applyNumericGuardForTest(juce::AudioBuffer<float>& buffer) noexcept
{
    const int channels = buffer.getNumChannels();
    const int samples  = buffer.getNumSamples();
    for (int ch = 0; ch < channels; ++ch) {
        float* p = buffer.getWritePointer(ch);
        for (int s = 0; s < samples; ++s) {
            if (!std::isfinite(p[s]))
                p[s] = 0.0f;
        }
    }
}

CaptureSegment* CaptureSession::findSegmentByContentKey(const ContentKey& key) const
{
    if (key.domainKind != DomainKind::RegularVST3Capture) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutableMutex_);
    for (const auto& seg : mutableSegments_) {
        if (seg->contentKey.objectId == key.objectId) {
            return seg.get();
        }
    }
    return nullptr;
}

}  // namespace OpenTune::Capture
