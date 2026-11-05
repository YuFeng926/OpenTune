#include "AutoTunePeriodDetector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace OpenTune {
namespace {

constexpr int kTrackingLagCount = 8;
// The nominal center N/2 sits at 1-based window position 4, i.e. zero-based
// index 3. Per the EH_OFFSET semantics the eight-lag neighborhood may extend
// past the full-rate lag bounds; only this center index must stay inside
// [kMinFullLag, kMaxFullLag].
constexpr int kTrackingCenterIndex = kTrackingLagCount / 2 - 1;
constexpr int kTrackingUpdateInterval = 5;
// eps in [0, 0.4]. The wider value is used in coarse acquisition to tolerate
// acquisition to tolerate decimated-domain quantisation error; the stricter
// value is used in full-rate tracking to reject noise.
constexpr double kCoarseEpsilon = 0.4;
constexpr double kPeriodicityEpsilon = 0.1;
constexpr double kMinimumEnergy = 1.0e-6;
constexpr double kReferenceSampleRate = 44100.0;
// EH tie margin relative to candidate energy: hint ordering applies only
// when the waveform evidence of two qualified candidates is indistinguishable
// at this scale.
constexpr double kPeriodicityTieMargin = 0.005;

struct EHValue {
    double energy = 0.0;
    double correlation = 0.0;

