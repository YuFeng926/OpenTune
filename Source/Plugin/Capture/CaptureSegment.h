#pragma once


#include "CaptureRingBuffer.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include "../../Content/ContentKey.h"
#include "../../Content/CaptureSegmentContent.h"

namespace OpenTune::Capture {

/**
 * Per-segment lifecycle state.
 *
 * Capturing  : audio thread is writing dry samples into fifo
 * Pending    : capture stopped, waiting for a safe message-thread drain/submit
 * Processing : submitted; F0/Vocoder rendering in progress
 * Edited     : rendered; eligible for replacement playback
 */
enum class SegmentState : int
{
    Capturing = 0,
    Pending,
    Processing,
    Edited
};

/**
 * One captured take. Owned by CaptureSession's mutableSegments_.
 *
 * Audio thread reads:
 *   - state (atomic), T_start (atomic), durationSeconds, anchored (atomic)
 *
 * Audio thread writes:
 *   - fifo (via CaptureRingBuffer::write)
 *   - anchored.store(true) + T_start.store(host_t) on first isPlaying block after arm
 *   - stopRequested.store(true) when reaching duration cap or transport stop
 *
 * Message thread writes everything else (creation, state transitions).
 *
 * Lifetime: segment object stays alive in CaptureSession until reclaim sweep
 * confirms no audio block can still see its pointer in published view.
 */
struct CaptureSegment
{
    /** Domain content key for this capture segment. Set at creation time. Serves as unique identity. */
    ContentKey contentKey;

    /** Strict creation order; equals contentKey.objectId. Newer = larger. */
    uint64_t creationOrder = 0;

    /** sampleRate at the time this segment was armed (used for fifo sizing + later resampling). */
    double captureSampleRate = 44100.0;

    /** Number of channels captured. Snapshotted from CaptureSession::captureChannels_
     *  at arm time and immutable for the segment's lifetime. Always 1 or 2 — see
     *  channel-layout-policy spec. */
    int captureChannels = 1;

    /** Maximum samples (= 600 s × captureSampleRate, set at arm time). */
    int maxSamples = 0;

    /** Anchor: first audio block with advancing host transport sets anchored=true and stores host_t into T_start. */
    std::atomic<bool> anchored { false };
    std::atomic<double> T_start { 0.0 };

    /** Filled at stopCapture (message thread). Length of accepted PCM in seconds. */
    double durationSeconds = 0.0;

    /** Lifecycle state. Audio thread reads, message thread writes (with publish-subscribe). */
    std::atomic<SegmentState> state { SegmentState::Capturing };

    /** Set by audio thread when capture must end (duration cap or transport discontinuity). */
    std::atomic<bool> stopRequested { false };

    /** Message-thread countdown before draining a stopped capture.
     *  This gives any in-flight audio block that already passed the Capturing state
     *  check time to leave CaptureRingBuffer::write before FIFO release. */
    int pendingDrainTicks = 0;

    /** Message-thread only: F0 Ready 后是否已提交过一次全量渲染。tick() 仅在
     *  跃迁（Ready 首次出现）时提交 requestFullRender，避免渲染窗口内
     *  （渲染耗时 > 33ms，大于 30Hz tick 间隔）重复提交取消并重启 Running chunk。 */
    bool renderRequested = false;

    /** Diagnostic: peak absolute sample value seen during capture. Audio thread writes,
     *  message thread reads in stopCapture log. Helps distinguish "host sends silence"
     *  from "fifo write path broken". */
    std::atomic<float> observedPeak { 0.0f };

    /** SPSC fifo: audio writes, message drains in stopCapture. */
    CaptureRingBuffer fifo;

    /** Content owner for this segment. */
    std::unique_ptr<OpenTune::CaptureSegmentContent> content;

    /** Compute end time (only valid for Edited segments). */
    double endTime() const noexcept { return T_start.load(std::memory_order_acquire) + durationSeconds; }

    /** Test if host_t is in [T_start, T_start + duration). */
    bool containsTime(double host_t) const noexcept
    {
        const double start = T_start.load(std::memory_order_acquire);
        return host_t >= start && host_t < start + durationSeconds;
    }
};

/**
 * Lightweight read-only view of segments published to the audio thread.
 * Pointers borrow the lifetime of CaptureSegment objects in mutableSegments_.
 */
struct SegmentsView
{
    std::vector<CaptureSegment*> snapshot;
};

}  // namespace OpenTune::Capture
