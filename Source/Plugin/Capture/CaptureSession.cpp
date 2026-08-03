
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
    constexpr int kMinStopDrainGraceTicks = 2;     // ticks before releasing a stopped capture FIFO
    constexpr int kMaxStopDrainGraceTicks = 30;
    constexpr double kApproxStopDrainTickHz = 30.0;
    constexpr double kTransportAdvanceEpsilonSeconds = 1.0e-6;
    constexpr double kMinTransportDiscontinuitySeconds = 0.05;

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
                && seg.pendingDrainTicks <= 0) {
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

    capturing->stopRequested.store(true, std::memory_order_release);
    const double maxBlockSeconds = currentSampleRate_ > 0.0
        ? static_cast<double>(juce::jmax(0, currentMaxBlockSize_)) / currentSampleRate_
        : 0.0;
    capturing->pendingDrainTicks = juce::jlimit(kMinStopDrainGraceTicks,
                                                kMaxStopDrainGraceTicks,
                                                kMinStopDrainGraceTicks + 1
                                                    + static_cast<int>(maxBlockSeconds * kApproxStopDrainTickHz));
    capturing->state.store(SegmentState::Pending, std::memory_order_release);

    AppLogger::log("CaptureSession::stopCapture id=" + juce::String(static_cast<juce::int64>(capturing->contentKey.objectId))
                   + " pendingDrainTicks=" + juce::String(capturing->pendingDrainTicks));

    publishSegmentsView();
}

// ─── Audio thread ─────────────────────────────────────────────────────────

void CaptureSession::processBlock(juce::AudioBuffer<float>& buffer,
                                  double hostTimeSeconds,
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
    // Any legitimate finite float (regardless of magnitude) passes through unchanged
    // — the user's recording is preserved as-is. NaN/Inf would otherwise propagate
    // through downstream FFT / SIMD lanes and corrupt the entire frame.
    for (int ch = 0; ch < copyChannels; ++ch) {
        float* p = dryScratch_.getWritePointer(ch);
        for (int s = 0; s < numSamples; ++s) {
            const float v = p[s];
            if (!std::isfinite(v)) {
                p[s] = 0.0f;
            }
        }
    }

    // Detect "transport is running" by observing host_t advance across blocks.
    // host_t-advancement is the only signal that's reliable across all DAWs:
    //   - Logic / Cubase / Studio One: isPlaying matches host_t advancement.
    //   - FL Studio / some Reaper configs: report isPlaying=false even while audio
    //     flows, but host_t still advances.
    //   - Some hosts hold isPlaying=true while paused (input monitoring): host_t
    //     is static even though the flag says "playing".
    // The last case is what triggers a sawtooth in the replace-playback path: every
    // block we'd read the same ~10 ms slice from the rendered audio. Gating on host_t
    // advance correctly identifies this as "not running" regardless of isPlaying.
    //
    // First-block edge case (no prior host_t observed): defer to isPlaying. We may
    // miss the very first block of a playback session (~10 ms), which is imperceptible.
    const double prevHostTime = lastSeenHostTime_;
    lastSeenHostTime_ = hostTimeSeconds;
    const double hostDeltaSeconds = (prevHostTime >= 0.0) ? hostTimeSeconds - prevHostTime : 0.0;
    const double blockDurationSeconds = static_cast<double>(numSamples) / juce::jmax(1.0, hostSampleRate);
    const double discontinuityThresholdSeconds = juce::jmax(kMinTransportDiscontinuitySeconds,
                                                            blockDurationSeconds * 4.0);
    const bool hostTimeMovedBackward = prevHostTime >= 0.0
        && hostDeltaSeconds < -kTransportAdvanceEpsilonSeconds;
    const bool hostTimeJumpedForward = prevHostTime >= 0.0
        && hostDeltaSeconds > discontinuityThresholdSeconds;
    const bool hostTimeDiscontinuous = hostTimeMovedBackward || hostTimeJumpedForward;
    bool transportRunning;
    if (prevHostTime < 0.0) {
        transportRunning = isPlaying;
    } else {
        transportRunning = hostDeltaSeconds > kTransportAdvanceEpsilonSeconds;
    }

    // Step 2: Reverse-iterate to find newest Edited segment covering host_t.
    CaptureSegment* hit = nullptr;
    for (auto it = view->snapshot.rbegin(); it != view->snapshot.rend(); ++it) {
        auto* seg = *it;
        if (seg->state.load(std::memory_order_acquire) == SegmentState::Edited
            && seg->containsTime(hostTimeSeconds)) {
            hit = seg;
            break;
        }
    }

    // Step 3: Replace if hit AND transport is running. While paused (host_t frozen),
    // emitting any rendered audio would re-play the same ~10 ms slice on every host
    // block — audible as a buzzing sawtooth. Skipping the overwrite leaves the
    // buffer holding the host's (silent) input.
    if (hit != nullptr && transportRunning && bindings_.replaceWithRendered) {
        const double readStartSeconds = hostTimeSeconds - hit->T_start.load(std::memory_order_acquire);
        // Pass full ContentKey to replaceWithRendered
        bindings_.replaceWithRendered(buffer, /*destStart*/ 0, numSamples,
                                       hit->contentKey, readStartSeconds, hostSampleRate);
    } else if (hit != nullptr && transportRunning) {
        // No binding wired: fail safe by silencing rather than emitting raw dry over the
        // edited time window (which would betray the user's intent).
        buffer.clear();
    }

    // Step 4: Capture dry into Capturing segment's fifo.
    //
    // Anchoring T_start: only stamp on the first block where transport is running,
    // otherwise paused-block visits (host_t frozen at click position) would pin
    // T_start to the wrong host time. Once anchored, only continuous host_t
    // advancement writes into the FIFO; pauses skip blocks, and rewind/loop/seek
    // discontinuities end the current take instead of concatenating unrelated audio.

    for (auto* seg : view->snapshot) {
        if (seg->state.load(std::memory_order_acquire) == SegmentState::Capturing) {
            if (seg->stopRequested.load(std::memory_order_acquire))
                break;

            if (seg->anchored.load(std::memory_order_acquire) && hostTimeDiscontinuous) {
                seg->stopRequested.store(true, std::memory_order_release);
                break;
            }

            if (!transportRunning)
                continue;

            // Anchor T_start on the first block where the transport is actually running.
            if (!seg->anchored.load(std::memory_order_acquire)) {
                seg->T_start.store(hostTimeSeconds, std::memory_order_release);
                seg->anchored.store(true, std::memory_order_release);
            }

            // Build channel pointer array on stack (no alloc). Cap at the segment's
            // snapshot captureChannels (immutable since arm) AND at how many channels
            // we actually copied into dryScratch this block.
            const float* srcPtrs[32];
            const int chCount = juce::jmin(seg->captureChannels, copyChannels, 32);
            for (int ch = 0; ch < chCount; ++ch)
                srcPtrs[ch] = dryScratch_.getReadPointer(ch);

            seg->fifo.write(srcPtrs, chCount, numSamples);

            // Diagnostic: track peak level so we can confirm whether host is actually
            // routing audio to this plugin. Audio thread updates atomic; tick() logs
            // occasionally on message thread (no AppLogger from audio thread).
            float blockPeak = 0.0f;
            for (int ch = 0; ch < chCount; ++ch) {
                const float* src = srcPtrs[ch];
                for (int s = 0; s < numSamples; ++s) {
                    const float a = std::abs(src[s]);
                    if (a > blockPeak) blockPeak = a;
                }
            }
            const float prevPeak = seg->observedPeak.load(std::memory_order_relaxed);
            if (blockPeak > prevPeak)
                seg->observedPeak.store(blockPeak, std::memory_order_relaxed);

            // Auto-stop at duration cap.
            if (seg->fifo.getTotalWrittenSamples() >= seg->maxSamples)
                seg->stopRequested.store(true, std::memory_order_release);

            break;
        }
    }
}

