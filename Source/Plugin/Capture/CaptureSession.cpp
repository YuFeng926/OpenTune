
#include "CaptureSession.h"

#include "CapturePersistence.h"
#include "../../Content/EditableContentSnapshot.h"
#include "../../Utils/AppLogger.h"
#include "../../Utils/ChannelLayoutLogger.h"
#include "../../Utils/PitchCurve.h"
#include "../../Utils/TimeCoordinate.h"
#include "../../Utils/CaptureContentSlice.h"

#include <algorithm>

namespace OpenTune::Capture {

namespace {
    constexpr double kMaxSegmentSeconds = 600.0;  // host window span per take
    constexpr size_t kSpanMetadataBudgetBytes = 1024 * 1024;  // per-take span metadata budget

    class SegmentViewReaderGuard
    {
    public:
        explicit SegmentViewReaderGuard(std::atomic<int>& readers) noexcept : readers_(readers)
        {
            readers_.fetch_add(1, std::memory_order_seq_cst);
        }

        ~SegmentViewReaderGuard()
        {
            readers_.fetch_sub(1, std::memory_order_seq_cst);
        }

        SegmentViewReaderGuard(const SegmentViewReaderGuard&) = delete;
        SegmentViewReaderGuard& operator=(const SegmentViewReaderGuard&) = delete;

    private:
        std::atomic<int>& readers_;
    };

    /** One merged host range produced by finalize. */
    struct HostInterval
    {
        int64_t start = 0;
        int64_t end = 0;
    };

    SegmentInfo makeSegmentInfo(const CaptureSegment& segment)

    {
        SegmentInfo info;
        info.contentKey = segment.contentKey;
        info.T_start = segment.T_start.load(std::memory_order_acquire);
        info.durationSeconds = segment.durationSeconds;
        info.state = segment.getState();
        return info;
    }

    CaptureSegment* findMutableSegmentByContentKey(
        std::vector<std::shared_ptr<CaptureSegment>>& segments,
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
}

CaptureSession::~CaptureSession() = default;

// ─── Lifecycle ────────────────────────────────────────────────────────────

void CaptureSession::prepareToPlay(double sampleRate, int maxBlockSize, int hostInputChannels)
{
    currentSampleRate_ = sampleRate;
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
    bool rejected = false;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);

        // A take that is still capturing or waiting to finalize blocks a new arm.
        // Processing may overlap the next take so rendering does not stall recording.
        for (const auto& seg : mutableSegments_) {
            const auto s = seg->getState();
            if (s == SegmentState::Capturing || s == SegmentState::Pending) {
                AppLogger::warn("CaptureSession::armNewCapture rejected: existing segment state="
                                + juce::String(static_cast<int>(s)));
                rejected = true;
                break;
            }
        }

        if (!rejected) {
            auto seg = std::make_shared<CaptureSegment>();
            const int ch = captureChannels_.load(std::memory_order_acquire);
            const uint64_t id = nextId();
            seg->contentKey = ContentKey{DomainKind::RegularVST3Capture, id, 0};
            seg->creationOrder = id;
            seg->captureSampleRate = currentSampleRate_;
            seg->captureChannels = ch;
            seg->capacitySamples = static_cast<int>(
                TimeCoordinate::secondsToSamplesCeil(kMaxSegmentSeconds, currentSampleRate_));
            seg->window.reserve(seg->captureChannels, seg->capacitySamples);
            // Fixed per-take span metadata budget (1 MiB of CapturedSpan entries). The
            // audio thread never grows spans or allocates; when the budget is spent it
            // latches spanBudgetExhausted and stops writing — the take is reclaimed whole.
            const int spanCapacityOverride = testSpanCapacityOverride_.load(std::memory_order_acquire);
            const int spanCapacity = spanCapacityOverride > 0
                ? spanCapacityOverride
                : juce::jmax(1, static_cast<int>(
                      kSpanMetadataBudgetBytes / sizeof(CapturedSpan)));
            seg->spans.resize(spanCapacity);
            seg->content = std::make_unique<OpenTune::CaptureSegmentContent>(id);

            AppLogger::log("CaptureSession::armNewCapture id=" + juce::String(static_cast<juce::int64>(id))
                         + " ch=" + juce::String(ch) + " sr=" + juce::String(currentSampleRate_, 1));

            ChannelLayoutLog::logSegmentArm(static_cast<juce::int64>(id), seg->captureChannels);

            mutableSegments_.push_back(std::move(seg));
        }
    }
    if (!rejected)
        publishSegmentsView();
    return !rejected;
}


