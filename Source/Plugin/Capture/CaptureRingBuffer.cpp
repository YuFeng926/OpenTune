
#include "CaptureRingBuffer.h"

namespace OpenTune::Capture {

CaptureRingBuffer::CaptureRingBuffer() = default;

void CaptureRingBuffer::reserve(int numChannels, int capacitySamples)
{
    numChannels_ = juce::jmax(1, numChannels);
    capacitySamples_ = juce::jmax(1, capacitySamples);
    buffer_.setSize(numChannels_, capacitySamples_, /*keepExistingContent*/ false,
                    /*clearExtraSpace*/ true, /*avoidReallocating*/ false);
    fifo_.setTotalSize(capacitySamples_);
    fifo_.reset();
    totalWritten_.store(0, std::memory_order_release);
}

void CaptureRingBuffer::release()
{
    buffer_.setSize(0, 0);
    fifo_.setTotalSize(1);
    fifo_.reset();
    numChannels_ = 0;
    capacitySamples_ = 0;
    totalWritten_.store(0, std::memory_order_release);
}

int CaptureRingBuffer::write(const float* const* srcChannels, int numSourceChannels, int numSamples) noexcept
{
    if (numSamples <= 0 || numChannels_ == 0 || capacitySamples_ == 0)
        return 0;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo_.prepareToWrite(numSamples, start1, size1, start2, size2);

    const int channelsToCopy = juce::jmin(numSourceChannels, numChannels_);

    auto copyRange = [&](int dstStart, int blockSize) {
        if (blockSize <= 0) return;
        for (int ch = 0; ch < numChannels_; ++ch) {
            float* dst = buffer_.getWritePointer(ch, dstStart);
            if (ch < channelsToCopy && srcChannels[ch] != nullptr) {
                std::memcpy(dst, srcChannels[ch], static_cast<size_t>(blockSize) * sizeof(float));
            } else {
                std::memset(dst, 0, static_cast<size_t>(blockSize) * sizeof(float));
            }
        }
    };

    copyRange(start1, size1);
    copyRange(start2, size2);

    const int written = size1 + size2;
    fifo_.finishedWrite(written);
    totalWritten_.fetch_add(written, std::memory_order_acq_rel);
    return written;
}

int CaptureRingBuffer::drainAll(juce::AudioBuffer<float>& out)
{
    const int available = fifo_.getNumReady();
    if (available <= 0 || out.getNumChannels() < numChannels_ || out.getNumSamples() < available)
        return 0;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo_.prepareToRead(available, start1, size1, start2, size2);

    auto copyRange = [&](int srcStart, int dstOffset, int blockSize) {
        if (blockSize <= 0) return;
        for (int ch = 0; ch < numChannels_; ++ch) {
            const float* src = buffer_.getReadPointer(ch, srcStart);
            float* dst = out.getWritePointer(ch, dstOffset);
            std::memcpy(dst, src, static_cast<size_t>(blockSize) * sizeof(float));
        }
    };

    copyRange(start1, 0, size1);
    copyRange(start2, size1, size2);

    fifo_.finishedRead(available);
    return available;
}

}  // namespace OpenTune::Capture

