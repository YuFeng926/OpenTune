#include "AutoTunePeriodDetector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace OpenTune {
namespace {

constexpr int kTrackingLagCount = 8;
// Reference Fig. 5A places the nominal center N/2 at 1-based window position
// 4, i.e. zero-based index 3. Per the reference EH_OFFSET semantics the
// eight-lag neighborhood may extend past the full-rate lag bounds; only this
// center index must stay inside [kMinFullLag, kMaxFullLag].
constexpr int kTrackingCenterIndex = kTrackingLagCount / 2 - 1;
constexpr int kTrackingUpdateInterval = 5;
// The patent permits eps in [0, 0.4]; the stricter project setting avoids
// accepting short-window noise minima while retaining the patent test form.
constexpr double kPeriodicityEpsilon = 0.1;
constexpr double kMinimumEnergy = 1.0e-6;
constexpr double kReferenceSampleRate = 44100.0;

struct EHValue {
    double energy = 0.0;
    double correlation = 0.0;

    double value() const
    {
        return energy - 2.0 * correlation;
    }
};

struct CoarseAcquisition {
    int selectedPeriod = 0;
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

bool allVoicedInRange(const std::vector<std::uint8_t>& voiced,
                      int begin, int end)
{
    if (voiced.empty())
        return true;
    if (begin < 0 || end > static_cast<int>(voiced.size()) || begin > end)
        return false;
    for (int i = begin; i < end; ++i)
    {
        if (voiced[static_cast<size_t>(i)] == 0)
            return false;
    }
    return true;
}

std::optional<CoarseAcquisition> coarsePeriod(
    const std::vector<float>& samples,
    const std::vector<std::uint8_t>& voiced,
    int endIndex)
{
    constexpr int kWindow = AutoTunePeriodDetector::kCoarseWindowSamples;
    constexpr int kFilterHistory = AutoTunePeriodDetector::kCoarseFilterTaps - 1;
    if (endIndex + 1 < kWindow + kFilterHistory)
        return std::nullopt;

    const int begin = endIndex - kWindow + 1;
    if (!allVoicedInRange(voiced, begin - kFilterHistory, endIndex + 1))
        return std::nullopt;

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
        return currentValue <= kPeriodicityEpsilon * current.energy
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


    int selectedPeriod = firstLag * AutoTunePeriodDetector::kDecimFactor;
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

        const int firstPeriod = firstLag * AutoTunePeriodDetector::kDecimFactor;
        const int secondPeriod = secondLag * AutoTunePeriodDetector::kDecimFactor;
        const auto firstFull = computeEHAt(samples, endIndex, firstPeriod);
        const auto secondFull = computeEHAt(samples, endIndex, secondPeriod);
        if (!firstFull.has_value() || !secondFull.has_value())
            return std::nullopt;

        selectedPeriod = firstFull->value() <= secondFull->value()
            ? firstPeriod : secondPeriod;
    }

    if (selectedPeriod < AutoTunePeriodDetector::kMinFullLag
        || selectedPeriod > AutoTunePeriodDetector::kMaxFullLag)
        return std::nullopt;
    return CoarseAcquisition{selectedPeriod};
}

} // namespace