void CaptureSession::stopCapture()
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    for (auto& seg : mutableSegments_) {
        if (seg->getState() != SegmentState::Capturing)
            continue;
        // A span-budget-exhausted take is reclaimed whole by tick(); it must
        // never enter Pending and publish partial data as a success.
        if (seg->spanBudgetExhausted.load(std::memory_order_acquire))
            continue;
        // Fails while the audio thread holds the writer bit: latch the request so
        // tick() retries the transition as soon as the claim is released.
        if (seg->tryStopCapturing()) {
            seg->stopRequested.store(false, std::memory_order_release);
            AppLogger::log("CaptureSession::stopCapture id="
                           + juce::String(static_cast<juce::int64>(seg->contentKey.objectId)));
        } else {
            seg->stopRequested.store(true, std::memory_order_release);
        }
    }
}

// ─── Audio thread ─────────────────────────────────────────────────────────

void CaptureSession::processBlock(juce::AudioBuffer<float>& buffer,
                                  int64_t hostAbsoluteSample,
                                  double hostSampleRate,
                                  bool isPlaying) noexcept
{
    SegmentViewReaderGuard readerGuard(activeSegmentReaders_);

    if (hostAbsoluteSample >= 0)
        lastHostPlaying_.store(isPlaying, std::memory_order_release);

    // Audio thread reads the raw pointer only: no shared_ptr operations, no owners.
    // seq_cst keeps the pointer load in the same total order as the reader counter
    // and the publishing exchange, so a reader that entered after the reclaim check
    // observes the new view.
    const SegmentsView* view = publishedSegments_.load(std::memory_order_seq_cst);
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

    // Step 2: replace every overlapping Edited segment in creation order.
    // Later segments run last, so later reads overwrite earlier reads. Each call
    // covers only the intersection and leaves the rest of the host block intact.
    if (isPlaying && bindings_.replaceWithRendered) {
        const int64_t blockStart = hostAbsoluteSample;
        const int64_t blockEnd = hostAbsoluteSample + static_cast<int64_t>(numSamples);
        for (auto* seg : view->snapshot) {
            if (seg->getState() != SegmentState::Edited)
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

    if (!isPlaying)
        return;

    // Step 3: write the dry block into the Capturing take's host window and append
    // its span. The audio thread never queries existing coverage: explicit recording
    // always writes, and a rewind overwrites the earlier pass sample by sample.
    for (auto* seg : view->snapshot) {
        if (seg->getState() != SegmentState::Capturing)
            continue;

        if (seg->spanBudgetExhausted.load(std::memory_order_acquire)
            || seg->windowExhausted.load(std::memory_order_acquire)) {
            break;  // latched: tick() ends the take; audio thread writes nothing
        }

        // CAS writer bit: only one writer (audio thread) at a time.
        if (!seg->tryAcquireWriter())
            break;

        // Re-check state after CAS: if stopCapture already set Pending, do not write.
        if (seg->getState() != SegmentState::Capturing) {
            seg->releaseWriter();
            break;
        }

        const int64_t blockStart = hostAbsoluteSample;
        const int64_t blockEnd = blockStart + static_cast<int64_t>(numSamples);
        const int64_t capacity = static_cast<int64_t>(seg->capacitySamples);
        const int64_t newMin = seg->hasWritten ? std::min(seg->minHostSample, blockStart) : blockStart;
        const int64_t newMax = seg->hasWritten ? std::max(seg->maxHostSampleExclusive, blockEnd) : blockEnd;

        if (newMax - newMin > capacity) {
            // The take already spans the whole window. Stop writing; tick() ends the
            // take and finalize publishes what was written before the overflow.
            seg->windowExhausted.store(true, std::memory_order_release);
            seg->releaseWriter();
            break;
        }

        const int curSpans = seg->numSpans.load(std::memory_order_relaxed);
        bool extendLastSpan = false;
        if (curSpans > 0) {
            const auto& lastSpan = seg->spans[static_cast<size_t>(curSpans - 1)];
            extendLastSpan = blockStart
                == lastSpan.hostStartSample + static_cast<int64_t>(lastSpan.sampleCount);
        }
        if (!extendLastSpan && curSpans >= static_cast<int>(seg->spans.size())) {
            // Span metadata budget is spent: the whole unfinished take is dropped.
            seg->spanBudgetExhausted.store(true, std::memory_order_release);
            seg->releaseWriter();
            break;
        }

        const int chCount = juce::jmin(seg->captureChannels, copyChannels, 32);
        const float* srcPtrs[32];
        for (int ch = 0; ch < chCount; ++ch)
            srcPtrs[ch] = dryScratch_.getReadPointer(ch);

        seg->window.writeAt(seg->windowOffset(blockStart), srcPtrs, chCount, numSamples);

        if (extendLastSpan) {
            seg->spans[static_cast<size_t>(curSpans - 1)].sampleCount
                += static_cast<int32_t>(numSamples);
        } else {
            seg->spans[static_cast<size_t>(curSpans)] = {
                blockStart, static_cast<int32_t>(numSamples) };
            seg->numSpans.store(curSpans + 1, std::memory_order_release);
        }

        if (!seg->hasWritten) {
            seg->anchorHostSample = blockStart;
            seg->hasWritten = true;
        }
        seg->minHostSample = newMin;
        seg->maxHostSampleExclusive = newMax;

        float blockPeak = 0.0f;
        for (int ch = 0; ch < chCount; ++ch) {
            const float* src = srcPtrs[ch];
            for (int s = 0; s < numSamples; ++s) {
                const float a = std::abs(src[s]);
                if (a > blockPeak)
                    blockPeak = a;
            }
        }
        if (blockPeak > seg->observedPeak.load(std::memory_order_relaxed))
            seg->observedPeak.store(blockPeak, std::memory_order_relaxed);

        seg->releaseWriter();

        break;  // single-Capturing invariant
    }
}

void CaptureSession::tick()
{
    bool anyChange = false;

    // 0. End the take on a latched user stop, host window overflow, or host stop.
    //    A take that never wrote a sample stays armed and waits for playback.
    //    tryStopCapturing fails while the audio thread holds the writer bit; the
    //    latched conditions persist, so the next tick retries the transition.
    //    State changes publish through the atomic word, so this step needs no view rebuild.
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        for (auto& seg : mutableSegments_) {
            if (seg->getState() != SegmentState::Capturing)
                continue;
            const bool userStop = seg->stopRequested.load(std::memory_order_acquire);
            const bool windowFull = seg->windowExhausted.load(std::memory_order_acquire);
            const bool hostStopped = !lastHostPlaying_.load(std::memory_order_acquire)
                && seg->numSpans.load(std::memory_order_acquire) > 0;
            if (!userStop && !windowFull && !hostStopped)
                continue;
            if (!seg->tryStopCapturing())
                continue;
            seg->stopRequested.store(false, std::memory_order_release);
            if (windowFull)
                AppLogger::warn("CaptureSession: host window exhausted, ending take id="
                                + juce::String(static_cast<juce::int64>(seg->contentKey.objectId))
                                + " spans=" + juce::String(seg->numSpans.load(std::memory_order_acquire)));
            break;
        }
    }

    // 1. Pending captures are no longer writable by the audio thread (writer-active

    //    handshake ensures the window is released before finalizePendingCapture reads).
    {
        std::vector<CaptureSegment*> pendingSegments;
        {
            std::lock_guard<std::mutex> lock(mutableMutex_);
            pendingSegments.reserve(mutableSegments_.size());
            for (auto& seg : mutableSegments_) {
                if (seg->getState() == SegmentState::Pending)
                    pendingSegments.push_back(seg.get());
            }
        }
        for (auto* seg : pendingSegments) {
            anyChange = finalizePendingCapture(*seg) || anyChange;
        }
    }

    // 2. Span metadata budget exhausted mid-take: the audio thread latched the flag
    //    and stopped writing. Reclaim the whole unfinished take — partial data must
    //    never be published as a successful segment. One explicit warning per take.
    {
        std::vector<CaptureSegment*> exhaustedSegments;
        {
            std::lock_guard<std::mutex> lock(mutableMutex_);
            for (auto& seg : mutableSegments_) {
                const auto state = seg->getState();
                if ((state == SegmentState::Capturing || state == SegmentState::Pending)
                    && seg->spanBudgetExhausted.load(std::memory_order_acquire)) {
                    exhaustedSegments.push_back(seg.get());
                }
            }
        }
        for (auto* seg : exhaustedSegments) {
            if (seg->isWriterActive())
                continue;  // audio thread still inside this block; retry next tick

            const int64_t writtenSamples = seg->hasWritten
                ? seg->maxHostSampleExclusive - seg->minHostSample
                : 0;
            AppLogger::warn("CaptureSession: span metadata budget exhausted, dropping unfinished take id="
                            + juce::String(static_cast<juce::int64>(seg->contentKey.objectId))
                            + " hostSamples=" + juce::String(writtenSamples)
                            + " spans=" + juce::String(seg->numSpans.load(std::memory_order_acquire)));
            {
                std::lock_guard<std::mutex> lock(mutableMutex_);
                auto it = std::find_if(mutableSegments_.begin(), mutableSegments_.end(),
                                       [seg](const std::shared_ptr<CaptureSegment>& s) {
                                           return s.get() == seg;
                                       });
                if (it != mutableSegments_.end())
                    mutableSegments_.erase(it);
            }
            anyChange = true;
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
                const auto state = seg.getState();
                if (state != SegmentState::Processing) {
                    ++it;
                    continue;
                }

                const auto& analysis = seg.content->content().analysis;
                const OriginalF0State f0State = analysis.originalF0State;
                // F0 render gate 以 owner 实际数据为准（等价 hasUsableOriginalF0），
                // 不以 originalF0State 枚举单独放行。
                const bool hasUsableOriginalF0 = analysis.pitchCurve != nullptr
                    && analysis.pitchCurve->hasUsableOriginalF0();

                if (hasUsableOriginalF0) {
                    // F0 可用跃迁（含首次观察即可用）：提交一次全量渲染，
                    // 等待 onRenderComplete 回调。状态记录+跃迁检测（与插件 UI 侧
                    // lastObservedOriginalF0States_ 同一模式）：tick 每轮只观察比对，
                    // requestFullRender 仅出现在跃迁路径，无"每 tick 触发"残留。
                    // 重复提交会在渲染窗口内（>33ms）把 Running chunk 取消并重启，
                    // 30Hz tick 下渲染永远无法完成。
                    if (seg.lastObservedF0State != OriginalF0State::Ready)
                        keysToRender.push_back(seg.contentKey);
                }
                // 记录本次观察基线：render gate 以可用 F0 数据为准，可用即归一化为 Ready。
                seg.lastObservedF0State = hasUsableOriginalF0 ? OriginalF0State::Ready : f0State;

                ++it;
            }
        }

        if (bindings_.requestFullRender) {
            for (const auto& key : keysToRender)
                bindings_.requestFullRender(key);
        }

    }

    // 4. Reclaim sweep: retire removed segments only after all audio readers left
    //    the retired views, and only for keys absent from the currently published set.
    if (activeSegmentReaders_.load(std::memory_order_seq_cst) == 0) {
        retiredViews_.clear();
        if (bindings_.retireSegment) {
            for (const auto& key : pendingRetireKeys_)
                bindings_.retireSegment(key);
        }
        pendingRetireKeys_.clear();
    }

    if (anyChange)
        publishSegmentsView();
}

