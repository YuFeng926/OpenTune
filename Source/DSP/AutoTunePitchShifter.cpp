#include "AutoTunePitchShifter.h"

#include "PitchShiftRateContract.h"

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
        // A detector tracking update only replaces the applied rate when the
        // request stays inside the ±75 cent light contract. The held rate can
        // absorb a detector transient, but only while the held output stays
        // inside the same contract relative to the requested target. A larger
        // gap makes this render unusable: the caller falls back to the
        // vocoder instead of publishing a target miss.
        if (updateResampleRate) {
            if (isPitchShiftRateWithinLightContract(targetResampleRate)) {
                if (snapMode) {
                    // retuneSpeed=100：直接锁定目标，不走 EMA
                    resampleRate_ = targetResampleRate;
                } else {
                    resampleRate_ += (targetResampleRate - resampleRate_)
                        * (1.0 - kDecayPerTrackingUpdate);
                }
            }
            else if (!isPitchShiftRateWithinLightContract(
                         targetResampleRate / resampleRate_)) {
                rateHoldExceeded_ = true;
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
    return shiftChunkInternal(
        input, numSamples, originalF0, correctedF0, numF0Frames,
        f0FrameRate, firstSampleFramePhase, lookahead, numLookaheadSamples,
        lookbehind, numLookbehindSamples, detectorPeriods, numDetectorSamples,
        snapMode, true);
}

std::vector<float> AutoTunePitchShifter::shiftChunkInternal(
    const float* input, int numSamples,
    const float* originalF0, const float* correctedF0,
    int numF0Frames, double f0FrameRate,
    double firstSampleFramePhase,
    const float* lookahead, int numLookaheadSamples,
    const float* lookbehind, int numLookbehindSamples,
    const AutoTunePeriodDetector::DetectedPeriod* detectorPeriods,
    int numDetectorSamples,
    bool snapMode,
    bool enforceRateContract)
{
    if (numSamples == 0)
        return {};

    rateHoldExceeded_ = false;

    assert(numLookaheadSamples >= 0);
    assert(numLookaheadSamples <= kLookaheadSamples);
    assert(numLookaheadSamples == 0 || lookahead != nullptr);
    assert(numLookbehindSamples >= 0);
    assert(numLookbehindSamples == 0 || lookbehind != nullptr);
    assert(numDetectorSamples >= 0);
    assert(numDetectorSamples == 0 || detectorPeriods != nullptr);
    assert(detectorPeriods == nullptr || numDetectorSamples >= numSamples);

    const double samplesPerF0Frame = sampleRate_ / f0FrameRate;
    const auto frameAt = [&](int sampleIndex) {
        return std::clamp(
            static_cast<int>(std::floor(
                firstSampleFramePhase
                + static_cast<double>(sampleIndex) / samplesPerF0Frame)),
            0, numF0Frames - 1);
    };
    const auto cyclePeriodAt = [&](int sampleIndex) {
        if (detectorPeriods != nullptr) {
            const auto& det = detectorPeriods[sampleIndex];
            return det.valid && det.periodSamples > 0.0f
                ? static_cast<double>(det.periodSamples)
                : 0.0;
        }
        const int f0Frame = frameAt(sampleIndex);
        const double original = static_cast<double>(originalF0[f0Frame]);
        return original > 0.0 && correctedF0[f0Frame] > 0.0f
            ? sampleRate_ / original
            : 0.0;
    };
    const auto targetRateAt = [&](int sampleIndex, double cyclePeriod) {
        if (!(cyclePeriod > 0.0))
            return 1.0;
        const double corrected =
            static_cast<double>(correctedF0[frameAt(sampleIndex)]);
        return corrected > 0.0
            ? corrected * cyclePeriod / sampleRate_
            : 1.0;
    };

    // Public entry validates every rate that actually updates the smoothed
    // resample rate. Detector hop-held samples report trackingUpdated == false
    // and keep the held rate, so their stale period never decides the
    // contract. Without a detector every sample is validated.
    if (enforceRateContract) {
        for (int i = 0; i < numSamples; ++i) {
            if (detectorPeriods != nullptr
                && !detectorPeriods[i].trackingUpdated)
                continue;
            const double cyclePeriod = cyclePeriodAt(i);
            if (!isPitchShiftRateWithinLightContract(
                    targetRateAt(i, cyclePeriod)))
                return {};
        }
    }

    std::vector<float> output(static_cast<size_t>(numSamples), 0.0f);

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

    for (int i = 0; i < kLookaheadSamples; ++i)
        feedSample(inputExt[static_cast<size_t>(i)]);

    outputAddr_ = inputAddr_ - 1.0;

    for (int i = 0; i < numSamples; ++i) {
        feedSample(inputExt[static_cast<size_t>(i) + kLookaheadSamples]);

        // The detector shadow supplies the measured period and the
        // voiced/unvoiced decision via its valid flag. Without a detector the
        // supplied original F0 is the source period. The smoothed rate is
        // refreshed only on detector tracking/acquisition events; hop-held
        // samples keep the established rate.
        const double cyclePeriod = cyclePeriodAt(i);
        const double targetRate = targetRateAt(i, cyclePeriod);
        const bool updateResampleRate = detectorPeriods == nullptr
            ? true
            : detectorPeriods[i].trackingUpdated;
        output[static_cast<size_t>(i)] =
            processSample(cyclePeriod, targetRate, updateResampleRate, snapMode);
    }

    if (rateHoldExceeded_)
        return {};

    return output;
}

AutoTunePitchShifter::OctaveRepairResult
AutoTunePitchShifter::shiftChunkWithOctaveRepair(
    const float* input, int numSamples,
    const float* originalF0, const float* correctedF0,
    int numF0Frames, double f0FrameRate,
    double firstSampleFramePhase,
    const float* lookahead, int numLookaheadSamples,
    const float* lookbehind, int numLookbehindSamples,
    const AutoTunePeriodDetector::DetectedPeriod* detectorPeriods,
    int numDetectorSamples,
    bool snapMode) const
{
    OctaveRepairResult result;
    if (numSamples <= 0)
        return result;

    // 没有完整 detector 影子源时 originalF0 是唯一的周期来源，
    // baseline 渲染即最终结果。
    if (detectorPeriods == nullptr || numDetectorSamples < numSamples) {
        AutoTunePitchShifter shifter(sampleRate_);
        result.audio = shifter.shiftChunk(
            input, numSamples, originalF0, correctedF0, numF0Frames,
            f0FrameRate, firstSampleFramePhase, lookahead,
            numLookaheadSamples, lookbehind, numLookbehindSamples,
            nullptr, 0, snapMode);
        result.success = result.audio.size()
            == static_cast<size_t>(numSamples);
        result.octave = 0;
        return result;
    }

    const double samplesPerF0Frame = sampleRate_ / f0FrameRate;
    const auto frameAt = [&](int sampleIndex) {
        return std::clamp(
            static_cast<int>(std::floor(
                firstSampleFramePhase
                + static_cast<double>(sampleIndex) / samplesPerF0Frame)),
            0, numF0Frames - 1);
    };
    const auto correctedAt = [&](int sampleIndex) {
        return static_cast<double>(correctedF0[frameAt(sampleIndex)]);
    };
    const auto rateAt = [&](int sampleIndex,
                            const AutoTunePeriodDetector::DetectedPeriod* periods) {
        const auto& det = periods[sampleIndex];
        if (!det.valid || det.periodSamples <= 0.0f)
            return 1.0;
        const double corrected = correctedAt(sampleIndex);
        if (!(corrected > 0.0))
            return 1.0;
        return corrected * static_cast<double>(det.periodSamples) / sampleRate_;
    };

    const auto render = [&](const AutoTunePeriodDetector::DetectedPeriod* periods) {
        AutoTunePitchShifter shifter(sampleRate_);
        return shifter.shiftChunkInternal(
            input, numSamples, originalF0, correctedF0, numF0Frames,
            f0FrameRate, firstSampleFramePhase, lookahead,
            numLookaheadSamples, lookbehind, numLookbehindSamples,
            periods, numDetectorSamples, snapMode, false);
    };

    const double threshold = static_cast<double>(kPitchShiftThresholdCents);

    struct Metrics {
        double targetErrorCents = 0.0;
        double continuityCents = 0.0;
        bool hasWindows = false;
        bool hasAnomaly = false;
        std::vector<uint8_t> anomaly;
    };

    const auto measure = [&](const std::vector<float>& audio) {
        Metrics metrics;
        if (audio.size() != static_cast<size_t>(numSamples))
            return metrics;

        metrics.anomaly.assign(static_cast<size_t>(numSamples), 0);

        double absErrorSum = 0.0;
        double continuitySum = 0.0;
        int measurementCount = 0;
        int continuityCount = 0;
        double previousError = 0.0;

        int runStart = 0;
        while (runStart < numSamples) {
            if (!(correctedAt(runStart) > 0.0)
                || !detectorPeriods[runStart].valid) {
                ++runStart;
                continue;
            }
            int runEnd = runStart;
            while (runEnd < numSamples
                   && correctedAt(runEnd) > 0.0
                   && detectorPeriods[runEnd].valid)
                ++runEnd;

            for (int windowStart = runStart; windowStart < runEnd;
                 windowStart += 256) {
                const int windowLength = std::min(512, runEnd - windowStart);
                const double corrected =
                    correctedAt(windowStart + windowLength / 2);
                if (!(corrected > 0.0))
                    continue;
                const double correctedStart = correctedAt(windowStart);
                const double correctedEnd =
                    correctedAt(windowStart + windowLength - 1);
                if (!(correctedStart > 0.0) || !(correctedEnd > 0.0))
                    continue;
                if (std::fabs(1200.0 * std::log2(correctedEnd / correctedStart))
                    > threshold)
                    continue;
                const double targetPeriod = sampleRate_ / corrected;
                if (targetPeriod > 0.45 * static_cast<double>(windowLength))
                    continue;

                const int minLag = std::max(
                    1, static_cast<int>(std::ceil(0.9 * targetPeriod)));
                const int maxLag = std::min(
                    windowLength - 1,
                    static_cast<int>(std::floor(1.1 * targetPeriod)));
                if (maxLag < minLag)
                    continue;

                double bestCorrelation = -1.0;
                int bestLag = 0;
                for (int lag = minLag; lag <= maxLag; ++lag) {
                    double cross = 0.0;
                    double energyA = 0.0;
                    double energyB = 0.0;
                    for (int i = 0; i + lag < windowLength; ++i) {
                        const double a =
                            audio[static_cast<size_t>(windowStart + i)];
                        const double b =
                            audio[static_cast<size_t>(windowStart + i + lag)];
                        cross += a * b;
                        energyA += a * a;
                        energyB += b * b;
                    }
                    const double energy = std::sqrt(energyA * energyB);
                    const double correlation =
                        energy > 0.0 ? cross / energy : 0.0;
                    if (correlation > bestCorrelation) {
                        bestCorrelation = correlation;
                        bestLag = lag;
                    }
                }
                if (bestLag == 0)
                    continue;

                const double error =
                    bestCorrelation < 0.5
                        ? 1200.0
                        : 1200.0 * std::log2(
                              (sampleRate_ / static_cast<double>(bestLag))
                              / corrected);
                const double absError = std::fabs(error);
                absErrorSum += absError;
                ++measurementCount;
                if (measurementCount > 1) {
                    continuitySum += std::fabs(error - previousError);
                    ++continuityCount;
                }
                previousError = error;
                if (absError > threshold) {
                    metrics.hasAnomaly = true;
                    for (int i = windowStart; i < windowStart + windowLength;
                         ++i)
                        metrics.anomaly[static_cast<size_t>(i)] = 1;
                }
            }
            runStart = runEnd;
        }

        metrics.hasWindows = measurementCount > 0;
        if (measurementCount > 0)
            metrics.targetErrorCents =
                absErrorSum / static_cast<double>(measurementCount);
        if (continuityCount > 0)
            metrics.continuityCents =
                continuitySum / static_cast<double>(continuityCount);
        return metrics;
    };

    const auto baselineAudio = render(detectorPeriods);
    const Metrics baselineMetrics = measure(baselineAudio);

    // Only detector tracking-update samples apply a new rate; hop-held samples
    // keep the held rate, so their stale period cannot trigger a repair. A
    // tracking update whose rate leaves the contract triggers octave repair
    // only when scaling the period by 1/2 or 2 would put the rate back inside
    // the ±75 cent contract. Every other excursion is a detection glitch: the
    // applied rate stays held while the held output remains inside the
    // contract; a larger hold miss invalidates the render.
    const auto isOctaveRepairableRate = [](double rate) {
        return isPitchShiftRateWithinLightContract(rate * 0.5)
            || isPitchShiftRateWithinLightContract(rate * 2.0);
    };
    bool sourceOctaveAnomaly = false;
    for (int i = 0; i < numSamples; ++i) {
        if (!detectorPeriods[i].valid || !(correctedAt(i) > 0.0))
            continue;
        if (!detectorPeriods[i].trackingUpdated)
            continue;
        const double rate = rateAt(i, detectorPeriods);
        if (!isPitchShiftRateWithinLightContract(rate)
            && isOctaveRepairableRate(rate)) {
            sourceOctaveAnomaly = true;
            break;
        }
    }

    if (!baselineAudio.empty() && !baselineMetrics.hasAnomaly
        && !sourceOctaveAnomaly) {
        result.audio = baselineAudio;
        result.success = true;
        result.octave = 0;
        result.targetErrorCents = baselineMetrics.targetErrorCents;
        result.continuityCents = baselineMetrics.continuityCents;
        return result;
    }

    std::vector<uint8_t> anomaly(static_cast<size_t>(numSamples), 0);
    if (!baselineAudio.empty())
        anomaly = baselineMetrics.anomaly;

    for (int i = 0; i < numSamples; ++i) {
        if (!detectorPeriods[i].valid)
            continue;
        if (!(correctedAt(i) > 0.0))
            continue;
        const double rate = rateAt(i, detectorPeriods);
        if (!isPitchShiftRateWithinLightContract(rate)
            && isOctaveRepairableRate(rate))
            anomaly[static_cast<size_t>(i)] = 1;
    }

    struct Span {
        int start;
        int end;
    };
    std::vector<Span> spans;
    for (int i = 0; i < numSamples;) {
        if (!anomaly[static_cast<size_t>(i)] || !detectorPeriods[i].valid) {
            ++i;
            continue;
        }
        const int start = i;
        while (i < numSamples
               && anomaly[static_cast<size_t>(i)]
               && detectorPeriods[i].valid)
            ++i;
        spans.push_back({ start, i - 1 });
    }

    OctaveRepairResult best;
    bool hasBest = false;
    double bestScore = 0.0;

    OctaveRepairResult accepted;
    bool hasAccepted = false;
    double acceptedScore = 0.0;

    const auto isBetter = [](double score, double targetError,
                             double otherScore, double otherTargetError) {
        return score < otherScore
            || (score == otherScore && targetError < otherTargetError);
    };

    for (int octave = -1; octave <= 1; ++octave) {
        std::vector<AutoTunePeriodDetector::DetectedPeriod> periods(
            detectorPeriods, detectorPeriods + numDetectorSamples);
        const double scale = std::ldexp(1.0, octave);
        for (const auto& span : spans) {
            for (int i = span.start; i <= span.end; ++i) {
                auto& period = periods[static_cast<size_t>(i)].periodSamples;
                if (period > 0.0f)
                    period = static_cast<float>(
                        static_cast<double>(period) * scale);
            }
            periods[static_cast<size_t>(span.start)].trackingUpdated = true;
            int resume = span.end + 1;
            while (resume < numSamples
                   && (anomaly[static_cast<size_t>(resume)]
                       || !periods[static_cast<size_t>(resume)].valid
                       || periods[static_cast<size_t>(resume)].periodSamples
                           <= 0.0f))
                ++resume;
            if (resume < numSamples)
                periods[static_cast<size_t>(resume)].trackingUpdated = true;
        }

        // Legality follows the requested rate at the forced span-boundary
        // updates plus the detector's own tracking updates. A rate outside the
        // contract invalidates the candidate only when an octave scaling is
        // supposed to repair it; other excursions stay held by the renderer.
        bool legal = true;
        for (int i = 0; i < numSamples; ++i) {
            const auto& period = periods[static_cast<size_t>(i)];
            if (!period.valid)
                continue;
            if (!(correctedAt(i) > 0.0))
                continue;
            if (!period.trackingUpdated)
                continue;
            const double rate = rateAt(i, periods.data());
            if (!isPitchShiftRateWithinLightContract(rate)
                && isOctaveRepairableRate(rate)) {
                legal = false;
                break;
            }
        }
        if (!legal)
            continue;

        auto audio = render(periods.data());
        if (audio.size() != static_cast<size_t>(numSamples))
            continue;

        const Metrics metrics = measure(audio);
        const bool candidateAccepted =
            !metrics.hasWindows || !metrics.hasAnomaly;
        const double score =
            metrics.targetErrorCents + metrics.continuityCents;

        if (!hasBest
            || isBetter(score, metrics.targetErrorCents,
                        bestScore, best.targetErrorCents)) {
            hasBest = true;
            bestScore = score;
            best.octave = octave;
            best.targetErrorCents = metrics.targetErrorCents;
            best.continuityCents = metrics.continuityCents;
        }

        if (candidateAccepted
            && (!hasAccepted
                || isBetter(score, metrics.targetErrorCents,
                            acceptedScore, accepted.targetErrorCents))) {
            hasAccepted = true;
            acceptedScore = score;
            accepted.audio = std::move(audio);
            accepted.success = true;
            accepted.octave = octave;
            accepted.targetErrorCents = metrics.targetErrorCents;
            accepted.continuityCents = metrics.continuityCents;
        }
    }

    if (hasAccepted)
        return accepted;

    result.audio.clear();
    result.success = false;
    if (hasBest) {
        result.octave = best.octave;
        result.targetErrorCents = best.targetErrorCents;
        result.continuityCents = best.continuityCents;
    }
    return result;
}

} // namespace OpenTune
