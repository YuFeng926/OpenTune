#pragma once


#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>

namespace OpenTune::Capture {

/**
 * Single-producer, single-consumer FIFO of stereo float audio.
 *
 * Producer: audio thread (CaptureSession::processBlock).
 *   - write(srcChannels, numSamples) is noexcept and never allocates.
 *   - Returns the number of samples actually accepted (may be less when full).
 *
 * Consumer: message thread (CaptureSession::stopCapture).
 *   - drainAll(out) reads everything and resets the FIFO.
 *
 * Capacity is fixed by reserve(); the underlying juce::AudioBuffer is
 * pre-allocated, so audio thread writes are pure copy + atomic index update.
 */
class CaptureRingBuffer
{
public:
    CaptureRingBuffer();

    /** Allocate the underlying buffer. Message thread only. */
    void reserve(int numChannels, int capacitySamples);

    /** Release the underlying buffer. Message thread only. */
    void release();

    int getCapacitySamples() const noexcept { return capacitySamples_; }
    int getNumChannels() const noexcept { return numChannels_; }
    int getTotalWrittenSamples() const noexcept { return totalWritten_.load(std::memory_order_acquire); }

    /**
     * Audio-thread-safe write. Returns number of samples accepted.
     * Never blocks, never allocates.
     */
    int write(const float* const* srcChannels, int numSourceChannels, int numSamples) noexcept;

    /**
     * Message-thread drain: copies all written samples into 'out' and
     * resets the FIFO read/write pointers. Returns number of samples drained.
     * 'out' must already be sized to at least getTotalWrittenSamples().
     */
    int drainAll(juce::AudioBuffer<float>& out);

private:
    int numChannels_ = 0;
    int capacitySamples_ = 0;
    juce::AudioBuffer<float> buffer_;
    juce::AbstractFifo fifo_ { 1 };  // sentinel size, replaced in reserve()
    std::atomic<int> totalWritten_ { 0 };
};

}  // namespace OpenTune::Capture