std::vector<AutoTunePeriodDetector::DetectedPeriod>
AutoTunePeriodDetector::analyze(
    const float* lookbehind, int numLookbehindSamples,
    const float* input, int numInputSamples,
    double sampleRate,
    const std::uint8_t* lookbehindVoiced,
    const std::uint8_t* inputVoiced)
{
    // The reference flow defines the lag grid for a preferred 44.1 kHz sample rate.
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

    std::vector<std::uint8_t> voiced(static_cast<size_t>(prefix + numInputSamples), 1);
    if (lookbehindVoiced != nullptr)
        std::copy(lookbehindVoiced, lookbehindVoiced + prefix, voiced.begin());
    if (inputVoiced != nullptr)
        std::copy(inputVoiced, inputVoiced + numInputSamples,
                  voiced.begin() + prefix);

    std::array<EHValue, kTrackingLagCount> tracking{};
    int ehOffset = kMinFullLag;
    bool trackingInitialized = false;
    DetectedPeriod held;

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

    const auto voicedForTracking = [&](int endIndex, int base) {
        const int maxLag = base + kTrackingLagCount - 1;
        return allVoicedInRange(voiced, endIndex - 2 * maxLag + 1, endIndex + 1);
    };

    const auto acquire = [&](int endIndex) {
        const auto seed = coarsePeriod(samples, voiced, endIndex);
        if (!seed.has_value())
            return false;

        // Seat the nominal center (zero-based kTrackingCenterIndex) on N/2;
        // the surrounding neighborhood may overhang [16, 880] per the
        // patent's EH_OFFSET semantics. initializeTracking validates that
        // center against the full-rate lag bounds.
        const int base = seed->selectedPeriod - kTrackingCenterIndex;
        return initializeTracking(endIndex, base);
    };

    if (prefix > 0 && acquire(prefix - 1))
        trackingInitialized = true;

    for (int i = 0; i < numInputSamples; ++i)
    {
        const int endIndex = prefix + i;
        // Set only when this sample completes a tracking/interpolation update
        // (initialization or the every-5-samples event); hop-held repeats and
        // failures leave it false.
        bool rateUpdated = false;
        if (voiced[static_cast<size_t>(endIndex)] == 0)
        {
            held = {};
            trackingInitialized = false;
            result[static_cast<size_t>(i)] = held;
            continue;
        }

        bool initializedNow = false;
        if (!trackingInitialized && acquire(endIndex))
        {
            trackingInitialized = true;
            initializedNow = true;
        }

        if (!trackingInitialized)
        {
            held = {};
            result[static_cast<size_t>(i)] = held;
            continue;
        }

        if (!voicedForTracking(endIndex, ehOffset)
            || (!initializedNow && !updateTracking(endIndex)))
        {
            held = {};
            trackingInitialized = false;
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
                held = {};
                trackingInitialized = false;
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
                if (newBase + kTrackingCenterIndex < kMinFullLag
                    || !voicedForTracking(endIndex, newBase))
                {
                    held = {};
                    trackingInitialized = false;
                    result[static_cast<size_t>(i)] = held;
                    continue;
                }
                for (int j = kTrackingLagCount - 1; j > 0; --j)
                    tracking[static_cast<size_t>(j)] =
                        tracking[static_cast<size_t>(j - 1)];
                const auto eh = computeEHAt(samples, endIndex, newBase);
                if (!eh.has_value())
                {
                    held = {};
                    trackingInitialized = false;
                    result[static_cast<size_t>(i)] = held;
                    continue;
                }
                tracking[0] = *eh;
                ehOffset = newBase;
            }
            else if (bestIndex > kTrackingCenterIndex + 1)
            {
                const int newBase = ehOffset + 1;
                if (newBase + kTrackingCenterIndex > kMaxFullLag
                    || !voicedForTracking(endIndex, newBase))
                {
                    held = {};
                    trackingInitialized = false;
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
                    held = {};
                    trackingInitialized = false;
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
                held = {};
                trackingInitialized = false;
                result[static_cast<size_t>(i)] = held;
                continue;
            }

            const EHValue& best = tracking[static_cast<size_t>(bestIndex)];
            if (!std::isfinite(best.value())
                || best.energy < kMinimumEnergy
                || best.value() > kPeriodicityEpsilon * best.energy)
            {
                held = {};
                trackingInitialized = false;
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
                held = {};
                trackingInitialized = false;
            }
            else
            {
                held.periodSamples = static_cast<float>(refined);
                held.valid = true;
                rateUpdated = true;
            }
        }

        result[static_cast<size_t>(i)] = held;
        result[static_cast<size_t>(i)].trackingUpdated = rateUpdated;
    }

    return result;
}

} // namespace OpenTune
