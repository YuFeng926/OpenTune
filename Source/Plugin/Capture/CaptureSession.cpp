
#include "CaptureSession.h"

#include "CaptureCompactor.h"
#include "CapturePersistence.h"
#include "../../Utils/AppLogger.h"
#include "../../Utils/ChannelLayoutLogger.h"

namespace OpenTune::Capture {

namespace {
    constexpr double kMaxSegmentSeconds = 600.0;  // 10 minutes per spec
    constexpr int kReclaimGraceTicks = 4;          // tick() iterations before destroying pending segments
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

        // Reject if any Capturing or Processing segment exists.
        for (const auto& seg : mutableSegments_) {
            const auto s = seg->state.load(std::memory_order_acquire);
            if (s == SegmentState::Capturing || s == SegmentState::Processing) {
                AppLogger::warn("CaptureSession::armNewCapture rejected: existing segment state="
                                + juce::String(static_cast<int>(s)));
                return false;
            }
        }

        // Cleanup of stale empty Pending segments left behind by prior failed arm/stop sequences:
        // if user clicked Read Audio but no PCM was ever captured (host idle / processBlock not
        // firing), the segment ended up Pending with 0 samples and SessionState reports HasCapturing
        // forever. Remove these to let the next arm proceed cleanly.
        auto it = mutableSegments_.begin();
        while (it != mutableSegments_.end()) {
            auto& seg = **it;
            if (seg.state.load(std::memory_order_acquire) == SegmentState::Pending
                && seg.fifo.getTotalWrittenSamples() == 0) {
                AppLogger::log("CaptureSession::armNewCapture: dropping stale empty Pending segment id="
                               + juce::String(static_cast<juce::int64>(seg.id)));
                pendingReclaim_.push_back(std::move(*it));
                it = mutableSegments_.erase(it);
                needPublish = true;
            } else {
                ++it;
            }
        }

        auto seg = std::make_unique<CaptureSegment>();
        seg->id = nextId();
        seg->creationOrder = seg->id;
        seg->captureSampleRate = currentSampleRate_;
        seg->captureChannels = captureChannels_.load(std::memory_order_acquire);  // snapshot, immutable per-segment
        seg->maxSamples = static_cast<int>(kMaxSegmentSeconds * currentSampleRate_);
        seg->fifo.reserve(seg->captureChannels, seg->maxSamples);
        seg->state.store(SegmentState::Capturing, std::memory_order_release);

        AppLogger::log("CaptureSession::armNewCapture id=" + juce::String(static_cast<juce::int64>(seg->id))
                       + " captureSampleRate=" + juce::String(seg->captureSampleRate, 1)
                       + " maxSamples=" + juce::String(seg->maxSamples)
                       + " captureChannels=" + juce::String(seg->captureChannels));
        ChannelLayoutLog::logSegmentArm(static_cast<juce::int64>(seg->id), seg->captureChannels);

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

    // Drain fifo into capturedAudio (message thread).
    const int written = capturing->fifo.getTotalWrittenSamples();
    const float observedPeak = capturing->observedPeak.load(std::memory_order_relaxed);
    AppLogger::log("CaptureSession::stopCapture id=" + juce::String(static_cast<juce::int64>(capturing->id))
                   + " writtenSamples=" + juce::String(written)
                   + " durationSec=" + juce::String(written / juce::jmax(1.0, capturing->captureSampleRate), 3)
                   + " observedPeak=" + juce::String(observedPeak, 6)
                   + (observedPeak < 1e-4f ? " [WARNING: near-silent buffer; host may not be routing audio to plugin]" : ""));
    if (written <= 0) {
        // Empty capture: discard the segment by leaving Pending state — armNewCapture will
        // reclaim such stale Pending segments next time the user clicks Read Audio.
        capturing->state.store(SegmentState::Pending, std::memory_order_release);
        publishSegmentsView();
        return;
    }

    auto pcm = std::make_shared<juce::AudioBuffer<float>>(capturing->captureChannels, written);
    capturing->fifo.drainAll(*pcm);
    capturing->fifo.release();
    capturing->capturedAudio = pcm;
    capturing->durationSeconds = static_cast<double>(written) / capturing->captureSampleRate;
    capturing->state.store(SegmentState::Processing, std::memory_order_release);

    // Submit to render pipeline; tick() polls bindings_.isRenderReady to drive Processing → Edited.
    if (bindings_.submitForRender) {
        const auto segId = capturing->id;
        const auto matId = bindings_.submitForRender(pcm, capturing->captureSampleRate,
                                                     "VST3 Capture " + juce::String(segId));
        if (matId != 0) {
            // Preliminary store — ARM the segment for completion lookup.
            capturing->materializationId = matId;
        } else {
            // Submit failed: revert state.
            capturing->state.store(SegmentState::Pending, std::memory_order_release);
        }
    }