    double value() const
    {
        return energy - 2.0 * correlation;
    }
};

struct PeriodCandidate {
    int lag = 0;
    double score = std::numeric_limits<double>::infinity();
    double energy = 0.0;
};

std::optional<EHValue> computeEHAt(
    const std::vector<float>& samples, int endIndex, int lag)
{
    if (lag <= 0 || endIndex < 2 * lag - 1)
        return std::nullopt;

    EHValue result;
    const int energyBegin = endIndex - 2 * lag + 1;
    const int correlationBegin = endIndex - lag + 1;
    for (int i = energyBegin; i <= endIndex; ++i)
    {
        const double value = static_cast<double>(samples[static_cast<size_t>(i)]);
        result.energy += value * value;
    }
    for (int i = correlationBegin; i <= endIndex; ++i)
    {
        result.correlation += static_cast<double>(
            samples[static_cast<size_t>(i)])
            * static_cast<double>(samples[static_cast<size_t>(i - lag)]);
    }
    return result;
}

std::optional<PeriodCandidate> confirmFullRateCandidate(
    const std::vector<float>& samples, int endIndex, int centerLag,
    int halfWindow)
{
    const int firstLag = std::max(
        centerLag - halfWindow, AutoTunePeriodDetector::kMinFullLag);
    const int lastLag = std::min(
        centerLag + halfWindow, AutoTunePeriodDetector::kMaxFullLag);

    std::optional<PeriodCandidate> best;
    for (int lag = firstLag; lag <= lastLag; ++lag)
    {
        const auto eh = computeEHAt(samples, endIndex, lag);
        if (!eh.has_value()
            || eh->energy < kMinimumEnergy
            || eh->value() > kPeriodicityEpsilon * eh->energy)
            continue;

        if (!best.has_value()
            || eh->value() < best->score
            || (eh->value() == best->score && lag < best->lag))
        {
            best = PeriodCandidate{lag, eh->value(), eh->energy};
        }
    }
    return best;
}

std::optional<PeriodCandidate> chooseCandidate(
    std::optional<PeriodCandidate> first,
    std::optional<PeriodCandidate> second,
    double hintPeriod)
{
    if (!first.has_value())
        return second;
    if (!second.has_value())
        return first;

    const double tieMargin = kPeriodicityTieMargin
        * std::max(first->energy, second->energy);
    if (std::fabs(first->score - second->score) > tieMargin)
    {
        return first->score < second->score ? first : second;
    }

    if (hintPeriod > 0.0)
    {
        const double firstDistance = std::fabs(
            static_cast<double>(first->lag) - hintPeriod);
        const double secondDistance = std::fabs(
            static_cast<double>(second->lag) - hintPeriod);
        if (firstDistance != secondDistance)
            return firstDistance < secondDistance ? first : second;
    }

    return first->lag <= second->lag ? first : second;
}

bool hasOctaveDivergence(double period, double hintPeriod)
{
    constexpr double kUpperLow = 1.8;
    constexpr double kUpperHigh = 2.2;
    constexpr double kLowerLow = 0.45;
    constexpr double kLowerHigh = 0.55;
    const double ratio = period / hintPeriod;
    return (ratio > kUpperLow && ratio < kUpperHigh)
        || (ratio > kLowerLow && ratio < kLowerHigh);
}

const std::array<double, AutoTunePeriodDetector::kCoarseFilterTaps>&
coarseFilterCoefficients()
{
    static const auto coefficients = [] {
        std::array<double, AutoTunePeriodDetector::kCoarseFilterTaps> result{};
        constexpr int taps = AutoTunePeriodDetector::kCoarseFilterTaps;
        constexpr int center = (taps - 1) / 2;
        constexpr double cutoff = 0.5 / AutoTunePeriodDetector::kDecimFactor;

        double sum = 0.0;
        for (int i = 0; i < taps; ++i)
        {
            const double offset = static_cast<double>(i - center);
            const double sinc = std::abs(offset) < 1.0e-12
                ? 2.0 * cutoff
                : std::sin(2.0 * 3.14159265358979323846 * cutoff * offset)
                    / (3.14159265358979323846 * offset);
            const double window = 0.54
                - 0.46 * std::cos(
                    2.0 * 3.14159265358979323846 * i / (taps - 1));
            result[static_cast<size_t>(i)] = sinc * window;
            sum += result[static_cast<size_t>(i)];
        }

        for (double& coefficient : result)
            coefficient /= sum;
        return result;
    }();
    return coefficients;
}

double lowPassAt(const std::vector<float>& samples, int index)
{
    const auto& coefficients = coarseFilterCoefficients();
    double value = 0.0;
    for (int i = 0; i < AutoTunePeriodDetector::kCoarseFilterTaps; ++i)
    {
        const int source = index - i;
        if (source < 0)
            return std::numeric_limits<double>::quiet_NaN();
        value += coefficients[static_cast<size_t>(i)]
            * static_cast<double>(samples[static_cast<size_t>(source)]);
    }
    return value;
}

std::optional<PeriodCandidate> coarsePeriod(
    const std::vector<float>& samples,
    int endIndex,
    int windowSamples = AutoTunePeriodDetector::kCoarseWindowSamples,
    double hintPeriod = 0.0)
{
    const int kWindow = windowSamples;
    constexpr int kFilterHistory = AutoTunePeriodDetector::kCoarseFilterTaps - 1;
    if (endIndex + 1 < kWindow + kFilterHistory)
        return std::nullopt;

    const int begin = endIndex - kWindow + 1;

    // Keep the decimation phase fixed in the concatenated input stream.
    const int firstDecimated = begin
        + (AutoTunePeriodDetector::kDecimFactor
            - begin % AutoTunePeriodDetector::kDecimFactor)
            % AutoTunePeriodDetector::kDecimFactor;
    std::vector<float> decimated;
    for (int index = firstDecimated; index <= endIndex;
         index += AutoTunePeriodDetector::kDecimFactor)
    {
        const double filtered = lowPassAt(samples, index);
        if (!std::isfinite(filtered))
            return std::nullopt;
        decimated.push_back(static_cast<float>(filtered));
    }

    std::array<EHValue, AutoTunePeriodDetector::kMaxDecimLag + 1> values{};
    std::array<bool, AutoTunePeriodDetector::kMaxDecimLag + 1> available{};
    for (int lag = AutoTunePeriodDetector::kMinDecimLag;
         lag <= AutoTunePeriodDetector::kMaxDecimLag; ++lag)
    {
        if (const auto eh = computeEHAt(
                decimated, static_cast<int>(decimated.size()) - 1, lag);
            eh.has_value())
        {
            values[static_cast<size_t>(lag)] = *eh;
            available[static_cast<size_t>(lag)] = true;
        }
    }

    const auto isCandidate = [&](int lag) {
        if (lag <= AutoTunePeriodDetector::kMinDecimLag
            || lag >= AutoTunePeriodDetector::kMaxDecimLag
            || !available[static_cast<size_t>(lag - 1)]
            || !available[static_cast<size_t>(lag)]
            || !available[static_cast<size_t>(lag + 1)])
            return false;

        const EHValue& current = values[static_cast<size_t>(lag)];
        const double currentValue = current.value();
        return currentValue <= kCoarseEpsilon * current.energy
            && currentValue <= values[static_cast<size_t>(lag - 1)].value()
            && currentValue <= values[static_cast<size_t>(lag + 1)].value();
    };

    int firstLag = 0;
    for (int lag = AutoTunePeriodDetector::kMinDecimLag + 1;
         lag < AutoTunePeriodDetector::kMaxDecimLag; ++lag)
    {
        if (isCandidate(lag))
        {
            firstLag = lag;
            break;
        }
    }
    if (firstLag == 0)
        return std::nullopt;


    constexpr int kHalfWindow = AutoTunePeriodDetector::kDecimFactor / 2;
    const auto firstCandidate = confirmFullRateCandidate(
        samples, endIndex,
        firstLag * AutoTunePeriodDetector::kDecimFactor, kHalfWindow);

    std::optional<PeriodCandidate> secondCandidate;
    if (firstLag < AutoTunePeriodDetector::kMaxDecimLag / 2)
    {
        int secondLag = 0;
        for (int lag = firstLag + 1;
             lag < AutoTunePeriodDetector::kMaxDecimLag; ++lag)
        {
            if (isCandidate(lag))
            {
                secondLag = lag;
                break;
            }
        }
        if (secondLag == 0)
            return std::nullopt;
        secondCandidate = confirmFullRateCandidate(
            samples, endIndex,
            secondLag * AutoTunePeriodDetector::kDecimFactor, kHalfWindow);
    }

    return chooseCandidate(firstCandidate, secondCandidate, hintPeriod);
}

} // namespace

