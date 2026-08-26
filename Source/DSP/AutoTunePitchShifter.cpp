#include "AutoTunePitchShifter.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace OpenTune {

AutoTunePitchShifter::AutoTunePitchShifter(double sampleRate)
    : sampleRate_(sampleRate)
{
}

bool AutoTunePitchShifter::isReadable(double addr) const {
    const auto first = static_cast<int64_t>(std::floor(addr));
    const auto oldest = static_cast<int64_t>(std::floor(
        std::max(0.0, inputAddr_ - static_cast<double>(bufferSize_))));
    return first >= oldest && static_cast<double>(first + 1) < inputAddr_;
}

float AutoTunePitchShifter::readInterpolated(double addr) const {
    assert(isReadable(addr));

    double wrapped = std::fmod(addr, static_cast<double>(bufferSize_));
    if (wrapped < 0.0)
        wrapped += static_cast<double>(bufferSize_);

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

float AutoTunePitchShifter::processSample(double cyclePeriod,
                                          double targetResampleRate) {
    if (cyclePeriod > 0.0)
        resampleRate_ += (targetResampleRate - resampleRate_) * (1.0 - kDecayPerSample);
    else
        resampleRate_ = 1.0;

    outputAddr_ += resampleRate_;

    if (cyclePeriod > 0.0) {
        if (resampleRate_ > 1.0 && outputAddr_ > inputAddr_) {
            outputAddr_ -= cyclePeriod;
            assert(isReadable(outputAddr_ - kLookaheadSamples));
        }
        else if (resampleRate_ <= 1.0
                 && outputAddr_ + cyclePeriod < inputAddr_) {
            outputAddr_ += cyclePeriod;
            assert(isReadable(outputAddr_ - kLookaheadSamples));
        }
    }

    const double readAddr = outputAddr_ - kLookaheadSamples;
    assert(isReadable(readAddr));
    const float sample = readInterpolated(readAddr);
    return sample;
}

std::vector<float> AutoTunePitchShifter::shiftChunk(
    const float* input, int numSamples,
    const float* originalF0, const float* correctedF0,
    int numF0Frames, double f0FrameRate,
    double firstSampleFramePhase,
    const float* lookahead, int numLookaheadSamples,
    const float* lookbehind, int numLookbehindSamples)
{
    std::vector<float> output(static_cast<size_t>(numSamples), 0.0f);
    if (numSamples == 0)
        return output;

    assert(numLookaheadSamples >= 0);
    assert(numLookaheadSamples <= kLookaheadSamples);
    assert(numLookaheadSamples == 0 || lookahead != nullptr);
    assert(numLookbehindSamples >= 0);
    assert(numLookbehindSamples == 0 || lookbehind != nullptr);

    double largestCyclePeriod = 0.0;
    for (int i = 0; i < numF0Frames; ++i) {
        if (originalF0[i] > 0.0f)
            largestCyclePeriod = std::max(
                largestCyclePeriod,
                sampleRate_ / static_cast<double>(originalF0[i]));
    }

    const int requiredBufferSize = static_cast<int>(std::ceil(largestCyclePeriod))
        + kLookaheadSamples + 2;
    bufferSize_ = requiredBufferSize;
    buffer_.assign(static_cast<size_t>(bufferSize_), 0.0f);
    writePos_ = 0;
    inputAddr_ = 0.0;
    outputAddr_ = static_cast<double>(kLookaheadSamples - 1);
    resampleRate_ = 1.0;

    for (int i = 0; i < numLookbehindSamples; ++i)
        feedSample(lookbehind[i]);

    std::vector<float> inputExt(
        static_cast<size_t>(numSamples) + kLookaheadSamples, 0.0f);
    std::copy(input, input + numSamples, inputExt.begin());
    if (numLookaheadSamples > 0)
        std::copy(lookahead,
                  lookahead + numLookaheadSamples,
                  inputExt.begin() + numSamples);

    const double samplesPerF0Frame = sampleRate_ / f0FrameRate;

    for (int i = 0; i < kLookaheadSamples; ++i)
        feedSample(inputExt[static_cast<size_t>(i)]);

    outputAddr_ = inputAddr_ - 1.0;

    for (int i = 0; i < numSamples; ++i) {
        feedSample(inputExt[static_cast<size_t>(i) + kLookaheadSamples]);

        const int f0Frame = std::clamp(
            static_cast<int>(std::floor(
                firstSampleFramePhase + static_cast<double>(i) / samplesPerF0Frame)),
            0, numF0Frames - 1);

        const double original = static_cast<double>(originalF0[f0Frame]);
        const double corrected = static_cast<double>(correctedF0[f0Frame]);
        const bool voiced = original > 0.0 && corrected > 0.0;
        const double cyclePeriod = voiced ? sampleRate_ / original : 0.0;
        const double targetRate = voiced ? corrected / original : 1.0;

        output[static_cast<size_t>(i)] = processSample(cyclePeriod, targetRate);
    }

    return output;
}

} // namespace OpenTune
