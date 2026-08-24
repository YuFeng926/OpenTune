#include "AutoTunePitchShifter.h"

namespace OpenTune {

AutoTunePitchShifter::AutoTunePitchShifter(double sampleRate)
    : sampleRate_(sampleRate)
{
    bufferSize_ = kMaxPeriodSamples * 4;
    buffer_.resize(static_cast<size_t>(bufferSize_), 0.0f);
    reset();
}

AutoTunePitchShifter::~AutoTunePitchShifter() = default;

void AutoTunePitchShifter::reset() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    writePos_ = 0;
    inputAddr_ = 0.0;
    // 起始 -1：配合 kLookaheadSamples 预喂，使第 i 次迭代的读地址恰好为 i，
    // 输出与输入严格样本对齐。
    outputAddr_ = -1.0;
    resampleRate_ = 1.0;
    inCrossfade_ = false;
    crossfadeRemaining_ = 0;
}

float AutoTunePitchShifter::readInterpolated(double addr) const {
    double wrapped = std::fmod(addr, static_cast<double>(bufferSize_));
    if (wrapped < 0.0) wrapped += static_cast<double>(bufferSize_);

    const int idx0 = static_cast<int>(wrapped);
    const int idx1 = (idx0 + 1) % bufferSize_;
    const float frac = static_cast<float>(wrapped - static_cast<double>(idx0));

    return buffer_[static_cast<size_t>(idx0)] * (1.0f - frac)
         + buffer_[static_cast<size_t>(idx1)] * frac;
}

void AutoTunePitchShifter::feedSample(float sample) {
    buffer_[static_cast<size_t>(writePos_)] = sample;
    writePos_ = (writePos_ + 1) % bufferSize_;
    inputAddr_ += 1.0;
}

float AutoTunePitchShifter::processSample(double currentPeriod, double targetResampleRate) {
    // Smooth resample rate toward target (retune speed, Claim 9-10)
    resampleRate_ += (targetResampleRate - resampleRate_) * (1.0 - kDecayPerSample);

    // Advance output pointer (Claim 5)
    outputAddr_ += resampleRate_;

    // Cycle correction: overrun/underrun detection (Claim 5)
    const double drift = outputAddr_ - inputAddr_;

    if (drift > currentPeriod) {
        // Overrun: read pointer ahead of write -> skip back one cycle
        crossfadeFromAddr_ = outputAddr_;
        outputAddr_ -= currentPeriod;
        inCrossfade_ = true;
        crossfadeRemaining_ = kCrossfadeSamples;
    } else if (drift < -currentPeriod) {
        // Underrun: read pointer behind write -> skip forward one cycle
        crossfadeFromAddr_ = outputAddr_;
        outputAddr_ += currentPeriod;
        inCrossfade_ = true;
        crossfadeRemaining_ = kCrossfadeSamples;
    }

    // Read output with crossfade at cycle boundaries
    float sample;
    if (inCrossfade_ && crossfadeRemaining_ > 0) {
        const float progress = 1.0f - static_cast<float>(crossfadeRemaining_)
                                      / static_cast<float>(kCrossfadeSamples);
        const float fromSample = readInterpolated(crossfadeFromAddr_);
        const float toSample = readInterpolated(outputAddr_);
        sample = fromSample + (toSample - fromSample) * progress;
        crossfadeFromAddr_ += resampleRate_;
        --crossfadeRemaining_;
        if (crossfadeRemaining_ <= 0) inCrossfade_ = false;
    } else {
        sample = readInterpolated(outputAddr_);
    }

    return sample;
}

std::vector<float> AutoTunePitchShifter::shiftChunk(
    const float* input, int numSamples,
    const float* originalF0, const float* correctedF0,
    int numF0Frames, double f0FrameRate,
    double firstSampleFramePhase,
    const float* lookahead, int numLookaheadSamples)
{
    std::vector<float> output(static_cast<size_t>(numSamples), 0.0f);

    // 拼接发布窗口与前视：读指针消费 inputExt[0 .. numSamples+kLookaheadSamples-1]。
    // 前视不足处补零（仅影响 clip 最后一个 chunk 末尾 5 样本的渲染精度）。
    std::vector<float> inputExt(static_cast<size_t>(numSamples) + kLookaheadSamples, 0.0f);
    std::copy(input, input + numSamples, inputExt.begin());
    if (lookahead != nullptr && numLookaheadSamples > 0)
    {
        std::copy(lookahead,
                  lookahead + std::min(numLookaheadSamples, kLookaheadSamples),
                  inputExt.begin() + numSamples);
    }

    const double samplesPerF0Frame = sampleRate_ / f0FrameRate;

    // 预喂前视样本：先于输出消费未来数据，消除读指针固有群延迟
    for (int i = 0; i < kLookaheadSamples; ++i)
        feedSample(inputExt[static_cast<size_t>(i)]);

    for (int i = 0; i < numSamples; ++i) {
        feedSample(inputExt[static_cast<size_t>(i) + kLookaheadSamples]);

        // Determine which F0 frame this sample belongs to
        const int f0Frame = std::clamp(
            static_cast<int>(std::floor(firstSampleFramePhase + static_cast<double>(i) / samplesPerF0Frame)),
            0, numF0Frames - 1);

        const float origF0 = originalF0[f0Frame];
        const float corrF0 = correctedF0[f0Frame];

        double currentPeriod;
        double targetResampleRate;

        if (origF0 > 0.0f && corrF0 > 0.0f) {
            currentPeriod = sampleRate_ / static_cast<double>(origF0);
            targetResampleRate = static_cast<double>(corrF0) / static_cast<double>(origF0);
        } else {
            // Unvoiced: passthrough
            currentPeriod = static_cast<double>(kMaxPeriodSamples);
            targetResampleRate = 1.0;
        }

        // Clamp period to valid range
        currentPeriod = std::clamp(currentPeriod,
            static_cast<double>(kMinPeriodSamples),
            static_cast<double>(kMaxPeriodSamples));

        output[static_cast<size_t>(i)] = processSample(currentPeriod, targetResampleRate);
    }

    return output;
}

} // namespace OpenTune