// ─── Render pipeline callback ──────────────────────────────────────────────

void CaptureSession::onRenderComplete(ContentKey segmentContentKey)
{
    bool edited = false;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        for (auto& seg : mutableSegments_) {
            if (seg->contentKey == segmentContentKey) {
                // Only transition Processing → Edited; guard against duplicate calls.
                if (seg->getState() != SegmentState::Processing)
                    return;
                seg->setState(SegmentState::Edited);
                activeDisplaySegmentId_ = seg->contentKey.objectId;
                edited = true;
                break;
            }
        }
    }
    if (!edited)
        return;

    AppLogger::log("CaptureSession: segment render complete -> Edited id=" 
        + juce::String(static_cast<juce::int64>(segmentContentKey.objectId)));
}

void CaptureSession::onRenderFailed(ContentKey segmentContentKey, const juce::String& reason)
{
    bool failed = false;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        for (auto& seg : mutableSegments_) {
            if (seg->contentKey == segmentContentKey) {
                if (seg->getState() != SegmentState::Processing)
                    return;
                seg->setState(SegmentState::Failed);
                failed = true;
                break;
            }
        }
    }
    if (!failed)
        return;

    AppLogger::warn("CaptureSession: segment render failed -> Failed id="
        + juce::String(static_cast<juce::int64>(segmentContentKey.objectId))
        + " reason=" + reason);
}