std::vector<AutoTunePeriodDetector::DetectedPeriod>
AutoTunePeriodDetector::analyze(
    const float* lookbehind, int numLookbehindSamples,
    const float* input, int numInputSamples,
    double sampleRate,
    const float* fcpeF0Hint,
    int numF0HintFrames,
    double f0HintFrameRate,
    double firstSampleFramePhase)
{
    // The lag grid is defined for a preferred 44.1 kHz sample rate.
    // These constants are sample-domain values and are intentionally not
    // rescaled for other rates.
    (void)sampleRate;
    static_assert(kReferenceSampleRate == 44100.0);

    std::vector<DetectedPeriod> result(static_cast<size_t>(
        std::max(0, numInputSamples)));
    if (input == nullptr || numInputSamples <= 0)
        return result;

    const int prefix = (lookbehind != nullptr && numLookbehindSamples > 0)
        ? numLookbehindSamples : 0;
    std::vector<float> samples(static_cast<size_t>(prefix + numInputSamples), 0.0f);
    if (prefix > 0)
        std::copy(lookbehind, lookbehind + prefix, samples.begin());
    std::copy(input, input + numInputSamples, samples.begin() + prefix);

    // ── FCPE hint lookup helper ───────────────────────────────────────
    // Maps a sample-domain endIndex (in the concatenated buffer) to the
    // reference period derived from the FCPE F0 array.  firstSampleFramePhase
    // is the float frame position of input[0] in the supplied array, so the
    // lookup indexes the true global frame grid.  A position outside
    // [0, numF0HintFrames) has no hint.  Returns 0 when no hint is available
    // or the addressed F0 is unvoiced (≤ 0).
    const auto hintPeriodAt = [&](int endIndex) -> double {
        if (fcpeF0Hint == nullptr || numF0HintFrames <= 0
            || f0HintFrameRate <= 0.0)
            return 0.0;
        const double samplePos =
            static_cast<double>(endIndex - prefix);
        const double framePos = firstSampleFramePhase
            + samplePos / kReferenceSampleRate * f0HintFrameRate;
        if (framePos < 0.0
            || framePos >= static_cast<double>(numF0HintFrames))
            return 0.0;
        const int frame = static_cast<int>(framePos);
        const float f0 = fcpeF0Hint[frame];
        return f0 > 0.0f ? kReferenceSampleRate / static_cast<double>(f0) : 0.0;
    };

    std::array<EHValue, kTrackingLagCount> tracking{};
    int ehOffset = kMinFullLag;
    bool trackingInitialized = false;
    DetectedPeriod held;
    // Failure-recovery boundary: tracking re-acquisition may not use samples
    // before the sample where tracking failed.
    int failureSampleIndex = -1;

    const auto initializeTracking = [&](int endIndex, int base) {
        const int centerLag = base + kTrackingCenterIndex;
        if (centerLag < kMinFullLag || centerLag > kMaxFullLag)
            return false;
        for (int i = 0; i < kTrackingLagCount; ++i)
        {
            const auto eh = computeEHAt(samples, endIndex, base + i);
            if (!eh.has_value())
                return false;
            tracking[static_cast<size_t>(i)] = *eh;
        }
        ehOffset = base;
        return true;
    };

    const auto updateTracking = [&](int endIndex) {
        for (int i = 0; i < kTrackingLagCount; ++i)
        {
            const int lag = ehOffset + i;
            const int oldest = endIndex - 2 * lag;
            if (oldest < 0)
                return false;

            const double newestValue = static_cast<double>(
                samples[static_cast<size_t>(endIndex)]);
            const double oldestValue = static_cast<double>(
                samples[static_cast<size_t>(oldest)]);
            const double delayedValue = static_cast<double>(
                samples[static_cast<size_t>(endIndex - lag)]);
            tracking[static_cast<size_t>(i)].energy +=
                newestValue * newestValue - oldestValue * oldestValue;
            tracking[static_cast<size_t>(i)].correlation +=
                newestValue * delayedValue - delayedValue * oldestValue;
        }
        return true;
    };

    // Unified coarse -> full-rate acquisition for initial lock and every
    // failure recovery.  maxWindowStart excludes pre-failure waveform samples
    // during recovery.  The short and large coarse windows are cross-checked
    // when the large window is fully inside the allowed region: identical
    // lags keep the short seed, divergent lags select the seed closer to the
    // FCPE hint, and without a hint the large window wins because it
    // resolves low frequencies better.  A single available seed is used
    // directly; no seed fails acquisition.
    const auto acquire = [&](int endIndex, int maxWindowStart) {
        // The public history contract covers the larger coarse window.
        if (endIndex + 1 < kRequiredLookbehindSamples)
            return false;

        const int largeWindowStart = endIndex
            - kCoarseWindowSamplesLarge - (kCoarseFilterTaps - 1) + 1;
        const int shortWindowStart = endIndex
            - kCoarseWindowSamples - (kCoarseFilterTaps - 1) + 1;
        const bool useLargeWindow = largeWindowStart >= maxWindowStart;
        if (!useLargeWindow && shortWindowStart < maxWindowStart)
            return false;

        const double hintPeriod = hintPeriodAt(endIndex);

        // Seat the nominal center (zero-based kTrackingCenterIndex) on N/2;
        // the surrounding neighborhood may overhang [16, 880] per the
        // EH_OFFSET semantics. initializeTracking validates that center
        // against the full-rate lag bounds.
        const auto shortSeed = coarsePeriod(
            samples, endIndex, kCoarseWindowSamples, hintPeriod);
        std::optional<PeriodCandidate> largeSeed;
        if (useLargeWindow)
        {
            largeSeed = coarsePeriod(
                samples, endIndex, kCoarseWindowSamplesLarge, hintPeriod);
        }

        std::optional<PeriodCandidate> seed = shortSeed;
        if (shortSeed.has_value() && largeSeed.has_value())
        {
            if (shortSeed->lag == largeSeed->lag)
            {
                seed = shortSeed;
            }
            else if (hintPeriod > 0.0)
            {
                const double shortDistance = std::fabs(
                    static_cast<double>(shortSeed->lag) - hintPeriod);
                const double largeDistance = std::fabs(
                    static_cast<double>(largeSeed->lag) - hintPeriod);
                seed = shortDistance <= largeDistance ? shortSeed : largeSeed;
            }
            else
            {
                seed = largeSeed;
            }
        }
        else if (largeSeed.has_value())
        {
            seed = largeSeed;
        }

        if (!seed.has_value())
            return false;

        return initializeTracking(
            endIndex, seed->lag - kTrackingCenterIndex);
    };

    // Unified failure transition: clear held and record the failure position.
    const auto failTracking = [&](int sampleEndIndex,
                                  bool preserveFailureBoundary = false) {
        held = {};
        trackingInitialized = false;
        if (!preserveFailureBoundary)
            failureSampleIndex = sampleEndIndex;
    };

    if (prefix > 0 && acquire(prefix - 1, 0))
        trackingInitialized = true;

    for (int i = 0; i < numInputSamples; ++i)
    {
        const int endIndex = prefix + i;
        // Set only when this sample completes a tracking/interpolation update
        // (initialization or the every-5-samples event); hop-held repeats and
        // failures leave it false.
        bool rateUpdated = false;

        bool initializedNow = false;
        bool initializedFromRecovery = false;
        if (!trackingInitialized && i % kTrackingUpdateInterval == 0)
        {
            // Initial lock and failure recovery share the same coarse ->
            // full-rate acquisition on the existing 5-sample cadence.  During
            // recovery no waveform sample before the failure sample is used
            // as evidence.
            const int maxWindowStart = failureSampleIndex >= 0
                ? failureSampleIndex : 0;
            if (acquire(endIndex, maxWindowStart))
            {
                trackingInitialized = true;
                initializedNow = true;
                initializedFromRecovery = failureSampleIndex >= 0;
            }
        }

        if (!trackingInitialized)
        {
            held = {};
            result[static_cast<size_t>(i)] = held;
            continue;
        }

        if (!initializedNow && !updateTracking(endIndex))
        {
            failTracking(endIndex, initializedFromRecovery);
            result[static_cast<size_t>(i)] = held;
            continue;
        }

        if (initializedNow || i % kTrackingUpdateInterval == 0)
        {
            int bestIndex = 0;
            for (int j = 1; j < kTrackingLagCount; ++j)
            {
                if (tracking[static_cast<size_t>(j)].value()
                    < tracking[static_cast<size_t>(bestIndex)].value())
                    bestIndex = j;
            }

            // Lmin == 1 or Lmin == N is a tracking failure in Figure 5A.
            if (bestIndex == 0 || bestIndex == kTrackingLagCount - 1)
            {
                failTracking(endIndex, initializedFromRecovery);
                result[static_cast<size_t>(i)] = held;
                continue;
            }

            // Figure 5B moves the eight-lag window by exactly one index.
            // Each slide re-centers the nominal center lag; only that lag
            // (newBase + kTrackingCenterIndex) must stay in [16, 880], not
            // the whole neighborhood.
            if (bestIndex < kTrackingCenterIndex)
            {
                const int newBase = ehOffset - 1;
                if (newBase + kTrackingCenterIndex < kMinFullLag)
                {
                    failTracking(endIndex, initializedFromRecovery);
                    result[static_cast<size_t>(i)] = held;
                    continue;
                }
                for (int j = kTrackingLagCount - 1; j > 0; --j)
                    tracking[static_cast<size_t>(j)] =
                        tracking[static_cast<size_t>(j - 1)];
                const auto eh = computeEHAt(samples, endIndex, newBase);
                if (!eh.has_value())
                {
                    failTracking(endIndex, initializedFromRecovery);
                    result[static_cast<size_t>(i)] = held;
                    continue;
                }
                tracking[0] = *eh;
                ehOffset = newBase;
            }
            else if (bestIndex > kTrackingCenterIndex + 1)
            {
                const int newBase = ehOffset + 1;
                if (newBase + kTrackingCenterIndex > kMaxFullLag)
                {
                    failTracking(endIndex, initializedFromRecovery);
                    result[static_cast<size_t>(i)] = held;
                    continue;
                }
                for (int j = 0; j < kTrackingLagCount - 1; ++j)
                    tracking[static_cast<size_t>(j)] =
                        tracking[static_cast<size_t>(j + 1)];
                const auto eh = computeEHAt(
                    samples, endIndex, newBase + kTrackingLagCount - 1);
                if (!eh.has_value())
                {
                    failTracking(endIndex, initializedFromRecovery);
                    result[static_cast<size_t>(i)] = held;
                    continue;
                }
                tracking[kTrackingLagCount - 1] = *eh;
                ehOffset = newBase;
            }

            bestIndex = 0;
            for (int j = 1; j < kTrackingLagCount; ++j)
            {
                if (tracking[static_cast<size_t>(j)].value()
                    < tracking[static_cast<size_t>(bestIndex)].value())
                    bestIndex = j;
            }
            if (bestIndex == 0 || bestIndex == kTrackingLagCount - 1)
            {
                failTracking(endIndex, initializedFromRecovery);
                result[static_cast<size_t>(i)] = held;
                continue;
            }

            const EHValue& best = tracking[static_cast<size_t>(bestIndex)];
            if (!std::isfinite(best.value())
                || best.energy < kMinimumEnergy
                || best.value() > kPeriodicityEpsilon * best.energy)
            {
                failTracking(endIndex, initializedFromRecovery);
                result[static_cast<size_t>(i)] = held;
                continue;
            }

            double refined = static_cast<double>(ehOffset + bestIndex);
            const double vm1 = tracking[static_cast<size_t>(bestIndex - 1)].value();
            const double v0 = best.value();
            const double vp1 = tracking[static_cast<size_t>(bestIndex + 1)].value();
            const double denominator = vm1 - 2.0 * v0 + vp1;
            if (std::isfinite(denominator) && denominator != 0.0)
            {
                const double delta = 0.5 * (vm1 - vp1) / denominator;
                if (std::isfinite(delta))
                    refined += delta;
            }

            if (refined < kMinFullLag || refined > kMaxFullLag)
            {
                failTracking(endIndex, initializedFromRecovery);
            }
            else
            {
                // Stable-tracking octave check: when the hint and the tracked
                // period clearly disagree by an octave, confirm both around
                // the same waveform position with the shared full-rate
                // helper.  The tracked period is kept when its waveform
                // evidence wins; a qualified hint-side candidate re-seats the
                // tracking window.  A stale hint cannot reject the waveform.
                const double hintPeriod = hintPeriodAt(endIndex);
                if (hintPeriod > 0.0
                    && hasOctaveDivergence(refined, hintPeriod))
                {
                    constexpr int kConfirmHalfWindow =
                        kDecimFactor / 2;
                    const auto currentCandidate = confirmFullRateCandidate(
                        samples, endIndex,
                        static_cast<int>(std::lround(refined)),
                        kConfirmHalfWindow);
                    const auto hintCandidate = confirmFullRateCandidate(
                        samples, endIndex,
                        static_cast<int>(std::lround(hintPeriod)),
                        kConfirmHalfWindow);
                    const auto selected = chooseCandidate(
                        currentCandidate, hintCandidate, hintPeriod);

                    if (selected.has_value()
                        && (!currentCandidate.has_value()
                            || selected->lag != currentCandidate->lag))
                    {
                        if (initializeTracking(
                                endIndex,
                                selected->lag - kTrackingCenterIndex))
                            refined = static_cast<double>(selected->lag);
                        else
                            failTracking(endIndex, initializedFromRecovery);
                    }
                }

                if (trackingInitialized)
                {
                    held.periodSamples = static_cast<float>(refined);
                    held.valid = true;
                    rateUpdated = true;
                }
            }
        }

        result[static_cast<size_t>(i)] = held;
        result[static_cast<size_t>(i)].trackingUpdated = rateUpdated;
    }

    return result;
}

} // namespace OpenTune