void CaptureSession::tick()
{
    bool anyChange = false;

    // 1. Audio-thread-signaled stop requests → drive Capturing → Pending → Processing.
    bool needStop = false;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        for (auto& seg : mutableSegments_) {
            if (seg->state.load(std::memory_order_acquire) == SegmentState::Capturing
                && seg->stopRequested.load(std::memory_order_acquire)) {
                needStop = true;
                break;
            }
        }
    }
    if (needStop) {
        stopCapture();   // single-Capturing invariant: only one to stop per tick
        anyChange = true;
    }

    // 2. Pending captures are no longer writable by the audio thread. Drain after a
    // short grace window so a block that already passed the Capturing check cannot
    // race FIFO release.
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
    // No reentrant callback — tick() owns the lock while scanning.
    {
        std::vector<ContentKey> failedKeys;

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

                if (f0State == OriginalF0State::Failed) {
                    AppLogger::warn("CaptureSession: drop Processing segment id="
                                    + juce::String(static_cast<juce::int64>(seg.contentKey.objectId))
                                    + " (F0 failed)");
                    failedKeys.push_back(seg.contentKey);
                    queueForReclaimLocked(std::move(*it));
                    it = mutableSegments_.erase(it);
                    anyChange = true;
                    continue;
                }

                if (f0State == OriginalF0State::Ready) {
                    // F0 Ready: trigger render, wait for onRenderComplete callback
                    // Do NOT set Edited here; wait for render completion
                    if (bindings_.requestFullRender) {
                        bindings_.requestFullRender(seg.contentKey);
                    }
                    anyChange = true;
                }

                ++it;
            }
        }

        if (bindings_.retireSegment) {
            for (const auto& key : failedKeys)
                bindings_.retireSegment(key);
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

    if (activeSegmentChanged_)
        activeSegmentChanged_(segmentContentKey);
}

