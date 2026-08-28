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
// the reference flow permits eps in [0, 0.4]. The wider value is used in coarse
// acquisition to tolerate decimated-domain quantisation error; the stricter
// value is used in full-rate tracking to reject noise.
constexpr double kCoarseEpsilon = 0.4;
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

std::optional<CoarseAcquisition> coarsePeriod(
    const std::vector<float>& samples,
    int endIndex,
    int windowSamples = AutoTunePeriodDetector::kCoarseWindowSamples,
    double fcpeHintPeriod = 0.0)
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

        // Full-rate confirmation: search ±(kDecimFactor/2) around each
        // coarse candidate's full-rate center (8*lag), intersected with
        // [kMinFullLag, kMaxFullLag]. Both candidates are confirmed; the
        // best is chosen by score with an energy-scaled tie margin.
        constexpr int kHalfWindow =
            AutoTunePeriodDetector::kDecimFactor / 2;

        struct FullRateCandidate {
            int lag = 0;
            double score = std::numeric_limits<double>::infinity();
            double energy = 0.0;
        };

        const auto confirmCandidate = [&](int coarseLag)
            -> std::optional<FullRateCandidate>
        {
            const int center =
                coarseLag * AutoTunePeriodDetector::kDecimFactor;
            FullRateCandidate best{};
            for (int lag = center - kHalfWindow;
                 lag <= center + kHalfWindow; ++lag)
            {
                if (lag < AutoTunePeriodDetector::kMinFullLag
                    || lag > AutoTunePeriodDetector::kMaxFullLag)
                    continue;
                const auto eh = computeEHAt(samples, endIndex, lag);
                if (!eh.has_value())
                    continue;
                if (eh->energy < kMinimumEnergy)
                    continue;
                const double v = eh->value();
                if (v > kPeriodicityEpsilon * eh->energy)
                    continue;
                if (v < best.score
                    || (v == best.score && lag < best.lag))
                {
                    best = FullRateCandidate{lag, v, eh->energy};
                }
            }
            return best.lag > 0
                ? std::optional<FullRateCandidate>(best)
                : std::nullopt;
        };

        constexpr double kTieMarginScale = 0.005;
        const auto firstCand = confirmCandidate(firstLag);
        const auto secondCand = confirmCandidate(secondLag);
        if (firstCand.has_value() && secondCand.has_value())
        {
            const double tieMargin =
                kTieMarginScale
                * std::max(firstCand->energy, secondCand->energy);

            // ── Sub-harmonic consistency check ──────────────────────────
            // When secondLag ≈ 2 × firstLag AND the EH scores are within
            // tie margin (a "tie"), the raw scores alone cannot reliably
            // distinguish P+non-stationarity from genuine 2P.  We verify
            // that P is also a good prediction lag inside the 2P window.
            //
            // If P passes the periodicity threshold in the 2P context,
            // the signal is P-periodic → select P.  Otherwise normal
            // tie-breaking applies.
            //
            // Only triggered on "ties" to avoid disrupting weak-fundamental
            // cases where secondCand (the longer period) genuinely wins.
            constexpr double kHarmonicRatioTolerance = 0.15;
            const double ratio = static_cast<double>(secondCand->lag)
                / static_cast<double>(firstCand->lag);
            const bool isHarmonicRelation = std::fabs(ratio - 2.0)
                < kHarmonicRatioTolerance * 2.0;

            const bool scoresTied =
                std::fabs(firstCand->score - secondCand->score) <= tieMargin;

            if (isHarmonicRelation && scoresTied)
            {
                const auto ehAtHalf = computeEHAt(
                    samples, endIndex, firstCand->lag);
                if (ehAtHalf.has_value()
                    && ehAtHalf->energy >= kMinimumEnergy
                    && ehAtHalf->value()
                        <= kPeriodicityEpsilon * ehAtHalf->energy)
                {
                    selectedPeriod = firstCand->lag;
                }
                else
                {
                    if (secondCand->score < firstCand->score)
                        selectedPeriod = secondCand->lag;
                    else
                        selectedPeriod = firstCand->lag;
                }
            }
            else
            {
                if (secondCand->score + tieMargin < firstCand->score)
                    selectedPeriod = secondCand->lag;
                else
                    selectedPeriod = firstCand->lag;
            }
        }
        else if (firstCand.has_value())
            selectedPeriod = firstCand->lag;
        else if (secondCand.has_value())
            selectedPeriod = secondCand->lag;
        else
            return std::nullopt;

        // ── FCPE octave prior ─────────────────────────────────────────
        // When an AI F0 hint is available, verify that the selected period
        // is within ±1 octave of the reference.  If not, pick the confirmed
        // candidate closest to the reference.  This prevents the EH-first
        // heuristic from locking onto a subharmonic when the AI model
        // confidently reports the correct octave.
        if (fcpeHintPeriod > 0.0 && selectedPeriod > 0)
        {
            constexpr double kOctaveRange = 0.5;
            const double ref = fcpeHintPeriod;
            const double selD = static_cast<double>(selectedPeriod);
            if (std::fabs(selD - ref) / ref > kOctaveRange)
            {
                auto pickClosest = [&](int lag) -> bool {
                    if (lag <= 0) return false;
                    const double d = static_cast<double>(lag);
                    return std::fabs(d - ref) / ref <= kOctaveRange;
                };
                if (pickClosest(firstCand.has_value() ? firstCand->lag : 0))
                    selectedPeriod = firstCand->lag;
                else if (pickClosest(secondCand.has_value() ? secondCand->lag : 0))
                    selectedPeriod = secondCand->lag;
                // Both outside ±1 oct → keep EH-selected period (graceful degradation)
            }
        }
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
    const float* fcpeF0Hint,
    int numF0HintFrames,
    double f0HintFrameRate)
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

    // ── FCPE hint interpolation helper ────────────────────────────────
    // Maps a sample-domain endIndex (in the concatenated buffer) to the
    // reference period derived from the FCPE F0 array.  Returns 0 when
    // no hint is available or the interpolated F0 is unvoiced (≤ 0).
    const auto hintPeriodAt = [&](int endIndex) -> double {
        if (fcpeF0Hint == nullptr || numF0HintFrames <= 0
            || f0HintFrameRate <= 0.0)
            return 0.0;
        const double samplePos =
            static_cast<double>(endIndex - prefix);
        const double framePos = samplePos / kReferenceSampleRate * f0HintFrameRate;
        const int frame = std::clamp(
            static_cast<int>(std::floor(framePos)),
            0, numF0HintFrames - 1);
        const float f0 = fcpeF0Hint[frame];
        return f0 > 0.0f ? kReferenceSampleRate / static_cast<double>(f0) : 0.0;
    };

    std::array<EHValue, kTrackingLagCount> tracking{};
    int ehOffset = kMinFullLag;
    bool trackingInitialized = false;
    DetectedPeriod held;
    // Failure-recovery state: after any tracking failure we record the
    // sample index where the failure occurred and the last reliably held
    // period.  On subsequent samples the detector first tries to
    // re-establish tracking using that period (only when the entire EH
    // window is guaranteed to sit in post-failure data), and only falls
    // back to coarse acquisition once kRequiredLookbehindSamples of
    // post-failure history have accumulated.
    int failureSampleIndex = -1;
    double lastReliablePeriod = 0.0;

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

    const auto acquire = [&](int endIndex, bool useLargeWindow) {
        // The public history contract covers the larger coarse window.  Do
        // not silently fall back to the shorter window when that context is
        // incomplete; otherwise kRequiredLookbehindSamples is misleading and
        // chunk-start detection becomes dependent on the fallback path.
        if (endIndex + 1 < kRequiredLookbehindSamples)
            return false;

        const double hintPeriod = hintPeriodAt(endIndex);

        // ── Multi-window cross-validation ─────────────────────────────
        // Run coarse acquisition at two window sizes.  When they agree
        // the result is robust; when they disagree, the FCPE hint
        // (highest-priority octave prior) picks the correct one.
        const auto seedA = coarsePeriod(
            samples, endIndex, kCoarseWindowSamples, hintPeriod);
        const auto seedB = useLargeWindow
            ? coarsePeriod(
                samples, endIndex, kCoarseWindowSamplesLarge, hintPeriod)
            : std::optional<CoarseAcquisition>{};

        const auto selectSeed = [&]() -> std::optional<CoarseAcquisition> {
            if (seedA.has_value() && seedB.has_value())
            {
                if (seedA->selectedPeriod == seedB->selectedPeriod)
                    return seedA;
                // Disagreement: use FCPE hint to pick closer candidate.
                if (hintPeriod > 0.0)
                {
                    const double diffA = std::fabs(
                        static_cast<double>(seedA->selectedPeriod) - hintPeriod);
                    const double diffB = std::fabs(
                        static_cast<double>(seedB->selectedPeriod) - hintPeriod);
                    return diffA <= diffB ? seedA : seedB;
                }
                // No hint: prefer larger window (better low-frequency resolution).
                return seedB;
            }
            return seedA.has_value() ? seedA : seedB;
        };

        const auto seed = selectSeed();
        if (!seed.has_value())
            return false;

        // Seat the nominal center (zero-based kTrackingCenterIndex) on N/2;
        // the surrounding neighborhood may overhang [16, 880] per the
        // patent's EH_OFFSET semantics. initializeTracking validates that
        // center against the full-rate lag bounds.
        const int base = seed->selectedPeriod - kTrackingCenterIndex;
        return initializeTracking(endIndex, base);
    };

    // Unified failure transition: preserve the last reliable period for
    // recovery, clear held, and record the failure position.
    const auto failTracking = [&](int sampleEndIndex,
                                  bool preserveFailureBoundary = false) {
        if (!preserveFailureBoundary && held.valid)
            lastReliablePeriod = held.periodSamples;
        held = {};
        trackingInitialized = false;
        if (!preserveFailureBoundary)
            failureSampleIndex = sampleEndIndex;
    };

    if (prefix > 0 && acquire(prefix - 1, true))
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
        if (!trackingInitialized)
        {
            if (failureSampleIndex < 0)
            {
                // No prior failure: normal initial/post-prefix acquisition.
                if (acquire(endIndex, true))
                {
                    trackingInitialized = true;
                    initializedNow = true;
                }
            }
            else
            {
                // Recovery after tracking failure: search only around the
                // last reliable period using current post-failure samples.
                // This avoids a global coarse relock onto stale pre-failure V
                // while still allowing a small period change at V recovery.
                int recoveryLag = 0;
                double recoveryScore = std::numeric_limits<double>::infinity();
                const int recoveryCenter = static_cast<int>(
                    std::round(lastReliablePeriod));
                for (int lag = recoveryCenter - kTrackingLagCount;
                     lag <= recoveryCenter + kTrackingLagCount; ++lag)
                {
                    if (lag < kMinFullLag || lag > kMaxFullLag)
                        continue;
                    const int base = lag - kTrackingCenterIndex;
                    const int maxLag = base + kTrackingLagCount - 1;
                    if (lastReliablePeriod <= 0.0
                        || endIndex - 2 * maxLag + 1 <= failureSampleIndex)
                        continue;
                    const auto eh = computeEHAt(samples, endIndex, lag);
                    if (!eh.has_value()
                        || !std::isfinite(eh->value())
                        || eh->energy < kMinimumEnergy
                        || eh->value() > kPeriodicityEpsilon * eh->energy)
                        continue;
                    if (eh->value() < recoveryScore)
                    {
                        recoveryScore = eh->value();
                        recoveryLag = lag;
                    }
                }

                if (recoveryLag > 0
                    && initializeTracking(
                        endIndex, recoveryLag - kTrackingCenterIndex))
                {
                    trackingInitialized = true;
                    initializedNow = true;
                    initializedFromRecovery = true;
                }
                else if (endIndex - failureSampleIndex + 1
                         >= kCoarseWindowSamples + kCoarseFilterTaps - 1)
                {
                    // Coarse fallback uses the short window only after its
                    // complete window and FIR history are post-failure.
                    if (acquire(endIndex, false))
                    {
                        trackingInitialized = true;
                        initializedNow = true;
                        initializedFromRecovery = true;
                    }
                }
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
                held.periodSamples = static_cast<float>(refined);
                held.valid = true;
                lastReliablePeriod = held.periodSamples;
                rateUpdated = true;
            }
        }

        result[static_cast<size_t>(i)] = held;
        result[static_cast<size_t>(i)].trackingUpdated = rateUpdated;
    }

    // ── Post-pass: octave-jump correction ──────────────────────────────
    // Mirrors the RMVPEExtractor::fixOctaveErrors pattern: a simple
    // forward + backward scan through consecutive valid frames.  When the
    // period ratio between neighbours is ≈2.0, snap the outlier back.
    // The bidirectional scan ensures that a single misplaced frame
    // (whether it jumped up or dropped down) is caught regardless of
    // scan direction.
    //
    // The tolerance is deliberately tight (±10%) to avoid interfering
    // with normal frequency glides or vibrato, where the period changes
    // smoothly by small ratios between frames.
    {
        constexpr float kOctaveLow = 1.85f;
        constexpr float kOctaveHigh = 2.15f;

        // Forward pass: if curr ≈ 2× prev, curr is wrong → snap to prev.
        for (int i = 1; i < numInputSamples; ++i)
        {
            auto& prev = result[static_cast<size_t>(i - 1)];
            auto& curr = result[static_cast<size_t>(i)];
            if (!prev.valid || !curr.valid)
                continue;
            const float ratio = curr.periodSamples / prev.periodSamples;
            if (ratio > kOctaveLow && ratio < kOctaveHigh)
                curr.periodSamples = prev.periodSamples;
        }

        // Backward pass: if prev ≈ 2× next, prev is wrong → snap to next.
        for (int i = numInputSamples - 2; i >= 0; --i)
        {
            auto& curr = result[static_cast<size_t>(i)];
            auto& next = result[static_cast<size_t>(i + 1)];
            if (!curr.valid || !next.valid)
                continue;
            const float ratio = curr.periodSamples / next.periodSamples;
            if (ratio > kOctaveLow && ratio < kOctaveHigh)
                curr.periodSamples = next.periodSamples;
        }
    }

    return result;
}

} // namespace OpenTune