bool CaptureSession::commitSegmentF0Result(
    ContentKey segmentContentKey,
    std::shared_ptr<PitchCurve> pitchCurve,
    OriginalF0State state,
    std::string failureReason)
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

    if (state == OriginalF0State::Ready && pitchCurve)
    {
        // 成功提交：applyOriginalF0 一次性写入 curve + Ready + analysisRevision，
        // 不再随后 applyOriginalF0State(Ready)，避免双 contentRevision bump。
        seg->content->applyOriginalF0(std::move(pitchCurve));
    }
    else
    {
        // 失败或无曲线：保持原状态机语义，普通曲线替换不伪造 F0 Ready。
        if (pitchCurve)
            seg->content->applyPitchCurve(std::move(pitchCurve));

        seg->content->applyOriginalF0State(state);
    }

    // F0 analysis failure: mark segment as Failed. Content is preserved (not
    // deleted) and the segment does not block the next capture.
    if (state == OriginalF0State::Failed) {
        if (failureReason.empty())
            failureReason = "OriginalF0 extraction failed without an error detail";
        seg->content->applyOriginalF0FailureReason(std::move(failureReason));
        seg->setState(SegmentState::Failed);
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

bool CaptureSession::applyExternalEdit(ContentKey segmentContentKey,
                                       std::vector<Note> notes,
                                       std::shared_ptr<PitchCurve> pitchCurve,
                                       AutomationLane envelope,
                                       PitchShiftSettings pitchShift)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto* seg = findMutableSegmentByContentKey(mutableSegments_, segmentContentKey);
    // Owner lookup must complete before any mutation: no partial state on miss.
    if (seg == nullptr || !seg->content)
        return false;

    auto* content = seg->content.get();

    if (pitchCurve != nullptr) {
        // Usable Original F0 → applyOriginalF0 (curve + Ready, single revision bump),
        // otherwise plain applyPitchCurve. Then apply the composed pitch-shift state
        // (notes + settings + curve correction segments).
        if (pitchCurve->hasUsableOriginalF0())
            content->applyOriginalF0(std::move(pitchCurve));
        else
            content->applyPitchCurve(std::move(pitchCurve));

        PitchShiftEditState shift;
        shift.settings = std::move(pitchShift);
        shift.notes = std::move(notes);
        shift.segments = content->content().analysis.pitchCurve->getSnapshot()->getCorrectionSegments();
        content->applyPitchShiftState(shift);
    } else {
        // No curve: replace notes only.
        content->applyNotes(std::move(notes));
    }

    // Envelope last: note outputGainDb is derived from the new envelope.
    content->applyVolumeEnvelope(std::move(envelope));
    return true;
}