bool CaptureSession::commitSegmentF0Result(
    ContentKey segmentContentKey,
    std::shared_ptr<PitchCurve> pitchCurve,
    OriginalF0State state,
    const DetectedKey& detectedKey)
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

    seg->content->applyDetectedKey(detectedKey);
    seg->content->applyOriginalF0State(state);
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

    seg->content->applyNotesWithOutputGain(std::move(notes));
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

bool CaptureSession::applyNotesWithOutputGain(ContentKey segmentContentKey, std::vector<Note> notes)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto* seg = findMutableSegmentByContentKey(mutableSegments_, segmentContentKey);
    if (seg == nullptr || !seg->content)
        return false;

    seg->content->applyNotesWithOutputGain(std::move(notes));
    return true;
}

bool CaptureSession::applySibilantGainEnvelope(ContentKey segmentContentKey, SibilantGainEnvelope envelope)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto* seg = findMutableSegmentByContentKey(mutableSegments_, segmentContentKey);
    if (seg == nullptr || !seg->content)
        return false;

    seg->content->applySibilantGainEnvelope(std::move(envelope));
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

void CaptureSession::setActiveSegmentChangedCallback(ActiveSegmentChangedFn fn)
{
    activeSegmentChanged_ = std::move(fn);
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
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto seg = std::make_unique<CaptureSegment>();
    const uint64_t id = segmentId > 0 ? segmentId : nextId();
    seg->contentKey = ContentKey{DomainKind::RegularVST3Capture, id, 0};
    seg->creationOrder = id;
    seg->captureSampleRate = currentSampleRate_;
    seg->captureChannels = pcm ? pcm->getNumChannels() : 2;
    seg->T_start.store(T_start, std::memory_order_release);
    seg->anchored.store(true, std::memory_order_release);
    seg->durationSeconds = durationSeconds;
    seg->content = std::make_unique<CaptureSegmentContent>(id);
    if (pcm)
        seg->content->applyAudioBuffer(*pcm, currentSampleRate_);
    seg->state.store(SegmentState::Edited, std::memory_order_release);
    activeDisplaySegmentId_ = id;
    mutableSegments_.push_back(std::move(seg));
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
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto seg = std::make_unique<CaptureSegment>();
    const uint64_t id = segmentId > 0 ? segmentId : nextId();
    seg->contentKey = ContentKey{DomainKind::RegularVST3Capture, id, 0};
    seg->creationOrder = id;
    seg->captureSampleRate = sampleRate;
    seg->captureChannels = pcm ? pcm->getNumChannels() : 1;
    seg->T_start.store(T_start, std::memory_order_release);
    seg->anchored.store(true, std::memory_order_release);
    seg->durationSeconds = durationSeconds;
    seg->content = std::make_unique<CaptureSegmentContent>(id);
    if (pcm)
        seg->content->applyAudioBuffer(*pcm, sampleRate);
    seg->content->applyOriginalF0State(OriginalF0State::Extracting);
    seg->state.store(SegmentState::Processing, std::memory_order_release);
    mutableSegments_.push_back(std::move(seg));
    publishSegmentsView();
    return id;
}

void CaptureSession::publishSegmentsView()
{
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

    if (pending.pendingDrainTicks > 0) {
        --pending.pendingDrainTicks;
        return false;
    }

    const int written = pending.fifo.getTotalWrittenSamples();
    const float observedPeak = pending.observedPeak.load(std::memory_order_relaxed);
    AppLogger::log("CaptureSession::finalizePendingCapture id="
                   + juce::String(static_cast<juce::int64>(pending.contentKey.objectId))
                   + " writtenSamples=" + juce::String(written)
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

    if (written <= 0) {
        dropPending();
        return true;
    }

    auto pcm = std::make_shared<juce::AudioBuffer<float>>(pending.captureChannels, written);
    const int drained = pending.fifo.drainAll(*pcm);
    pending.fifo.release();
    if (drained <= 0) {
        dropPending();
        return true;
    }

    pending.durationSeconds = static_cast<double>(drained) / pending.captureSampleRate;

    if (!pending.content || !bindings_.publishPlaybackSource) {
        dropPending();
        return true;
    }

    pending.content->applyAudioBuffer(*pcm, pending.captureSampleRate);
    pending.content->applyOriginalF0State(OriginalF0State::Extracting);

    const auto snap = pending.content->snapshotContent();
    bindings_.publishPlaybackSource(pending.contentKey, snap->audioBuffer, snap->audioSampleRate);

    pending.state.store(SegmentState::Processing, std::memory_order_release);

    if (bindings_.refreshSegment) {
        bindings_.refreshSegment(pending.contentKey);
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
