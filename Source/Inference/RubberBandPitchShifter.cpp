#include "RubberBandPitchShifter.h"
#include <rubberband/RubberBandLiveShifter.h>
#include <cmath>
#include <algorithm>

namespace OpenTune {

RubberBandPitchShifter::RubberBandPitchShifter(double sampleRate)
    : sampleRate_(sampleRate)
{
    using RBLive = RubberBand::RubberBandLiveShifter;
    RBLive::Options options =
        RBLive::OptionChannelsTogether |
        RBLive::OptionFormantPreserved;

    shifter_ = std::make_unique<RBLive>(
        static_cast<size_t>(sampleRate), 1, options);
    blockSize_ = shifter_->getBlockSize();
}

RubberBandPitchShifter::~RubberBandPitchShifter() = default;

size_t RubberBandPitchShifter::getBlockSize() const { return blockSize_; }

std::vector<float> RubberBandPitchShifter::shiftChunk(
    const float* input, int numSamples,
    const float* originalF0, const float* correctedF0,
    int numF0Frames, double f0FrameRate)
{
    std::vector<float> output(static_cast<size_t>(numSamples), 0.0f);

    // 处理 startDelay：LiveShifter 有固定延迟，需要先喂静音消除
    const size_t startDelay = shifter_->getStartDelay();
    std::vector<float> padBlock(blockSize_, 0.0f);
    std::vector<float> padOut(blockSize_, 0.0f);
    const float* padInPtr = padBlock.data();
    float* padOutPtr = padOut.data();

    size_t padFed = 0;
    while (padFed < startDelay) {
        shifter_->setPitchScale(1.0);
        shifter_->shift(&padInPtr, &padOutPtr);
        padFed += blockSize_;
    }

    // 逐 block 处理
    size_t samplesProcessed = 0;
    size_t outputWritten = 0;
    std::vector<float> blockIn(blockSize_, 0.0f);
    std::vector<float> blockOut(blockSize_, 0.0f);

    while (outputWritten < static_cast<size_t>(numSamples)) {
        // 填充输入 block（末尾不足补零）
        const size_t remaining = static_cast<size_t>(numSamples) - samplesProcessed;
        const size_t toCopy = std::min(blockSize_, remaining);
        std::copy(input + samplesProcessed, input + samplesProcessed + toCopy,
                  blockIn.begin());
        if (toCopy < blockSize_) {
            std::fill(blockIn.begin() + static_cast<ptrdiff_t>(toCopy), blockIn.end(), 0.0f);
        }

        // 计算当前 block 中心时间对应的 F0 帧
        const double blockCenterSec =
            (static_cast<double>(samplesProcessed) + static_cast<double>(blockSize_) * 0.5) / sampleRate_;
        const int f0Frame = static_cast<int>(blockCenterSec * f0FrameRate);
        const int clampedFrame = std::clamp(f0Frame, 0, numF0Frames - 1);

        // 计算 pitch ratio
        const float origF0 = originalF0[clampedFrame];
        const float corrF0 = correctedF0[clampedFrame];
        double pitchScale = 1.0;
        if (origF0 > 0.0f && corrF0 > 0.0f) {
            pitchScale = static_cast<double>(corrF0) / static_cast<double>(origF0);
        }

        // 设置 pitch 并处理
        shifter_->setPitchScale(pitchScale);
        const float* inPtr = blockIn.data();
        float* outPtr = blockOut.data();
        shifter_->shift(&inPtr, &outPtr);

        // 写入输出（截断到实际需要的长度）
        const size_t toWrite = std::min(blockSize_,
            static_cast<size_t>(numSamples) - outputWritten);
        std::copy(blockOut.begin(), blockOut.begin() + static_cast<ptrdiff_t>(toWrite),
                  output.begin() + static_cast<ptrdiff_t>(outputWritten));

        samplesProcessed += toCopy;
        outputWritten += toWrite;
    }

    return output;
}

} // namespace OpenTune