// ─── Query ────────────────────────────────────────────────────────────────

SessionState CaptureSession::getGlobalState() const noexcept
{
    std::lock_guard<std::mutex> lock(mutableMutex_);

    bool hasCapturing = false, hasProcessing = false;
    for (const auto& seg : mutableSegments_) {
        const auto s = seg->getState();
        if (s == SegmentState::Capturing) hasCapturing = true;
        else if (s == SegmentState::Pending || s == SegmentState::Processing) hasProcessing = true;
        // Failed segments are intentionally ignored — they do not block next capture.
    }
    if (hasCapturing) return SessionState::HasCapturing;
    if (hasProcessing) return SessionState::HasProcessing;
    return SessionState::Idle;
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
        if (seg->getState() == SegmentState::Edited) {
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
        if (seg.getState() == SegmentState::Edited
            && seg.containsTime(hostTimeSeconds)) {
            out = makeSegmentInfo(seg);
            return true;
        }
    }

    for (const auto& seg : mutableSegments_) {
        if (seg->contentKey.objectId == activeDisplaySegmentId_
            && seg->getState() == SegmentState::Edited) {
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
                                                  std::shared_ptr<juce::AudioBuffer<float>> pcm,
                                                  double sampleRate)
{
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        auto seg = std::make_shared<CaptureSegment>();
        id = segmentId > 0 ? segmentId : nextId();
        if (id > idCounter_)
            idCounter_ = id;
        seg->contentKey = ContentKey{DomainKind::RegularVST3Capture, id, 0};
        seg->creationOrder = id;
        seg->captureSampleRate = sampleRate;
        seg->captureChannels = pcm ? pcm->getNumChannels() : 2;
        seg->T_start.store(T_start, std::memory_order_release);
        seg->hostStartSample.store(TimeCoordinate::secondsToSamples(T_start, sampleRate), std::memory_order_release);
        seg->hostSampleCount.store(TimeCoordinate::secondsToSamples(durationSeconds, sampleRate), std::memory_order_release);
        seg->durationSeconds = durationSeconds;
        seg->content = std::make_unique<CaptureSegmentContent>(id);
        if (pcm)
            seg->content->applyAudioBuffer(*pcm, sampleRate);
        seg->setState(SegmentState::Edited);
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
        auto seg = std::make_shared<CaptureSegment>();
        id = segmentId > 0 ? segmentId : nextId();
        if (id > idCounter_)
            idCounter_ = id;
        seg->contentKey = ContentKey{DomainKind::RegularVST3Capture, id, 0};
        seg->creationOrder = id;
        seg->captureSampleRate = sampleRate;
        seg->captureChannels = pcm ? pcm->getNumChannels() : 1;
        seg->T_start.store(T_start, std::memory_order_release);
        seg->hostStartSample.store(TimeCoordinate::secondsToSamples(T_start, sampleRate), std::memory_order_release);
        seg->hostSampleCount.store(TimeCoordinate::secondsToSamples(durationSeconds, sampleRate), std::memory_order_release);
        seg->durationSeconds = durationSeconds;
        seg->content = std::make_unique<CaptureSegmentContent>(id);
        if (pcm)
            seg->content->applyAudioBuffer(*pcm, sampleRate);
        seg->content->applyOriginalF0State(OriginalF0State::Extracting);
        seg->setState(SegmentState::Processing);
        mutableSegments_.push_back(std::move(seg));
    }
    publishSegmentsView();
    return id;
}

uint64_t CaptureSession::testInjectSegment(OpenTune::ContentState state,
                                           SegmentState segmentState,
                                           double T_start,
                                           double durationSeconds,
                                           uint64_t segmentId,
                                           double sampleRate)
{
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        auto seg = std::make_shared<CaptureSegment>();
        id = segmentId > 0 ? segmentId : nextId();
        if (id > idCounter_)
            idCounter_ = id;
        seg->contentKey = ContentKey{DomainKind::RegularVST3Capture, id, 0};
        seg->creationOrder = id;
        seg->captureSampleRate = sampleRate;
        seg->captureChannels = state.audioBuffer ? state.audioBuffer->getNumChannels() : 1;
        seg->T_start.store(T_start, std::memory_order_release);
        seg->hostStartSample.store(TimeCoordinate::secondsToSamples(T_start, sampleRate), std::memory_order_release);
        seg->hostSampleCount.store(TimeCoordinate::secondsToSamples(durationSeconds, sampleRate), std::memory_order_release);
        seg->durationSeconds = durationSeconds;
        seg->content = std::make_unique<CaptureSegmentContent>(id);
        seg->content->applyContentState(std::move(state));
        seg->setState(segmentState);
        activeDisplaySegmentId_ = id;
        mutableSegments_.push_back(std::move(seg));
    }
    publishSegmentsView();
    return id;
}

