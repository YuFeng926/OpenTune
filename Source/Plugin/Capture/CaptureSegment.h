#pragma once


#include "CaptureRingBuffer.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "../../Content/ContentKey.h"
#include "../../Content/CaptureSegmentContent.h"
#include "../../Utils/ContentAnalysisState.h"

namespace OpenTune::Capture {

/**
 * Per-segment lifecycle state.
 *
 * Capturing  : audio thread is writing dry samples into fifo
 * Pending    : capture stopped, waiting for a safe message-thread drain/submit
 * Processing : submitted; F0/Vocoder rendering in progress
 * Edited     : rendered; eligible for replacement playback
 * Failed     : render or F0 analysis failed; content preserved, not blocking next capture
 */
enum class SegmentState : int
{
    Capturing = 0,
    Pending,
    Processing,
    Edited,
    Failed
};

/**
 * Pre-allocated metadata for one continuous run of captured PCM within a CaptureSegment.
 * Audio thread writes only within pre-allocated capacity; message thread reads at drain time.
 */
struct CapturedSpan
{
    int64_t hostStartSample = 0;   // absolute host sample at span start
    int pcmOffsetSamples = 0;      // offset into drained FIFO buffer
    int sampleCount = 0;           // number of samples accepted by fifo.write
};

/**
 * One captured take. Owned by CaptureSession's mutableSegments_.
 *
 * Audio thread reads:
 *   - state (atomic), anchored (atomic), hostStartSample (atomic), hostSampleCount (atomic)
 *
 * Audio thread writes:
 *   - fifo (via CaptureRingBuffer::write)
 *   - anchored.store(true) + hostStartSample.store(sample) on first isPlaying block after arm
 *
 * Message thread writes everything else (creation, state transitions, T_start/durationSeconds).
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

    /** Anchor: first isPlaying audio block sets anchored=true and stores hostAbsoluteSample into hostStartSample. */
    std::atomic<bool> anchored { false };

    /** Authoritative absolute host sample range. Set at finalize (message thread).
     *  Audio thread reads for playback hit detection via containsAbsoluteSample. */
    std::atomic<int64_t> hostStartSample { 0 };
    std::atomic<int64_t> hostSampleCount { 0 };

    /** UI/persistence presentation fields. Computed from hostStartSample/hostSampleCount
     *  at finalize time (message thread). NOT used by audio thread for positioning. */
    std::atomic<double> T_start { 0.0 };
    double durationSeconds = 0.0;

    /** Lifecycle state. Audio thread reads, message thread writes (with publish-subscribe). */
    std::atomic<SegmentState> state { SegmentState::Capturing };

    /** Atomic writer-active handshake. Audio thread CAS true before writing to FIFO;
     *  message thread tick() only drains after this reads false. Replaces
     *  the old pendingDrainTicks countdown with a deterministic lock-free handshake. */
    std::atomic<bool> writerActive { false };

    /** Number of populated span entries in spans[]. Audio thread writes (monotonically
     *  increasing), message thread reads at drain time. Bounded by spans.size(). */
    std::atomic<int> numSpans { 0 };

    /** Pre-allocated span metadata array. Capacity covers the full PCM budget, so
     *  the audio thread never needs to allocate or silently lose a discontinuity. */
    std::vector<CapturedSpan> spans;

    /** Message-thread only: 上次 tick 观察到的 F0 状态。tick() 仅在跃迁
     *  （非 Ready → Ready，含首次观察即 Ready）时提交一次 requestFullRender，
     *  避免渲染窗口内（渲染耗时 > 33ms，大于 30Hz tick 间隔）重复提交取消并重启
     *  Running chunk。与插件 UI 侧 lastObservedOriginalF0States_ 同为
     *  状态记录+跃迁检测模式；初始 NotRequested 保证首次观察即 Ready 不丢失。 */
    OriginalF0State lastObservedF0State = OriginalF0State::NotRequested;

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

    /** Test if host_t is in [T_start, T_start + duration). UI/display path only. */
    bool containsTime(double host_t) const noexcept
    {
        const double start = T_start.load(std::memory_order_acquire);
        return host_t >= start && host_t < start + durationSeconds;
    }

    /** Test if an absolute host sample falls within this segment's captured range.
     *  Audio thread uses this for playback hit detection — no seconds conversion. */
    bool containsAbsoluteSample(int64_t sample) const noexcept
    {
        const int64_t start = hostStartSample.load(std::memory_order_acquire);
        const int64_t count = hostSampleCount.load(std::memory_order_acquire);
        return count > 0 && sample >= start && sample < start + count;
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
