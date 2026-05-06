#pragma once


#include "CaptureRingBuffer.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace OpenTune::Capture {

/**
 * Per-segment lifecycle state.
 *
 * Capturing  : audio thread is writing dry samples into fifo
 * Pending    : capture stopped, awaiting submit to render pipeline
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
 *   - materializationId (only valid in Edited state; treated as immutable once written)
 *
 * Audio thread writes:
 *   - fifo (via CaptureRingBuffer::write)
 *   - anchored.store(true) + T_start.store(host_t) on first isPlaying block after arm
 *   - stopRequested.store(true) when reaching duration cap or transport stop
 *
 * Message thread writes everything else (creation, state transitions, materializationId).
 *
 * Lifetime: segment object stays alive in CaptureSession until reclaim sweep
 * confirms no audio block can still see its pointer in published view.
 */
struct CaptureSegment
{
    /** Monotonic id, assigned at construction. */
    uint64_t id = 0;

    /** Strict creation order; equals id but kept separate for clarity. Newer = larger. */
    uint64_t creationOrder = 0;

    /** sampleRate at the time this segment was armed (used for fifo sizing + later resampling). */
    double captureSampleRate = 44100.0;

    /** Number of channels captured. Snapshotted from CaptureSession::captureChannels_
     *  at arm time and immutable for the segment's lifetime. Always 1 or 2 — see
     *  channel-layout-policy spec. */
    int captureChannels = 1;

    /** Maximum samples (= 600 s × captureSampleRate, set at arm time). */
    int maxSamples = 0;

    /** Anchor: first audio block with isPlaying==true sets anchored=true and stores host_t into T_start. */
    std::atomic<bool> anchored { false };
    std::atomic<double> T_start { 0.0 };

    /** Filled at stopCapture (message thread). Length of accepted PCM in seconds. */
    double durationSeconds = 0.0;

    /** Lifecycle state. Audio thread reads, message thread writes (with publish-subscribe). */
    std::atomic<SegmentState> state { SegmentState::Capturing };

    /** Set by audio thread when capture must end (duration cap or host stop). */
    std::atomic<bool> stopRequested { false };

    /** Diagnostic: peak absolute sample value seen during capture. Audio thread writes,
     *  message thread reads in stopCapture log. Helps distinguish "host sends silence"
     *  from "fifo write path broken". */
    std::atomic<float> observedPeak { 0.0f };

    /** SPSC fifo: audio writes, message drains in stopCapture. */
    CaptureRingBuffer fifo;

    /** Owned PCM after drainAll (message thread); stays valid through Edited and persistence. */
    std::shared_ptr<juce::AudioBuffer<float>> capturedAudio;

    /** Set when transitioning Processing → Edited. */
    uint64_t materializationId = 0;

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