void CaptureSession::publishSegmentsView()
{
    // Message-thread only. Build the new view under the mutex, swap the published
    // pointer atomically, then move the replaced view into retiredViews_.
    auto view = std::make_unique<SegmentsView>();
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        view->snapshot.reserve(mutableSegments_.size());
        view->owners.reserve(mutableSegments_.size());
        for (const auto& seg : mutableSegments_) {
            view->owners.push_back(seg);
            view->snapshot.push_back(seg.get());
        }

        std::vector<ContentKey> nextKeys;
        nextKeys.reserve(mutableSegments_.size());
        for (const auto& seg : mutableSegments_)
            nextKeys.push_back(seg->contentKey);

        // Keys removed by this publication become retire candidates; keys that
        // reappear in the new view are no longer pending.
        std::vector<ContentKey> nextPending;
        nextPending.reserve(pendingRetireKeys_.size() + nextKeys.size());
        for (const auto& key : pendingRetireKeys_) {
            if (std::find(nextKeys.begin(), nextKeys.end(), key) == nextKeys.end())
                nextPending.push_back(key);
        }
        for (const auto& key : publishedKeys_) {
            if (std::find(nextKeys.begin(), nextKeys.end(), key) == nextKeys.end())
                nextPending.push_back(key);
        }
        pendingRetireKeys_ = std::move(nextPending);
        publishedKeys_ = std::move(nextKeys);

        // seq_cst ties this store into the same total order as the audio reader
        // counter: a reader that increments after a reclaim check necessarily loads
        // the new pointer, never a view that is about to be destroyed.
        const SegmentsView* previous = publishedSegments_.exchange(view.get(), std::memory_order_seq_cst);
        if (previous != nullptr)
            retiredViews_.push_back(std::move(currentView_));
        currentView_ = std::move(view);
        ++publishedViewCount_;
    }
}