    publishSegmentsView();
}

// ─── Audio thread ─────────────────────────────────────────────────────────

void CaptureSession::processBlock(juce::AudioBuffer<float>& buffer,
                                  double hostTimeSeconds,
                                  double hostSampleRate,
                                  bool isPlaying) noexcept
{
    juce::ignoreUnused(hostSampleRate);

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
    int zeroedSamplesAcrossChannels = 0;
    int firstZeroedChannel = -1;
    for (int ch = 0; ch < copyChannels; ++ch) {
        float* p = dryScratch_.getWritePointer(ch);
        int chZeroed = 0;
        for (int s = 0; s < numSamples; ++s) {
            const float v = p[s];
            if (!std::isfinite(v)) {
                p[s] = 0.0f;
                ++chZeroed;
            }
        }
        if (chZeroed > 0) {
            zeroedSamplesAcrossChannels += chZeroed;
            if (firstZeroedChannel < 0) firstZeroedChannel = ch;
        }
    }
    if (zeroedSamplesAcrossChannels > 0) {
        // Audio-thread direct call to ChannelLayoutLog is allowed: AppLogger::log is
        // thread-safe (uses juce::Logger which serializes via FileLogger). The 1Hz
        // throttle inside logNumericGuard prevents log floods.
        ChannelLayoutLog::logNumericGuard(zeroedSamplesAcrossChannels, firstZeroedChannel);
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
    bool transportRunning;
    if (prevHostTime < 0.0) {
        transportRunning = isPlaying;
    } else {
        transportRunning = (hostTimeSeconds - prevHostTime) > 1.0e-6;
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
        bindings_.replaceWithRendered(buffer, /*destStart*/ 0, numSamples,
                                       hit->materializationId, readStartSeconds, hostSampleRate);
    } else if (hit != nullptr && transportRunning) {
        // No binding wired: fail safe by silencing rather than emitting raw dry over the
        // edited time window (which would betray the user's intent).
        buffer.clear();
    }

    // Step 4: Capture dry into Capturing segment's fifo.
    //
    // Anchoring T_start: only stamp on the first block where transport is running,
    // otherwise paused-block visits (host_t frozen at click position) would pin
    // T_start to the wrong host time. Once anchored, we keep writing fifo every
    // block until the segment leaves Capturing — a brief mid-recording pause does
    // not desync the rest of the take.

    for (auto* seg : view->snapshot) {
        if (seg->state.load(std::memory_order_acquire) == SegmentState::Capturing) {
            // Anchor T_start on the first block where the transport is actually running.
            if (!seg->anchored.load(std::memory_order_acquire)) {
                if (!transportRunning)
                    continue;  // Skip this block; wait for transport to start.
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

    // 2. Poll Processing segments: promote to Edited on success, drop on failure.
    if (bindings_.isRenderReady || bindings_.isRenderFailed) {
        std::vector<std::pair<uint64_t, uint64_t>> toEdited;  // (segmentId, materializationId)
        std::vector<std::unique_ptr<CaptureSegment>> toDropFailed;
        {
            std::lock_guard<std::mutex> lock(mutableMutex_);
            auto it = mutableSegments_.begin();
            while (it != mutableSegments_.end()) {
                auto& seg = **it;
                const auto state = seg.state.load(std::memory_order_acquire);
                if (state != SegmentState::Processing || seg.materializationId == 0) {
                    ++it;
                    continue;
                }
                // Failure path takes precedence: F0 extraction returning empty/unvoiced
                // means this take is unusable. Drop it so the user can re-record without
                // getting stuck in Processing forever.
                if (bindings_.isRenderFailed && bindings_.isRenderFailed(seg.materializationId)) {
                    AppLogger::warn("CaptureSession: drop Processing segment id="
                                    + juce::String(static_cast<juce::int64>(seg.id))
                                    + " (render failed)");
                    if (bindings_.retireMaterialization)
                        bindings_.retireMaterialization(seg.materializationId);
                    toDropFailed.push_back(std::move(*it));
                    it = mutableSegments_.erase(it);
                    continue;
                }
                if (bindings_.isRenderReady && bindings_.isRenderReady(seg.materializationId)) {
                    toEdited.emplace_back(seg.id, seg.materializationId);
                }
                ++it;
            }
            if (!toDropFailed.empty()) {
                for (auto& dropped : toDropFailed)
                    pendingReclaim_.push_back(std::move(dropped));
                anyChange = true;
            }
        }
        for (auto& [segId, matId] : toEdited) {
            onSegmentRenderingComplete(segId, matId);
            anyChange = true;
        }
    }

    // 3. Reclaim sweep: destroy segments parked in pendingReclaim_ after grace period.
    const int currentTick = tickCounter_.fetch_add(1, std::memory_order_acq_rel);
    if (currentTick % kReclaimGraceTicks == 0) {
        std::vector<std::unique_ptr<CaptureSegment>> toDestroy;
        {
            std::lock_guard<std::mutex> lock(mutableMutex_);
            toDestroy = std::move(pendingReclaim_);
            pendingReclaim_.clear();
        }
        // toDestroy goes out of scope here, releasing the segments.
        // If retire callback is wired, retire materializations of segments removed by compaction.
        if (bindings_.retireMaterialization) {
            for (auto& seg : toDestroy) {
                if (seg && seg->materializationId != 0)
                    bindings_.retireMaterialization(seg->materializationId);
            }
        }
    }

    if (anyChange)
        publishSegmentsView();
}

// ─── Render pipeline callback ──────────────────────────────────────────────

void CaptureSession::onSegmentRenderingComplete(uint64_t segmentId, uint64_t materializationId)
{
    CaptureSegment* edited = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        for (auto& seg : mutableSegments_) {
            if (seg->id == segmentId) {
                seg->materializationId = materializationId;
                seg->state.store(SegmentState::Edited, std::memory_order_release);
                edited = seg.get();
                break;
            }
        }
    }
    if (edited == nullptr)
        return;

    AppLogger::log("CaptureSession: segment Edited id=" + juce::String(static_cast<juce::int64>(segmentId))
                   + " materializationId=" + juce::String(static_cast<juce::int64>(materializationId)));
    ChannelLayoutLog::logSegmentFinalize(static_cast<juce::int64>(segmentId),
                                          edited->captureChannels,
                                          edited->durationSeconds);

    runCompaction(*edited);
    publishSegmentsView();

    if (activeSegmentChanged_)
        activeSegmentChanged_(materializationId);
}

void CaptureSession::setActiveSegmentChangedCallback(ActiveSegmentChangedFn fn)
{
    activeSegmentChanged_ = std::move(fn);
}

// ─── Query ────────────────────────────────────────────────────────────────

SessionState CaptureSession::getGlobalState() const noexcept
{
    auto view = std::atomic_load(&publishedSegments_);
    if (view == nullptr) return SessionState::Idle;

    bool hasCapturing = false, hasProcessing = false;
    for (auto* seg : view->snapshot) {
        const auto s = seg->state.load(std::memory_order_acquire);
        // Pending is intentionally treated as Idle: it represents a discarded/failed
        // transient (empty fifo on stopCapture). Letting it count as HasCapturing
        // would trap the GUI state machine — the user keeps hitting "stop" forever.
        if (s == SegmentState::Capturing) hasCapturing = true;
        else if (s == SegmentState::Processing) hasProcessing = true;
    }
    if (hasCapturing) return SessionState::HasCapturing;
    if (hasProcessing) return SessionState::HasProcessing;
    return SessionState::Idle;
}

double CaptureSession::getCurrentlyCapturedSeconds() const noexcept
{
    auto view = std::atomic_load(&publishedSegments_);
    if (view == nullptr) return 0.0;
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
    if (view == nullptr) return 0;
    for (auto* seg : view->snapshot) {
        if (seg->capturedAudio) {
            total += static_cast<size_t>(seg->capturedAudio->getNumChannels())
                   * static_cast<size_t>(seg->capturedAudio->getNumSamples())
                   * sizeof(float);
        }
    }
    return total;
}

std::vector<SegmentInfo> CaptureSession::listSegments() const
{
    std::vector<SegmentInfo> result;
    std::lock_guard<std::mutex> lock(mutableMutex_);
    result.reserve(mutableSegments_.size());
    for (const auto& seg : mutableSegments_) {
        SegmentInfo info;
        info.id = seg->id;
        info.T_start = seg->T_start.load(std::memory_order_acquire);
        info.durationSeconds = seg->durationSeconds;
        info.state = seg->state.load(std::memory_order_acquire);
        info.materializationId = seg->materializationId;
        result.push_back(info);
    }
    return result;
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
                                                  uint64_t materializationId,
                                                  std::shared_ptr<juce::AudioBuffer<float>> pcm)
{
    std::lock_guard<std::mutex> lock(mutableMutex_);
    auto seg = std::make_unique<CaptureSegment>();
    seg->id = nextId();
    seg->creationOrder = seg->id;
    seg->captureSampleRate = currentSampleRate_;
    seg->captureChannels = pcm ? pcm->getNumChannels() : 2;
    seg->T_start.store(T_start, std::memory_order_release);
    seg->anchored.store(true, std::memory_order_release);
    seg->durationSeconds = durationSeconds;
    seg->capturedAudio = std::move(pcm);
    seg->materializationId = materializationId;
    seg->state.store(SegmentState::Edited, std::memory_order_release);
    const auto id = seg->id;
    mutableSegments_.push_back(std::move(seg));
    publishSegmentsView();
    return id;
}

// ─── Private helpers ───────────────────────────────────────────────────────

void CaptureSession::publishSegmentsView()
{
    auto view = std::make_shared<SegmentsView>();
    view->snapshot.reserve(mutableSegments_.size());
    for (const auto& seg : mutableSegments_)
        view->snapshot.push_back(seg.get());
    std::atomic_store(&publishedSegments_, std::shared_ptr<const SegmentsView>(view));
}

void CaptureSession::runCompaction(const CaptureSegment& newlyEdited)
{
    std::vector<std::unique_ptr<CaptureSegment>> removed;
    {
        std::lock_guard<std::mutex> lock(mutableMutex_);
        removed = CaptureCompactor::removeFullyCovered(mutableSegments_, newlyEdited);

        // Move removed segments to pendingReclaim_ for grace-period reclaim.
        for (auto& r : removed)
            pendingReclaim_.push_back(std::move(r));
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

}  // namespace OpenTune::Capture

