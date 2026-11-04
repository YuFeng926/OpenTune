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
                                          double targetResampleRate,
                                          bool updateResampleRate,
                                          bool snapMode) {
    if (cyclePeriod > 0.0) {
        if (updateResampleRate) {
            if (snapMode) {
                // retuneSpeed=100：直接锁定目标，不走 EMA
                resampleRate_ = targetResampleRate;
            } else {
                resampleRate_ += (targetResampleRate - resampleRate_)
                    * (1.0 - kDecayPerTrackingUpdate);
            }
        }
    }
    else {
        resampleRate_ = 1.0;
    }

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
    const float* lookbehind, int numLookbehindSamples,
    const AutoTunePeriodDetector::DetectedPeriod* detectorPeriods,
    int numDetectorSamples,
    bool snapMode)
{
    std::vector<float> output(static_cast<size_t>(numSamples), 0.0f);
    if (numSamples == 0)
        return output;

    assert(numLookaheadSamples >= 0);
    assert(numLookaheadSamples <= kLookaheadSamples);
    assert(numLookaheadSamples == 0 || lookahead != nullptr);
    assert(numLookbehindSamples >= 0);
    assert(numLookbehindSamples == 0 || lookbehind != nullptr);
    assert(numDetectorSamples >= 0);
    assert(numDetectorSamples == 0 || detectorPeriods != nullptr);
    assert(detectorPeriods == nullptr || numDetectorSamples >= numSamples);

    double largestCyclePeriod = 0.0;
    if (detectorPeriods != nullptr) {
        largestCyclePeriod = static_cast<double>(
            AutoTunePeriodDetector::kMaxFullLag);
        for (int i = 0; i < numSamples && i < numDetectorSamples; ++i) {
            if (detectorPeriods[i].valid)
                largestCyclePeriod = std::max(
                    largestCyclePeriod,
                    static_cast<double>(detectorPeriods[i].periodSamples));
        }
    }
    else {
        for (int i = 0; i < numF0Frames; ++i) {
            if (originalF0[i] > 0.0f)
                largestCyclePeriod = std::max(
                    largestCyclePeriod,
                    sampleRate_ / static_cast<double>(originalF0[i]));
        }
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

        const double corrected = static_cast<double>(correctedF0[f0Frame]);

        // The detector shadow supplies both the measured period and the
        // voiced/unvoiced decision via its valid flag.  The resampler never
        // hard-resets — address continuity is always preserved.
        double cyclePeriod = 0.0;
        double targetRate = 1.0;
        if (detectorPeriods != nullptr) {
            const auto& det = detectorPeriods[i];
            if (det.valid && det.periodSamples > 0.0f) {
                cyclePeriod = static_cast<double>(det.periodSamples);
                // Desired F0 comes from correctedF0 (effectiveF0). The smoothed
                // rate is refreshed only on detector tracking/acquisition
                // update events; hop-held samples keep the established rate.
                // A missing effective target is not UV: keep the detector's
                // measured source pitch as a neutral 1:1 AutoTune target.
                targetRate = corrected > 0.0
                    ? corrected * cyclePeriod / sampleRate_
                    : 1.0;
                output[static_cast<size_t>(i)] =
                    processSample(cyclePeriod, targetRate, det.trackingUpdated, snapMode);
                continue;
            }
            // Detector failure (including unvoiced sections where the detector
            // outputs valid=false): preserve address continuity at rate 1,
            // maintaining resampler state until the next valid detection.
            output[static_cast<size_t>(i)] = processSample(0.0, 1.0, true, snapMode);
            continue;
        }
        else {
            const double original = static_cast<double>(originalF0[f0Frame]);
            if (original > 0.0 && corrected > 0.0) {
                cyclePeriod = sampleRate_ / original;
                targetRate = corrected / original;
            }
        }

        output[static_cast<size_t>(i)] =
            processSample(cyclePeriod, targetRate, true, snapMode);
    }

    return output;
}

} // namespace OpenTune