bool CaptureSession::finalizePendingCapture(CaptureSegment& pending)
{
    if (pending.getState() != SegmentState::Pending)
        return false;

    // Atomic writer-active handshake: only read the window after the audio thread released.
    if (pending.isWriterActive())
        return false;
    if (pending.spanBudgetExhausted.load(std::memory_order_acquire))
        return false;  // tick() reclaims the whole take

    const int numSpans = pending.numSpans.load(std::memory_order_acquire);
    const float observedPeak = pending.observedPeak.load(std::memory_order_relaxed);
    const bool windowFull = pending.windowExhausted.load(std::memory_order_acquire);
    const int64_t hostSpan = pending.hasWritten
        ? pending.maxHostSampleExclusive - pending.minHostSample
        : 0;

    AppLogger::log("CaptureSession::finalizePendingCapture id="
                   + juce::String(static_cast<juce::int64>(pending.contentKey.objectId))
                   + " hostSamples=" + juce::String(hostSpan)
                   + " spans=" + juce::String(numSpans)
                   + " durationSec=" + juce::String(
                         hostSpan / juce::jmax(1.0, pending.captureSampleRate), 3)
                   + " observedPeak=" + juce::String(observedPeak, 6)
                   + (windowFull ? " [WARNING: host window exhausted; publishing content up to the limit]" : "")
                   + (observedPeak < 1e-4f ? " [WARNING: near-silent buffer; host may not be routing audio to plugin]" : ""));

    auto dropPending = [this, &pending]() {
        pending.window.release();
        std::vector<CapturedSpan>().swap(pending.spans);

        std::lock_guard<std::mutex> lock(mutableMutex_);
        auto it = std::find_if(mutableSegments_.begin(), mutableSegments_.end(),
                               [&pending](const std::shared_ptr<CaptureSegment>& seg) {
                                   return seg.get() == &pending;
                               });
        if (it != mutableSegments_.end())
            mutableSegments_.erase(it);
    };

    if (!pending.content || !bindings_.publishPlaybackSource) {
        dropPending();
        return true;
    }

    // Spans are appended in write order and may overlap after a rewind: sort by host
    // start, then merge overlapping/touching ranges into the published segment ranges.
    std::vector<HostInterval> merged;
    merged.reserve(static_cast<size_t>(numSpans));
    for (int i = 0; i < numSpans; ++i) {
        const auto& span = pending.spans[static_cast<size_t>(i)];
        if (span.sampleCount <= 0)
            continue;
        merged.push_back({ span.hostStartSample,
                           span.hostStartSample + static_cast<int64_t>(span.sampleCount) });
    }
    std::sort(merged.begin(), merged.end(),
              [](const HostInterval& a, const HostInterval& b) { return a.start < b.start; });
    int mergedCount = 0;
    for (const auto& interval : merged) {
        if (mergedCount > 0) {
            auto& previous = merged[static_cast<size_t>(mergedCount - 1)];
            if (interval.start <= previous.end) {
                previous.end = std::max(previous.end, interval.end);
                continue;
            }
        }
        merged[static_cast<size_t>(mergedCount++)] = interval;
    }
    merged.resize(static_cast<size_t>(mergedCount));

    if (merged.empty()) {
        dropPending();
        return true;
    }

    bool reusedPendingSegment = false;
    std::vector<ContentKey> newKeys;
    newKeys.reserve(merged.size());
    for (const auto& interval : merged) {
        const int count = static_cast<int>(interval.end - interval.start);
        if (count <= 0)
            continue;

        auto pcm = std::make_shared<juce::AudioBuffer<float>>(pending.captureChannels, count);
        pending.window.readRange(pending.windowOffset(interval.start), *pcm, 0, count);

        CaptureSegment* target = nullptr;
        if (!reusedPendingSegment) {
            // First range reuses the pending segment (preserves the original ContentKey).
            target = &pending;
            reusedPendingSegment = true;
        } else {
            auto newSeg = std::make_shared<CaptureSegment>();
            const uint64_t newId = nextId();
            newSeg->contentKey = ContentKey{DomainKind::RegularVST3Capture, newId, 0};
            newSeg->creationOrder = newId;
            newSeg->captureSampleRate = pending.captureSampleRate;
            newSeg->captureChannels = pending.captureChannels;
            newSeg->capacitySamples = pending.capacitySamples;
            newSeg->content = std::make_unique<OpenTune::CaptureSegmentContent>(newId);
            target = newSeg.get();

            std::lock_guard<std::mutex> lock(mutableMutex_);
            mutableSegments_.push_back(std::move(newSeg));
        }

        // Authoritative absolute host range; T_start/durationSeconds are presentation only.
        target->hostStartSample.store(interval.start, std::memory_order_release);
        target->hostSampleCount.store(static_cast<int64_t>(count), std::memory_order_release);
        target->T_start.store(static_cast<double>(interval.start) / pending.captureSampleRate,
                              std::memory_order_release);
        target->durationSeconds = static_cast<double>(count) / pending.captureSampleRate;

        target->content->applyAudioBuffer(std::move(pcm), pending.captureSampleRate);
        target->content->applyOriginalF0State(OriginalF0State::Extracting);
        const auto snap = target->content->snapshotContent();
        bindings_.publishPlaybackSource(target->contentKey, snap,
                                        snap->audioBuffer, snap->audioSampleRate);
        target->setState(SegmentState::Processing);

        if (bindings_.refreshSegment)
            bindings_.refreshSegment(target->contentKey);

        newKeys.push_back(target->contentKey);
    }

    // The window and span metadata are baked into the published segments — free them.
    pending.window.release();
    std::vector<CapturedSpan>().swap(pending.spans);

    // New take fully finalized: trim/split pre-existing segments overlapped by the
    // published host ranges. Old keys leave the published set on tick()'s single
    // view publication and are retired via pendingRetireKeys_.
    std::vector<std::pair<int64_t, int64_t>> newRanges;
    newRanges.reserve(merged.size());
    for (const auto& interval : merged)
        newRanges.emplace_back(interval.start, interval.end);

    trimOverlappedSegments(newRanges, newKeys);

    return true;
}

void CaptureSession::trimOverlappedSegments(const std::vector<std::pair<int64_t, int64_t>>& newRanges,
                                            const std::vector<ContentKey>& newKeys)
{
    struct TrimPiece
    {
        bool hasCurve = false;
        std::shared_ptr<CaptureSegment> segment;
    };

    struct TrimAction
    {
        SegmentState oldState = SegmentState::Edited;
        std::vector<TrimPiece> pieces;
    };

    std::vector<TrimAction> actions;

    {
        std::lock_guard<std::mutex> lock(mutableMutex_);

        for (auto it = mutableSegments_.begin(); it != mutableSegments_.end();) {
            auto& seg = *it;
            const auto state = seg->getState();
            if (state == SegmentState::Capturing || state == SegmentState::Pending
                || std::find(newKeys.begin(), newKeys.end(), seg->contentKey) != newKeys.end()) {
                ++it;
                continue;
            }

            const int64_t s = seg->hostStartSample.load(std::memory_order_acquire);
            const int64_t e = s + seg->hostSampleCount.load(std::memory_order_acquire);

            // remaining = [s,e) - N, ascending. N is sorted and merged.
            std::vector<std::pair<int64_t, int64_t>> remaining;
            int64_t cursor = s;
            for (const auto& [nStart, nEnd] : newRanges) {
                if (nEnd <= cursor)
                    continue;
                if (nStart >= e)
                    break;
                if (nStart > cursor)
                    remaining.emplace_back(cursor, std::min(nStart, e));
                cursor = std::max(cursor, std::min(nEnd, e));
                if (cursor >= e)
                    break;
            }
            if (cursor < e)
                remaining.emplace_back(cursor, e);

            if (remaining.size() == 1 && remaining[0].first == s && remaining[0].second == e) {
                ++it;
                continue;
            }
            if (remaining.empty()) {
                it = mutableSegments_.erase(it);
                continue;
            }

            // Split: every kept interval becomes a brand-new CaptureSegment object
            // (the old object stays owned by audio-thread views until reclaim).
            TrimAction action;
            action.oldState = state;

            for (const auto& [keptStart, keptEnd] : remaining) {
                const int64_t offset = keptStart - s;
                const int64_t audioSamples = seg->content
                    && seg->content->content().audioBuffer
                    ? static_cast<int64_t>(seg->content->content().audioBuffer->getNumSamples())
                    : 0;
                const int64_t count = std::min(keptEnd - keptStart, audioSamples - offset);
                if (count <= 0)
                    continue;

                auto newSeg = std::make_shared<CaptureSegment>();
                const uint64_t newId = nextId();
                newSeg->contentKey = ContentKey{DomainKind::RegularVST3Capture, newId, 0};
                newSeg->creationOrder = newId;
                newSeg->captureSampleRate = seg->captureSampleRate;
                newSeg->captureChannels = seg->captureChannels;
                newSeg->capacitySamples = seg->capacitySamples;
                newSeg->hostStartSample.store(keptStart, std::memory_order_release);
                newSeg->hostSampleCount.store(count, std::memory_order_release);
                newSeg->T_start.store(static_cast<double>(keptStart) / seg->captureSampleRate,
                                      std::memory_order_release);
                newSeg->durationSeconds = static_cast<double>(count) / seg->captureSampleRate;
                newSeg->content = std::make_unique<OpenTune::CaptureSegmentContent>(newId);
                newSeg->content->applyContentState(
                    OpenTune::sliceContentState(seg->content->content(), offset, count, seg->captureSampleRate));
                newSeg->setState(state);

                TrimPiece piece;
                piece.hasCurve = newSeg->content->content().analysis.pitchCurve != nullptr;
                // A Processing piece with a sliced curve is submitted by the callback
                // below; record the baseline so tick() does not submit it a second time.
                if (state == SegmentState::Processing && piece.hasCurve)
                    newSeg->lastObservedF0State = OriginalF0State::Ready;
                piece.segment = std::move(newSeg);
                action.pieces.push_back(std::move(piece));
            }

            if (!action.pieces.empty()) {
                if (activeDisplaySegmentId_ == seg->contentKey.objectId)
                    activeDisplaySegmentId_ = action.pieces.front().segment->contentKey.objectId;
                actions.push_back(std::move(action));
            }

            it = mutableSegments_.erase(it);
        }

        for (auto& action : actions) {
            for (auto& piece : action.pieces)
                mutableSegments_.push_back(piece.segment);
        }
    }

    // Lock-free callbacks: publish audio for every new piece, then per-state actions.
    for (auto& action : actions) {
        for (auto& piece : action.pieces) {
            const auto& newSeg = piece.segment;
            const auto snap = newSeg->content->snapshotContent();
            if (bindings_.publishPlaybackSource)
                bindings_.publishPlaybackSource(newSeg->contentKey, snap,
                                                snap->audioBuffer, snap->audioSampleRate);

            switch (action.oldState) {
                case SegmentState::Edited:
                    if (piece.hasCurve && bindings_.requestFullRender)
                        bindings_.requestFullRender(newSeg->contentKey);
                    break;
                case SegmentState::Processing:
                    if (piece.hasCurve) {
                        if (bindings_.requestFullRender)
                            bindings_.requestFullRender(newSeg->contentKey);
                    } else if (bindings_.refreshSegment) {
                        bindings_.refreshSegment(newSeg->contentKey);
                    }
                    break;
                case SegmentState::Failed:
                    break;  // audio published above; stays Failed
                default:
                    break;
            }
        }
    }
}

uint64_t CaptureSession::nextId() noexcept

{
    return ++idCounter_;
}

std::vector<std::pair<SegmentState, bool>> CaptureSession::testStateSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    std::vector<std::pair<SegmentState, bool>> result;
    result.reserve(mutableSegments_.size());
    for (const auto& seg : mutableSegments_)
        result.emplace_back(seg->getState(), seg->isWriterActive());
    return result;
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
