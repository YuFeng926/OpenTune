// ==============================================================================
// Tests/AutoTunePitchShifterTests.cpp
//
// Self-contained standard-library tests for OpenTune::AutoTunePitchShifter.
// Deliberately free of JUCE / Tests/TestSupport dependencies so it builds and
// runs standalone via the OPENTUNE_BUILD_TESTS CMake option.
// ==============================================================================

#include "DSP/AutoTunePeriodDetector.h"
#include "DSP/AutoTunePitchShifter.h"
#include "DSP/PitchShiftRateContract.h"
#include "Inference/ChunkRenderStrategy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

using Shifter = OpenTune::AutoTunePitchShifter;

constexpr double kTwoPi = 6.283185307179586;

int g_failures = 0;

void expectTrue(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

int countPositiveZeroCrossings(const std::vector<float>& signal,
                               int beginIndex, int endIndex)
{
    int crossings = 0;
    for (int i = beginIndex + 1; i < endIndex; ++i) {
        if (signal[static_cast<size_t>(i - 1)] <= 0.0f
            && signal[static_cast<size_t>(i)] > 0.0f)
            ++crossings;
    }
    return crossings;
}

// ---------------------------------------------------------------------------
// Deterministic signal generators (LCG instead of <random>: stable everywhere)
// ---------------------------------------------------------------------------

std::vector<float> makeNoise(int numSamples, uint32_t seed)
{
    std::vector<float> out(static_cast<size_t>(numSamples));
    uint32_t state = seed;
    for (int i = 0; i < numSamples; ++i) {
        state = state * 1664525u + 1013904223u;
        const uint32_t r = (state >> 8) & 0xFFFFu; // 0..65535
        out[static_cast<size_t>(i)] =
            (static_cast<float>(r) / 32768.0f - 1.0f) * 0.8f; // about +/-0.8
    }
    return out;
}

std::vector<float> makeSine(double freqHz, double sampleRate, int numSamples, double amp)
{
    std::vector<float> out(static_cast<size_t>(numSamples));
    for (int i = 0; i < numSamples; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        out[static_cast<size_t>(i)] =
            static_cast<float>(amp * std::sin(kTwoPi * freqHz * t));
    }
    return out;
}

std::vector<float> makeSineSegment(double freqHz, double sampleRate,
                                   int firstSample, int numSamples, double amp)
{
    std::vector<float> out(static_cast<size_t>(numSamples));
    for (int i = 0; i < numSamples; ++i) {
        const double sample = static_cast<double>(firstSample + i);
        out[static_cast<size_t>(i)] = static_cast<float>(
            amp * std::sin(kTwoPi * freqHz * sample / sampleRate));
    }
    return out;
}

// Phase-continuous sine in the period domain: holds startPeriod samples for
// holdLeadingSamples, ramps the instantaneous period linearly to endPeriod
// over glideSamples, then holds endPeriod for holdTrailingSamples. Exact
// per-sample periods make detector boundary assertions deterministic.
std::vector<float> makeHoldGlideHoldSine(double startPeriod, double endPeriod,
                                         int holdLeadingSamples,
                                         int glideSamples,
                                         int holdTrailingSamples, double amp)
{
    const int total = holdLeadingSamples + glideSamples + holdTrailingSamples;
    std::vector<float> out(static_cast<size_t>(total));
    double phase = 0.0;
    for (int i = 0; i < total; ++i) {
        double period = startPeriod;
        if (i >= holdLeadingSamples + glideSamples) {
            period = endPeriod;
        } else if (i >= holdLeadingSamples) {
            const double t =
                static_cast<double>(i - holdLeadingSamples)
                / static_cast<double>(glideSamples);
            period = startPeriod + (endPeriod - startPeriod) * t;
        }
        phase += kTwoPi / period;
        out[static_cast<size_t>(i)] =
            static_cast<float>(amp * std::sin(phase));
    }
    return out;
}

// Linear frequency glide; also fills originalF0 with the instantaneous
// frequency at each frame centre so the shifter gets honest guidance.
std::vector<float> makeGlide(double fStartHz, double fEndHz, double sampleRate,
                             int numSamples, int numFrames, double f0FrameRate,
                             double amp, std::vector<float>* originalF0)
{
    const double duration = static_cast<double>(numSamples) / sampleRate;
    auto instFreq = [&](double t) {
        return fStartHz + (fEndHz - fStartHz) * (t / duration);
    };

    originalF0->assign(static_cast<size_t>(numFrames), 0.0f);
    for (int k = 0; k < numFrames; ++k) {
        const double frameCentreT =
            (static_cast<double>(k) + 0.5) / f0FrameRate;
        (*originalF0)[static_cast<size_t>(k)] =
            static_cast<float>(instFreq(frameCentreT));
    }

    std::vector<float> out(static_cast<size_t>(numSamples));
    double phase = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        phase += kTwoPi * instFreq(t) / sampleRate;
        out[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(phase));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

bool allFinite(const std::vector<float>& v)
{
    return std::all_of(v.begin(), v.end(), [](float s) { return std::isfinite(s); });
}

double maxAbs(const std::vector<float>& v)
{
    double m = 0.0;
    for (float s : v)
        m = std::max(m, static_cast<double>(std::fabs(s)));
    return m;
}

double maxAdjacentDelta(const std::vector<float>& v)
{
    double m = 0.0;
    for (size_t i = 1; i < v.size(); ++i)
        m = std::max(m, static_cast<double>(std::fabs(v[i] - v[i - 1])));
    return m;
}

double meanAbsRange(const std::vector<float>& v, int beginIndex, int endIndex)
{
    if (endIndex <= beginIndex)
        return 0.0;
    double sum = 0.0;
    for (int i = beginIndex; i < endIndex; ++i)
        sum += std::fabs(static_cast<double>(v[static_cast<size_t>(i)]));
    return sum / static_cast<double>(endIndex - beginIndex);
}

double meanAbsDiffRange(const std::vector<float>& a, const std::vector<float>& b,
                        int beginIndex, int endIndex)
{
    if (endIndex <= beginIndex)
        return 0.0;
    double sum = 0.0;
    for (int i = beginIndex; i < endIndex; ++i) {
        sum += std::fabs(static_cast<double>(a[static_cast<size_t>(i)])
                       - static_cast<double>(b[static_cast<size_t>(i)]));
    }
    return sum / static_cast<double>(endIndex - beginIndex);
}

// Integrate Hann-windowed DFT bins over disjoint windows. Goertzel keeps this
// spectral regression self-contained; Parseval supplies the total energy.
double bandEnergyRatio(const std::vector<float>& samples, int begin, int end,
                       double sampleRate, double lowHz, double highHz)
{
    constexpr int windowSize = 1024;
    double bandEnergy = 0.0;
    double totalEnergy = 0.0;
    const int firstBin = static_cast<int>(std::ceil(lowHz * windowSize / sampleRate));
    const int lastBin = std::min(windowSize / 2 - 1,
        static_cast<int>(std::floor(highHz * windowSize / sampleRate)));
    double window[windowSize];
    double frame[windowSize];
    for (int i = 0; i < windowSize; ++i)
        window[i] = 0.5 - 0.5 * std::cos(kTwoPi * i / windowSize);
    for (int start = begin; start + windowSize <= end; start += windowSize) {
        for (int i = 0; i < windowSize; ++i) {
            frame[i] = samples[static_cast<size_t>(start + i)] * window[i];
            totalEnergy += windowSize * frame[i] * frame[i];
        }
        for (int bin = firstBin; bin <= lastBin; ++bin) {
            const double coefficient = 2.0 * std::cos(kTwoPi * bin / windowSize);
            double previous = 0.0;
            double previous2 = 0.0;
            for (const double sample : frame) {
                const double next = sample + coefficient * previous - previous2;
                previous2 = previous;
                previous = next;
            }
            bandEnergy += 2.0 * (previous * previous + previous2 * previous2
                - coefficient * previous * previous2);
        }
    }
    return bandEnergy / totalEnergy;
}

double toneAmplitude(const std::vector<float>& samples, int begin, int end,
                     double sampleRate, double frequency)
{
    double real = 0.0;
    double imag = 0.0;
    double weightSum = 0.0;
    for (int i = begin; i < end; ++i) {
        const double weight = 0.5 - 0.5 * std::cos(kTwoPi * (i - begin) / (end - begin));
        const double phase = kTwoPi * frequency * i / sampleRate;
        real += weight * samples[static_cast<size_t>(i)] * std::cos(phase);
        imag += weight * samples[static_cast<size_t>(i)] * std::sin(phase);
        weightSum += weight;
    }
    return 2.0 * std::hypot(real, imag) / weightSum;
}

// Identity correction must reproduce every input sample. Feeding interpolation
// lookahead must preserve the published sample alignment.
void testIdentityRampSampleAlignment()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192; // crosses the 4096-sample ring buffer seam
    constexpr int kLook = Shifter::kLookaheadSamples;

    // Tail of the generated stream doubles as the clip lookahead samples.
    const std::vector<float> input = makeNoise(kNumSamples + kLook, 12345u);
    const float* lookahead = input.data() + kNumSamples;

    // Guard the test itself: the signal must actually change across a
    // lookahead offset, otherwise alignment below could not detect latency.
    double fiveStepVariation = 0.0;
    for (int i = kLook; i < kNumSamples; ++i) {
        fiveStepVariation += std::fabs(static_cast<double>(
            input[static_cast<size_t>(i)] - input[static_cast<size_t>(i - kLook)]));
    }
    fiveStepVariation /= static_cast<double>(kNumSamples - kLook);
    expectTrue(fiveStepVariation > 0.05,
               "identity setup: test signal must vary over the lookahead offset");

    Shifter shifter(static_cast<double>(kSampleRate));
    const float f0 = 220.0f;
    const std::vector<float> out = shifter.shiftChunk(
        input.data(), kNumSamples, &f0, &f0, 1, 100.0, 0.0, lookahead, kLook);

    expectTrue(static_cast<int>(out.size()) == kNumSamples,
               "identity: output length must equal input length");
    expectTrue(allFinite(out), "identity: output must be finite");

    double maxErr = 0.0;
    for (int i = 0; i < kNumSamples; ++i) {
        maxErr = std::max(maxErr, std::fabs(static_cast<double>(
            out[static_cast<size_t>(i)] - input[static_cast<size_t>(i)])));
    }
    // Exact passthrough lands at ~0 here (integer read addresses), while any
    // residual group delay such as the lookahead samples would produce
    // O(0.1..1) sample errors on this noise signal. Tolerance 1e-5.
    expectTrue(maxErr <= 1e-5,
               "identity: output[i] must match input[i] within 1e-5 "
               "(rejects a lookahead-delayed result)");

    // Discrimination proof: the delayed hypothesis must fail badly on this
    // signal, demonstrating the assertion above really rejects latency.
    double maxDelayedErr = 0.0;
    for (int i = kLook; i < kNumSamples; ++i) {
        maxDelayedErr = std::max(maxDelayedErr, std::fabs(static_cast<double>(
            out[static_cast<size_t>(i)] - input[static_cast<size_t>(i - kLook)])));
    }
    expectTrue(maxDelayedErr > 0.1,
               "identity: delayed hypothesis must be clearly distinguishable");
}

// Each published chunk owns an independent shifter instance. This verifies
// the per-chunk contract without introducing cross-chunk state.
void testIdentityRampAcrossIndependentChunks()
{
    constexpr int kSampleRate = 44100;
    constexpr int kTotal = 8192;
    constexpr int kChunk = 512;
    constexpr int kLook = Shifter::kLookaheadSamples;
    constexpr double kSamplesPerFrame = 441.0; // 100 fps at 44.1 kHz

    const std::vector<float> stream = makeNoise(kTotal + kLook, 777u);
    const float f0 = 220.0f;

    for (int start = 0; start < kTotal; start += kChunk) {
        const int count = std::min(kChunk, kTotal - start);
        const double firstSampleFramePhase =
            std::fmod(static_cast<double>(start), kSamplesPerFrame) / kSamplesPerFrame;

        Shifter shifter(static_cast<double>(kSampleRate));
        const std::vector<float> out = shifter.shiftChunk(
            stream.data() + start, count, &f0, &f0, 1, 100.0,
            firstSampleFramePhase, stream.data() + start + count, kLook);

        expectTrue(static_cast<int>(out.size()) == count,
                   "identity chunks: length preserved per chunk");

        double maxErr = 0.0;
        for (int i = 0; i < count; ++i) {
            maxErr = std::max(maxErr, std::fabs(static_cast<double>(
                out[static_cast<size_t>(i)]
                - stream[static_cast<size_t>(start + i)])));
        }
        expectTrue(maxErr <= 1e-5,
                   "independent chunks: each output sample remains aligned "
                   "(tolerance 1e-5)");
    }
}

// ---------------------------------------------------------------------------
// 2. Sustained non-integer-ratio shifts up and down over a long ramp: length
//    strictly preserved, all finite, no extreme outliers or adjacent spikes.
// ---------------------------------------------------------------------------
void testNonIdentityShiftKeepsLengthFiniteAndSmooth(bool shiftUp)
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 22050; // 0.5 s sustained shift
    constexpr double kInputAmp = 0.5;

    const std::vector<float> input =
        makeSine(220.0, kSampleRate, kNumSamples, kInputAmp);
    const float originalF0 = 220.0f;
    // Both corrections remain inside the production 75-cent contract.
    const float correctedF0 = shiftUp ? 226.0f : 214.0f;
    const int historySamples = 220;
    const std::vector<float> history = makeSineSegment(
        220.0, kSampleRate, -historySamples, historySamples, kInputAmp);
    const char* lengthMsg = shiftUp
        ? "shift up: length strictly preserved"
        : "shift down: length strictly preserved";
    const char* finiteMsg = shiftUp
        ? "shift up: all outputs finite"
        : "shift down: all outputs finite";
    const char* outlierMsg = shiftUp
        ? "shift up: no extreme outliers (peak stays within input envelope)"
        : "shift down: no extreme outliers (peak stays within input envelope)";
    const char* spikeMsg = shiftUp
        ? "shift up: no adjacent-sample spikes"
        : "shift down: no adjacent-sample spikes";

    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> out = shifter.shiftChunk(
        input.data(), kNumSamples, &originalF0, &correctedF0, 1, 100.0,
        0.0, nullptr, 0, history.data(), historySamples);

    expectTrue(static_cast<int>(out.size()) == kNumSamples, lengthMsg);
    expectTrue(allFinite(out), finiteMsg);

    // This band-limited sine should retain its original amplitude.
    expectTrue(maxAbs(out) <= kInputAmp + 0.05, outlierMsg);

    // Sample-to-sample slope of a 220 Hz sine at amp 0.5 is ~0.016. A large
    // isolated jump means the cycle replacement lost waveform continuity.
    expectTrue(maxAdjacentDelta(out) <= 0.5, spikeMsg);
}

// ---------------------------------------------------------------------------
// 3. Non-integer periods: fixed tone with fractional period plus a gliding
//    F0 ramp. Output must stay finite with preserved length.
// ---------------------------------------------------------------------------
void testNonIntegerPeriodInputsStayFinite()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 22050;

    // A) Period 44100 / 307.8 is ~143.24 samples (never integer), shifted by
    //    the non-integer ratio 300.0 / 307.8.
    {
        const std::vector<float> input =
            makeSine(307.8, kSampleRate, kNumSamples, 0.5);
        const float originalF0 = 307.8f;
        const float correctedF0 = 300.0f;
        const int historySamples = 160;
        const std::vector<float> history = makeSineSegment(
            307.8, kSampleRate, -historySamples, historySamples, 0.5);

        Shifter shifter(static_cast<double>(kSampleRate));
        const std::vector<float> out = shifter.shiftChunk(
            input.data(), kNumSamples, &originalF0, &correctedF0, 1, 100.0,
            0.0, nullptr, 0, history.data(), historySamples);

        expectTrue(static_cast<int>(out.size()) == kNumSamples,
                   "non-integer sine: length preserved");
        expectTrue(allFinite(out), "non-integer sine: all outputs finite");
    }

    // B) Continuous F0 glide 180 -> 320 Hz: every instantaneous period is
    //    fractional (137.8 .. 245 samples).
    {
        constexpr int kNumFrames = 50;
        std::vector<float> originalF0;
        const std::vector<float> input = makeGlide(
            180.0, 320.0, kSampleRate, kNumSamples, kNumFrames, 100.0, 0.5,
            &originalF0);
        std::vector<float> correctedF0(static_cast<size_t>(kNumFrames));
        for (int k = 0; k < kNumFrames; ++k)
            correctedF0[static_cast<size_t>(k)] =
                originalF0[static_cast<size_t>(k)] * 1.02f;
        const int historySamples = 260;
        const std::vector<float> history = makeSineSegment(
            180.0, kSampleRate, -historySamples, historySamples, 0.5);

        Shifter shifter(static_cast<double>(kSampleRate));
        const std::vector<float> out = shifter.shiftChunk(
            input.data(), kNumSamples, originalF0.data(), correctedF0.data(),
            kNumFrames, 100.0, 0.0, nullptr, 0,
            history.data(), historySamples);

        expectTrue(static_cast<int>(out.size()) == kNumSamples,
                   "non-integer F0 glide: length preserved");
        expectTrue(allFinite(out),
                   "non-integer F0 glide: all outputs finite");
    }
}

void testFloatingCyclePeriodProducesTargetRate()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 44100;
    constexpr int kBegin = 10000;
    constexpr int kEnd = 40000;
    constexpr double originalF0 = 307.8;
    constexpr double correctedF0 = 300.0;

    const std::vector<float> input = makeSine(
        originalF0, kSampleRate, kNumSamples, 0.5);
    const float original = static_cast<float>(originalF0);
    const float corrected = static_cast<float>(correctedF0);
    const int historySamples = 160;
    const std::vector<float> history = makeSineSegment(
        originalF0, kSampleRate, -historySamples, historySamples, 0.5);

    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> output = shifter.shiftChunk(
        input.data(), kNumSamples, &original, &corrected, 1, 100.0,
        0.0, nullptr, 0, history.data(), historySamples);

    const int crossings = countPositiveZeroCrossings(output, kBegin, kEnd);
    const double duration = static_cast<double>(kEnd - kBegin) / kSampleRate;
    const double expected = correctedF0 * duration;
    expectTrue(std::fabs(static_cast<double>(crossings) - expected) < expected * 0.03,
               "floating cycle period: output frequency follows corrected F0");
}

void testCycleTransitionsSuppressAddedHighFrequencies()
{
    constexpr int sampleRate = 44100;
    constexpr int count = sampleRate * 2;
    constexpr int history = 1024;
    constexpr float sourceF0 = 200.0f;
    constexpr float period = 220.5f;
    std::vector<float> stream(history + count + Shifter::kLookaheadSamples);
    for (int i = 0; i < static_cast<int>(stream.size()); ++i) {
        const double t = static_cast<double>(i - history) / sampleRate;
        double voice = 0.0;
        for (int harmonic = 1; harmonic <= 20; ++harmonic)
            voice += 0.2 / harmonic * std::sin(kTwoPi * sourceF0 * harmonic * t + 0.17 * harmonic);
        voice *= 1.0 + 0.35 * std::sin(kTwoPi * 7.0 * t);
        // Incommensurate, band-limited components model nonperiodic breath.
        // The exact fundamental period leaves adjacent cycles different.
        for (int partial = 0; partial < 17; ++partial)
            voice += 0.006 * std::sin(kTwoPi * (731.3 + 173.17 * partial) * t + 0.71 * partial * partial);
        stream[static_cast<size_t>(i)] = static_cast<float>(voice);
    }
    std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod> periods(count);
    for (int i = 0; i < count; ++i)
        periods[static_cast<size_t>(i)] = {period, true, i % 5 == 0};
    const std::vector<float> input(stream.begin() + history, stream.begin() + history + count);
    const double inputHigh = bandEnergyRatio(input, sampleRate / 2, count - 1024,
                                            sampleRate, 8000.0, 22000.0);
    expectTrue(inputHigh < 1e-10, "spectral setup: source is confined below 4 kHz");
    for (const double cents : {-50.0, 50.0}) {
        const float targetF0 = static_cast<float>(sourceF0 * std::exp2(cents / 1200.0));
        Shifter shifter(sampleRate);
        const auto output = shifter.shiftChunk(
            stream.data() + history, count, &sourceF0, &targetF0, 1, 100.0, 0.0,
            stream.data() + history + count, Shifter::kLookaheadSamples,
            stream.data(), history, periods.data(), count, true);
        expectTrue(output.size() == static_cast<size_t>(count), "spectral shift: duration preserved");
        if (output.size() != static_cast<size_t>(count))
            continue;
        const double high = bandEnergyRatio(output, sampleRate / 2, count - 1024,
                                            sampleRate, 8000.0, 22000.0);
        std::printf("Cycle transition %+.0f cents: added 8-22 kHz %.1f dB\n", cents, 10.0 * std::log10(high));
        expectTrue(high < 1e-8, "cycle transitions: added 8-22 kHz energy stays below -80 dB");
        const double fundamental = toneAmplitude(output, sampleRate / 2, count - 1024,
                                                 sampleRate, targetF0);
        expectTrue(fundamental > 0.18 && fundamental < 0.22,
                   "cycle transitions: target fundamental retains its amplitude and pitch");
    }
}

void testResamplingPreservesHighHarmonicsAndRejectsAliases()
{
    constexpr int sampleRate = 44100;
    constexpr int count = sampleRate;
    constexpr int history = 1024;
    constexpr float sourceF0 = 200.0f;
    std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod> periods(count);
    for (int i = 0; i < count; ++i)
        periods[static_cast<size_t>(i)] = {220.5f, true, i % 5 == 0};
    for (const double highFrequency : {16000.0, 21800.0}) {
        std::vector<float> stream(history + count + Shifter::kLookaheadSamples);
        for (int i = 0; i < static_cast<int>(stream.size()); ++i) {
            const double t = static_cast<double>(i - history) / sampleRate;
            stream[static_cast<size_t>(i)] = static_cast<float>(
                0.2 * std::sin(kTwoPi * sourceF0 * t)
                + 0.1 * std::sin(kTwoPi * highFrequency * t));
        }
        for (const double cents : {-74.0, 74.0}) {
            const float targetF0 = static_cast<float>(sourceF0 * std::exp2(cents / 1200.0));
            const double rate = targetF0 / static_cast<double>(sourceF0);
            Shifter shifter(sampleRate);
            const auto output = shifter.shiftChunk(
                stream.data() + history, count, &sourceF0, &targetF0, 1, 100.0, 0.0,
                stream.data() + history + count, Shifter::kLookaheadSamples,
                stream.data(), history, periods.data(), count, true);
            expectTrue(output.size() == static_cast<size_t>(count), "band-limited shift: duration preserved");
            if (output.size() != static_cast<size_t>(count))
                continue;
            if (highFrequency == 16000.0) {
                const double amplitude = toneAmplitude(output, 8192, count - 1024,
                                                       sampleRate, highFrequency * rate);
                expectTrue(amplitude > 0.094 && amplitude < 0.106,
                           "band-limited shift: 16 kHz harmonic gain stays within 0.55 dB");
            } else if (cents > 0.0) {
                const double alias = toneAmplitude(output, 8192, count - 1024,
                                                   sampleRate, sampleRate - highFrequency * rate);
                std::printf("Upshift 21.8 kHz alias attenuation: %.1f dB\n", 20.0 * std::log10(alias / 0.1));
                expectTrue(alias < 0.0032, "band-limited shift: above-Nyquist alias suppressed by 30 dB");
            }
        }
    }
}

void testCycleTransitionRateReversals()
{
    constexpr int sampleRate = 44100;
    constexpr int count = 12000;
    constexpr int history = 2048;
    constexpr int samplesPerFrame = 40;
    constexpr int frameCount = count / samplesPerFrame;
    for (const float period : {16.0f, 880.0f}) {
        const double sourceFrequency = sampleRate / static_cast<double>(period);
        const auto stream = makeSineSegment(sourceFrequency, sampleRate, -history,
            history + count + Shifter::kLookaheadSamples, 0.3);
        std::vector<float> original(frameCount, static_cast<float>(sourceFrequency));
        std::vector<float> target(frameCount);
        for (int frame = 0; frame < frameCount; ++frame) {
            const double cents = frame % 4 == 0 ? 74.0 : -74.0;
            target[static_cast<size_t>(frame)] = static_cast<float>(sourceFrequency * std::exp2(cents / 1200.0));
        }
        std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod> periods(count);
        for (int i = 0; i < count; ++i)
            periods[static_cast<size_t>(i)] = {period, true, i % 5 == 0};
        Shifter shifter(sampleRate);
        const auto output = shifter.shiftChunk(
            stream.data() + history, count, original.data(), target.data(), frameCount,
            static_cast<double>(sampleRate) / samplesPerFrame, 0.0,
            stream.data() + history + count, Shifter::kLookaheadSamples,
            stream.data(), history, periods.data(), count, true);
        expectTrue(output.size() == static_cast<size_t>(count), "rate reversal: duration preserved");
        if (output.size() != static_cast<size_t>(count))
            continue;
        double position = -1.0;
        double maxError = 0.0;
        for (int i = 0; i < count; ++i) {
            position += target[static_cast<size_t>(i / samplesPerFrame)] * static_cast<double>(period) / sampleRate;
            const double expected = 0.3 * std::sin(kTwoPi * position / period);
            maxError = std::max(maxError, std::fabs(output[static_cast<size_t>(i)] - expected));
        }
        expectTrue(maxError < 0.001,
                   "rate reversal: overlapping cycle requests preserve phase and amplitude");
    }
}

void testNonIdentityDoesNotMoveChunkStart()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 2048;
    constexpr int kHistorySamples = 220;

    std::vector<float> input(static_cast<size_t>(kNumSamples), 0.0f);
    input[0] = 1.0f;
    const std::vector<float> history(static_cast<size_t>(kHistorySamples), 0.0f);
    const float originalF0 = 220.0f;
    const float correctedF0 = 226.0f;

    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> output = shifter.shiftChunk(
        input.data(), kNumSamples, &originalF0, &correctedF0, 1, 100.0,
        0.0, nullptr, 0, history.data(), kHistorySamples);

    int firstNonZero = -1;
    for (int i = 0; i < kNumSamples; ++i) {
        if (std::fabs(output[static_cast<size_t>(i)]) > 0.25f) {
            firstNonZero = i;
            break;
        }
    }
    expectTrue(firstNonZero == 0,
               "non-identity: chunk onset remains at output sample zero");
}

// ---------------------------------------------------------------------------
// 4. F0 frame phase: a chunk starting mid-frame must consume the correctedF0
//    frames that actually overlap it, not reuse frame 0 throughout. Verified
//    via signal deviation against input (robust; no exact-pitch measurement).
// ---------------------------------------------------------------------------
void testF0FramePhaseUsesLaterFrames()
{
    constexpr int kSampleRate = 44100;
    constexpr int kFrames = 3;
    constexpr double kFrameRate = 100.0;
    constexpr int kSamplesPerFrame =
        static_cast<int>(kSampleRate / kFrameRate); // 441
    constexpr int kNumSamples = kSamplesPerFrame * kFrames; // 1323
    constexpr double kFirstPhase = 0.5;                     // starts mid-frame

    const std::vector<float> input =
        makeSine(220.0, kSampleRate, kNumSamples, 0.5);
    const std::vector<float> originalF0 = {220.0f, 220.0f, 220.0f};
    const int historySamples = 220;
    const std::vector<float> history = makeSineSegment(
        220.0, kSampleRate, -historySamples, historySamples, 0.5);

    // Control run: identical guidance in every frame -> pure passthrough.
    Shifter control(static_cast<double>(kSampleRate));
    const std::vector<float> controlOut = control.shiftChunk(
        input.data(), kNumSamples, originalF0.data(), originalF0.data(),
        kFrames, kFrameRate, kFirstPhase, nullptr, 0,
        history.data(), historySamples);

    // Candidate run: frame 0 untouched, later frames carry distinct targets.
    const std::vector<float> correctedF0 = {220.0f, 226.0f, 214.0f};
    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> out = shifter.shiftChunk(
        input.data(), kNumSamples, originalF0.data(), correctedF0.data(),
        kFrames, kFrameRate, kFirstPhase, nullptr, 0,
        history.data(), historySamples);

    expectTrue(static_cast<int>(controlOut.size()) == kNumSamples,
               "frame phase control: length preserved");
    expectTrue(static_cast<int>(out.size()) == kNumSamples,
               "frame phase candidate: length preserved");
    expectTrue(allFinite(controlOut), "frame phase control: all outputs finite");
    expectTrue(allFinite(out), "frame phase candidate: all outputs finite");

    const int windowBegin = kSamplesPerFrame * 2; // final-frame region
    const int windowEnd = kNumSamples;
    const double refEnergy = meanAbsRange(input, windowBegin, windowEnd);
    expectTrue(refEnergy > 0.1, "frame phase: reference window carries signal");

    const double controlDev = refEnergy > 0.0
        ? meanAbsDiffRange(controlOut, input, windowBegin, windowEnd) / refEnergy
        : 1.0;
    const double shiftedDev = refEnergy > 0.0
        ? meanAbsDiffRange(out, input, windowBegin, windowEnd) / refEnergy
        : 1.0;

    // Metric validity: the control must be near-perfect passthrough.
    expectTrue(controlDev < 0.01,
               "frame phase: identity control matches input in late window");
    // If frames 1..2 were ignored and frame 0 reused, the late window would
    // look like the control (~0). Real per-frame retuning decorrelates the
    // waveform from the input within a few periods, far above this threshold.
    expectTrue(shiftedDev > 0.10,
               "frame phase: later-frame corrections applied (not stuck on "
               "the first frame)");
}

// ---------------------------------------------------------------------------
// 5. Clip-final chunks with missing tail lookahead: output length preserved
//    and fully finite.
// ---------------------------------------------------------------------------
void testTailChunksWithShortLookahead()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 1000;

    const std::vector<float> input =
        makeSine(220.0, kSampleRate, kNumSamples, 0.5);
    const float originalF0 = 220.0f;
    const float correctedF0 = 226.0f;
    const int historySamples = 220;
    const std::vector<float> history = makeSineSegment(
        220.0, kSampleRate, -historySamples, historySamples, 0.5);

    {
        Shifter shifter(static_cast<double>(kSampleRate));
        const std::vector<float> out = shifter.shiftChunk(
            input.data(), kNumSamples, &originalF0, &correctedF0, 1, 100.0,
            0.0, nullptr, 0, history.data(), historySamples);
        expectTrue(static_cast<int>(out.size()) == kNumSamples,
                   "tail (0 lookahead): length preserved");
        expectTrue(allFinite(out),
                   "tail (0 lookahead): all outputs finite");
    }

    {
        const float tail[2] = {0.25f, -0.25f};
        Shifter shifter(static_cast<double>(kSampleRate));
        const std::vector<float> out = shifter.shiftChunk(
            input.data(), kNumSamples, &originalF0, &correctedF0, 1, 100.0,
            0.0, tail, 2, history.data(), historySamples);
        expectTrue(static_cast<int>(out.size()) == kNumSamples,
                   "tail (2 lookahead): length preserved");
        expectTrue(allFinite(out),
                   "tail (2 lookahead): all outputs finite");
    }
}

// ---------------------------------------------------------------------------
// 6. Detector shadow source: a fixed sine must lock every frame onto the true
//    period, and white noise must report valid == false everywhere.
// ---------------------------------------------------------------------------
void testDetectorFixedSineValidPeriod()
{
    constexpr int kSampleRate = 44100;
    constexpr double kFreq = 220.0; // period ~200.45 samples (fractional)
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 2048;

    // Phase-continuous stream: prefix then input, as in the render path.
    const std::vector<float> stream = makeSineSegment(
        kFreq, kSampleRate, -kPrefix, kPrefix + kNumSamples, 0.5);

    const auto frames = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        static_cast<double>(kSampleRate));

    expectTrue(static_cast<int>(frames.size()) == kNumSamples,
               "detector sine: one result per input sample");

    const double expectedPeriod = static_cast<double>(kSampleRate) / kFreq;
    int validCount = 0;
    double maxPeriodErr = 0.0;
    for (const auto& f : frames) {
        if (!f.valid)
            continue;
        ++validCount;
        maxPeriodErr = std::max(maxPeriodErr,
            std::fabs(static_cast<double>(f.periodSamples) - expectedPeriod));
    }
    expectTrue(validCount == kNumSamples,
               "detector sine: every frame locks onto the tone");
    expectTrue(maxPeriodErr <= 2.0,
               "detector sine: refined period within 2 samples of SR/freq");
}

void testDetectorRejectsNoise()
{
    constexpr int kSampleRate = 44100;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 2048;

    const std::vector<float> stream =
        makeNoise(kPrefix + kNumSamples, 20240726u);

    const auto frames = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        static_cast<double>(kSampleRate));

    expectTrue(static_cast<int>(frames.size()) == kNumSamples,
               "detector noise: one result per input sample");
    bool anyValid = false;
    for (int t = 0; t < kNumSamples && !anyValid; ++t)
        anyValid = frames[static_cast<size_t>(t)].valid;
    expectTrue(!anyValid,
               "detector noise: every frame must report valid == false");
}

void testDetectorTracksLowAndHighTones()
{
    constexpr int kSampleRate = 44100;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 4096;

    // 882 Hz has a 50-sample period on the fixed 44.1 kHz grid. The
    // 8-sample coarse candidate quantization otherwise makes 1000 Hz choose
    // its 88-sample second minimum during the strict full-rate confirmation.
    for (const double frequency : {110.0, 882.0})
    {
        const std::vector<float> stream = makeSineSegment(
            frequency, kSampleRate, -kPrefix, kPrefix + kNumSamples, 0.5);
        const auto frames = OpenTune::AutoTunePeriodDetector::analyze(
            stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
            static_cast<double>(kSampleRate));

        const double expectedPeriod = static_cast<double>(kSampleRate) / frequency;
        int validCount = 0;
        double maxPeriodError = 0.0;
        for (const auto& frame : frames)
        {
            if (!frame.valid)
                continue;
            ++validCount;
            maxPeriodError = std::max(maxPeriodError,
                std::fabs(static_cast<double>(frame.periodSamples) - expectedPeriod));
        }

        expectTrue(validCount > kNumSamples * 3 / 4,
                   "detector range: tone remains valid after initialization");
        expectTrue(maxPeriodError < expectedPeriod * 0.12,
                   "detector range: period stays within 12 percent");
    }
}

void testDetectorDropsOutAcrossNoise()
{
    constexpr int kSampleRate = 44100;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kToneSamples = 2304;
    constexpr int kNoiseSamples = 2048;
    constexpr int kNumSamples = kToneSamples + kNoiseSamples + kToneSamples;

    const std::vector<float> prefix = makeSineSegment(
        220.0, kSampleRate, -kPrefix, kPrefix, 0.5);
    const std::vector<float> firstTone = makeSineSegment(
        220.0, kSampleRate, 0, kToneSamples, 0.5);
    const std::vector<float> noise = makeNoise(kNoiseSamples, 20260826u);
    const std::vector<float> lastTone = makeSineSegment(
        220.0, kSampleRate, kToneSamples + kNoiseSamples, kToneSamples, 0.5);

    std::vector<float> input(static_cast<size_t>(kNumSamples));
    std::copy(firstTone.begin(), firstTone.end(), input.begin());
    std::copy(noise.begin(), noise.end(), input.begin() + kToneSamples);
    std::copy(lastTone.begin(), lastTone.end(),
              input.begin() + kToneSamples + kNoiseSamples);

    const auto frames = OpenTune::AutoTunePeriodDetector::analyze(
        prefix.data(), kPrefix, input.data(), kNumSamples,
        static_cast<double>(kSampleRate));

    bool invalidInNoise = false;
    for (int i = kToneSamples + kNoiseSamples / 4;
         i < kToneSamples + kNoiseSamples * 3 / 4;
         ++i)
    {
        if (!frames[static_cast<size_t>(i)].valid)
        {
            invalidInNoise = true;
            break;
        }
    }

    bool validAfterNoise = false;
    int firstValidAfterNoise = -1;
    int validAfterNoiseCount = 0;
    for (int i = kToneSamples + kNoiseSamples;
         i < kNumSamples;
         ++i)
    {
        if (frames[static_cast<size_t>(i)].valid)
        {
            validAfterNoise = true;
            if (firstValidAfterNoise < 0)
                firstValidAfterNoise = i;
            ++validAfterNoiseCount;
        }
    }

    std::printf("NOISE_TRANSITION firstValidAfterNoise=%d validCount=%d\n",
                firstValidAfterNoise, validAfterNoiseCount);


    expectTrue(invalidInNoise,
               "detector transition: noise region becomes invalid");
    expectTrue(validAfterNoise,
               "detector transition: voiced period reacquires after noise");
}

// A short V -> UV -> V gap retains positive source and target F0 values. A
// detector failure inside that voiced correction makes octave repair fail so
// the runtime can route the chunk through the vocoder.
void testShortUvGapWithPositiveCorrectedF0()
{
    constexpr int kSampleRate = 44100;
    constexpr double kVoicedFrequency = 220.0;
    constexpr double kSourceF0 = 220.0;
    constexpr double kCorrectedF0 = 225.0;
    constexpr double kF0FrameRate = 100.0;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kLeadSamples = 2200;
    constexpr int kGapSamples = 1323;
    constexpr int kTailSamples = 2200;
    constexpr int kLookahead = Shifter::kLookaheadSamples;
    constexpr int kNumSamples = kLeadSamples + kGapSamples + kTailSamples;
    const std::vector<float> prefix = makeSineSegment(
        kVoicedFrequency, kSampleRate, -kPrefix, kPrefix, 0.5);
    const std::vector<float> lead = makeSineSegment(
        kVoicedFrequency, kSampleRate, 0, kLeadSamples, 0.5);
    std::vector<float> gap = makeNoise(kGapSamples, 20260828u);
    for (float& sample : gap)
        sample *= 0.25f;
    const std::vector<float> tail = makeSineSegment(
        kVoicedFrequency, kSampleRate, kLeadSamples + kGapSamples,
        kTailSamples + kLookahead, 0.5);

    std::vector<float> input;
    input.reserve(static_cast<size_t>(kNumSamples));
    input.insert(input.end(), lead.begin(), lead.end());
    input.insert(input.end(), gap.begin(), gap.end());
    input.insert(input.end(), tail.begin(), tail.begin() + kTailSamples);

    std::vector<float> fullStream;
    fullStream.reserve(static_cast<size_t>(kPrefix + kNumSamples + kLookahead));
    fullStream.insert(fullStream.end(), prefix.begin(), prefix.end());
    fullStream.insert(fullStream.end(), input.begin(), input.end());
    fullStream.insert(fullStream.end(), tail.begin() + kTailSamples, tail.end());

    const auto detected = OpenTune::AutoTunePeriodDetector::analyze(
        fullStream.data(), kPrefix, input.data(), kNumSamples,
        static_cast<double>(kSampleRate));
    bool hasInvalidGapSample = false;
    for (int i = kLeadSamples; i < kLeadSamples + kGapSamples; ++i)
        hasInvalidGapSample = hasInvalidGapSample
            || !detected[static_cast<size_t>(i)].valid;

    const int numF0Frames = static_cast<int>(std::ceil(
        static_cast<double>(kNumSamples) / kSampleRate * kF0FrameRate)) + 1;
    std::vector<float> originalF0(static_cast<size_t>(numF0Frames),
                                  static_cast<float>(kSourceF0));
    std::vector<float> correctedF0(static_cast<size_t>(numF0Frames),
                                   static_cast<float>(kCorrectedF0));

    Shifter shifter(static_cast<double>(kSampleRate));
    const auto result = shifter.shiftChunkWithOctaveRepair(
        input.data(), kNumSamples, originalF0.data(), correctedF0.data(),
        numF0Frames, kF0FrameRate, 0.0,
        fullStream.data() + kPrefix + kNumSamples, kLookahead,
        prefix.data(), kPrefix,
        detected.data(), static_cast<int>(detected.size()), false);
    expectTrue(hasInvalidGapSample,
               "short UV: detector reports an unconfirmed period inside the gap");
    expectTrue(originalF0.front() > 0.0f && correctedF0.front() > 0.0f,
               "short UV: positive source and target F0 span the gap");
    expectTrue(!result.success && result.audio.empty(),
               "short UV: voiced correction with an unconfirmed period fails repair");
}

void testDetectorRequiresCompleteHistory()
{
    constexpr int kSampleRate = 44100;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples - 2;
    constexpr int kNumSamples = 1;

    const std::vector<float> prefix = makeSineSegment(
        220.0, kSampleRate, -kPrefix, kPrefix, 0.5);
    const std::vector<float> input = makeSineSegment(
        220.0, kSampleRate, 0, kNumSamples, 0.5);
    const auto frames = OpenTune::AutoTunePeriodDetector::analyze(
        prefix.data(), kPrefix, input.data(), kNumSamples,
        static_cast<double>(kSampleRate));

    bool anyValid = false;
    for (const auto& frame : frames)
        anyValid = anyValid || frame.valid;
    expectTrue(!anyValid,
               "detector history: incomplete current context stays invalid");
}

// The shifter must consume the detector shadow source: the measured cycle
// period comes from the detector, and the output still follows correctedF0.
void testShifterUsesDetectorShadowSource()
{
    constexpr int kSampleRate = 44100;
    constexpr double kFreq = 307.8;
    constexpr double kCorrected = 300.0;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 22050;
    constexpr int kBegin = 5000;
    constexpr int kEnd = 20000;

    const std::vector<float> stream = makeSineSegment(
        kFreq, kSampleRate, -kPrefix, kPrefix + kNumSamples, 0.5);
    const std::vector<float> tail = makeSineSegment(
        kFreq, kSampleRate, kPrefix + kNumSamples,
        Shifter::kLookaheadSamples, 0.5);

    const auto frames = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        static_cast<double>(kSampleRate));

    // RMVPE originalF0 deliberately matches the tone here; the measured
    // period must come from the detector shadow either way.
    const float rmvpeF0 = 220.0f;
    const float correctedF0 = static_cast<float>(kCorrected);

    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> out = shifter.shiftChunk(
        stream.data() + kPrefix, kNumSamples, &rmvpeF0, &correctedF0,
        1, 100.0, 0.0, tail.data(), Shifter::kLookaheadSamples,
        stream.data(), kPrefix,
        frames.data(), static_cast<int>(frames.size()));

    const int crossings = countPositiveZeroCrossings(out, kBegin, kEnd);
    const double duration = static_cast<double>(kEnd - kBegin) / kSampleRate;
    const double expected = kCorrected * duration;
    expectTrue(std::fabs(static_cast<double>(crossings) - expected)
                   < expected * 0.03,
               "detector shadow: output frequency follows correctedF0");
}

// All-invalid detector frames must force exact neutral passthrough even when
// originalF0/correctedF0 demand a shift (RMVPE originalF0 is ignored).
void testShifterStaysNeutralWhenDetectorInvalid()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 4096;
    constexpr int kLook = Shifter::kLookaheadSamples;

    const std::vector<float> stream = makeNoise(kNumSamples + kLook, 99u);
    const float* lookahead = stream.data() + kNumSamples;

    std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod> invalid(
        static_cast<size_t>(kNumSamples)); // default: valid == false
    const float originalF0 = 220.0f;
    const float correctedF0 = 240.0f;

    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> out = shifter.shiftChunk(
        stream.data(), kNumSamples, &originalF0, &correctedF0, 1, 100.0,
        0.0, lookahead, kLook, nullptr, 0,
        invalid.data(), kNumSamples);

    expectTrue(static_cast<int>(out.size()) == kNumSamples,
               "neutral: length preserved");
    double maxErr = 0.0;
    for (int i = 0; i < kNumSamples; ++i) {
        maxErr = std::max(maxErr, std::fabs(static_cast<double>(
            out[static_cast<size_t>(i)] - stream[static_cast<size_t>(i)])));
    }
    expectTrue(maxErr <= 1e-5,
               "neutral: invalid detector forces sample-exact passthrough");
}

void testShifterUsesDetectorValidityForUv()
{
    constexpr int kSampleRate = 44100;
    constexpr double kSourceFrequency = 220.0;
    constexpr double kTargetFrequency = 225.0;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 22050;
    constexpr int kLook = Shifter::kLookaheadSamples;

    const std::vector<float> stream = makeSineSegment(
        kSourceFrequency, kSampleRate, -kPrefix, kPrefix + kNumSamples + kLook, 0.5);
    const auto detected = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        static_cast<double>(kSampleRate));

    // RMVPE's finalF0 is zero here, but the waveform is periodic and the
    // detector is valid. AutoTune must follow detector validity, not RMVPE UV.
    const float rmvpeF0 = 0.0f;
    const float correctedF0 = static_cast<float>(kTargetFrequency);

    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> out = shifter.shiftChunk(
        stream.data() + kPrefix, kNumSamples, &rmvpeF0, &correctedF0, 1, 100.0,
        0.0, stream.data() + kPrefix + kNumSamples, kLook,
        stream.data(), kPrefix, detected.data(), static_cast<int>(detected.size()));

    constexpr int kBegin = 5000;
    constexpr int kEnd = 20000;
    const int crossings = countPositiveZeroCrossings(out, kBegin, kEnd);
    const double duration = static_cast<double>(kEnd - kBegin) / kSampleRate;
    const double measuredFrequency = static_cast<double>(crossings) / duration;
    expectTrue(measuredFrequency > kTargetFrequency * 0.9
                   && measuredFrequency < kTargetFrequency * 1.1,
               "UV source: detector-valid periodic input follows correctedF0 even when RMVPE F0 is zero");
}

// Detector tracking/acquisition update events: trackingUpdated marks exactly
// the refreshed measurements; hop-held repeats must bit-exactly reproduce the
// last updated period so consumers can gate smoothed-rate refreshes on it.
void testDetectorMarksTrackingUpdateEvents()
{
    constexpr int kSampleRate = 44100;
    constexpr double kFreq = 220.0;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 2048;

    const std::vector<float> stream = makeSineSegment(
        kFreq, kSampleRate, -kPrefix, kPrefix + kNumSamples, 0.5);
    const auto frames = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        static_cast<double>(kSampleRate));

    expectTrue(static_cast<int>(frames.size()) == kNumSamples,
               "events: one result per input sample");

    int updates = 0;
    bool sawHeldRepeat = false;
    bool haveEvent = false;
    float lastEventPeriod = 0.0f;
    for (int i = 0; i < kNumSamples; ++i) {
        const auto& frame = frames[static_cast<size_t>(i)];
        if (frame.trackingUpdated) {
            expectTrue(frame.valid,
                       "events: a tracking update must carry a valid period");
            ++updates;
            lastEventPeriod = frame.periodSamples;
            haveEvent = true;
        }
        else if (frame.valid) {
            expectTrue(haveEvent,
                       "events: held results only follow an update event");
            expectTrue(frame.periodSamples == lastEventPeriod,
                       "events: hop-held frames repeat the last updated "
                       "period exactly");
            sawHeldRepeat = true;
        }
    }

    expectTrue(updates >= kNumSamples / 5 - 8,
               "events: a locked tone refreshes on nearly every 5-sample interval");
    expectTrue(sawHeldRepeat,
               "events: hop-held valid frames exist between updates");
}

// Resample-rate smoothing must be driven by detector update events only.
// With events present the output converges onto the correction target; with
// the same valid periods but no events the held rate stays 1 and the chunk is
// an exact passthrough.
void testShifterUpdatesRateOnlyOnDetectorEvents()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192;
    constexpr int kLook = Shifter::kLookaheadSamples;
    // Up-shifts remove whole cycles by moving the output pointer backward one
    // period; like the render path, supply preceding history so early cycle
    // jumps stay inside the readable ring.
    constexpr int kHistorySamples = 256;

    const std::vector<float> stream = makeNoise(kNumSamples + kLook, 4242u);
    const float* lookahead = stream.data() + kNumSamples;
    const std::vector<float> history =
        makeNoise(kHistorySamples, 777u);

    std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod> frames(
        static_cast<size_t>(kNumSamples));
    for (int i = 0; i < kNumSamples; ++i) {
        frames[static_cast<size_t>(i)].periodSamples = 200.0f;
        frames[static_cast<size_t>(i)].valid = true;
    }

    const float originalF0 = 220.0f;  // voiced gate open throughout
    const float correctedF0 = 225.0f; // demands an upward retune inside ±75c

    // A) Updates every 5 samples: the smoothed rate must leave 1.0 and chase
    //    the corrected target, clearly departing from passthrough.
    for (int i = 0; i < kNumSamples; ++i)
        frames[static_cast<size_t>(i)].trackingUpdated = (i % 5 == 0);

    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> out = shifter.shiftChunk(
        stream.data(), kNumSamples, &originalF0, &correctedF0, 1, 100.0,
        0.0, lookahead, kLook, history.data(), kHistorySamples,
        frames.data(), kNumSamples);

    expectTrue(allFinite(out), "rate events: all outputs finite");
    double maxDiff = 0.0;
    for (int i = 1024; i < kNumSamples; ++i) {
        maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(
            out[static_cast<size_t>(i)] - stream[static_cast<size_t>(i)])));
    }
    expectTrue(maxDiff > 0.05,
               "rate events: periodic tracking updates drive the retune");

    // B) Same valid periods but zero update events: the rate stays held at
    //    1.0 and the output remains a sample-exact passthrough.
    for (auto& frame : frames)
        frame.trackingUpdated = false;

    Shifter held(static_cast<double>(kSampleRate));
    const std::vector<float> outHeld = held.shiftChunk(
        stream.data(), kNumSamples, &originalF0, &correctedF0, 1, 100.0,
        0.0, lookahead, kLook, history.data(), kHistorySamples,
        frames.data(), kNumSamples);

    double maxErr = 0.0;
    for (int i = 0; i < kNumSamples; ++i) {
        maxErr = std::max(maxErr, std::fabs(static_cast<double>(
            outHeld[static_cast<size_t>(i)] - stream[static_cast<size_t>(i)])));
    }
    expectTrue(maxErr <= 1e-5,
               "rate events: without updates the held rate keeps exact neutral");
}

// Voiced detector failure must keep walking the continuous resampled path at
// rate 1 (detector failure semantics) instead of resetting the chunk-local
// output pointer. After a retuned section, the accumulated resampler offset
// survives into the failure stretch, so the output is NOT snapped back to the
// raw input.
void testShifterKeepsAddressContinuityThroughVoicedFailure()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 6000;
    constexpr int kValidLead = 3000;
    constexpr int kFailureSamples = 1000;
    constexpr int kLook = Shifter::kLookaheadSamples;
    // The retuned lead-in removes whole cycles (rate > 1); supply preceding
    // history as the render path does so cycle jumps remain readable.
    constexpr int kHistorySamples = 256;

    const std::vector<float> stream = makeNoise(kNumSamples + kLook, 20260826u);
    const float* lookahead = stream.data() + kNumSamples;
    const std::vector<float> history = makeNoise(kHistorySamples, 424242u);

    std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod> frames(
        static_cast<size_t>(kNumSamples));
    for (int i = 0; i < kNumSamples; ++i) {
        auto& frame = frames[static_cast<size_t>(i)];
        frame.periodSamples = 200.0f;
        frame.valid = (i < kValidLead || i >= kValidLead + kFailureSamples);
        frame.trackingUpdated = frame.valid && (i % 5 == 0);
    }

    const float originalF0 = 220.0f;  // voiced gate open: failures here are
    const float correctedF0 = 225.0f; // detector failures, not UV neutrals

    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> out = shifter.shiftChunk(
        stream.data(), kNumSamples, &originalF0, &correctedF0, 1, 100.0,
        0.0, lookahead, kLook, history.data(), kHistorySamples,
        frames.data(), kNumSamples);

    expectTrue(static_cast<int>(out.size()) == kNumSamples,
               "continuity: length preserved");
    expectTrue(allFinite(out), "continuity: all outputs finite");

    const int begin = kValidLead + 200;
    const int end = kValidLead + kFailureSamples - 200;
    const double diff = meanAbsDiffRange(out, stream, begin, end);
    expectTrue(diff > 0.1,
               "continuity: voiced failure keeps the resampler offset instead "
               "of resetting to raw input");
}

// Full-rate lag limits (16 .. 880): boundary tones must lock inside the
// limits, and out-of-range periods must fail cleanly without fabricating a
// clamped or relocated "valid" result.
void testDetectorFullRateLagBounds()
{
    constexpr int kSampleRate = 44100;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 4096;
    using Detector = OpenTune::AutoTunePeriodDetector;

    auto runTone = [&](double frequency) {
        const std::vector<float> stream = makeSineSegment(
            frequency, kSampleRate, -kPrefix, kPrefix + kNumSamples, 0.5);
        return Detector::analyze(
            stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
            static_cast<double>(kSampleRate));
    };

    // Periods hugging both limits from the inside: ~24.5 samples (1800 Hz,
    // just above kMinFullLag = 16) and ~864.7 samples (51 Hz, just below
    // kMaxFullLag = 880).
    for (const double frequency : {1800.0, 51.0})
    {
        const auto frames = runTone(frequency);
        const double expected = static_cast<double>(kSampleRate) / frequency;
        int validCount = 0;
        double maxErr = 0.0;
        for (const auto& frame : frames)
        {
            if (!frame.valid)
                continue;
            ++validCount;
            expectTrue(
                frame.periodSamples
                    >= static_cast<float>(Detector::kMinFullLag)
                && frame.periodSamples
                    <= static_cast<float>(Detector::kMaxFullLag),
                "bounds: in-range boundary lock stays within [16, 880]");
            maxErr = std::max(maxErr,
                std::fabs(static_cast<double>(frame.periodSamples) - expected));
        }
        expectTrue(validCount > 0,
                   "bounds: boundary-inside tone achieves a lock");
        expectTrue(maxErr <= expected * 0.12,
                   "bounds: boundary lock stays within 12 percent of truth");
    }

    // A period above the upper limit (~900 samples at 49 Hz) has no
    // representable coarse candidate: detection must fail cleanly on every
    // sample instead of fabricating or clamping a period.
    {
        const auto frames = runTone(49.0);
        int validCount = 0;
        for (const auto& frame : frames)
        {
            if (!frame.valid)
                continue;
            ++validCount;
            expectTrue(
                frame.periodSamples
                    >= static_cast<float>(Detector::kMinFullLag)
                && frame.periodSamples
                    <= static_cast<float>(Detector::kMaxFullLag),
                "bounds: no valid result may fall outside [16, 880]");
        }
        expectTrue(validCount == 0,
                   "bounds: over-limit period produces no fabricated result");
    }

    // A period below the lower limit (~15.2 samples at 2900 Hz) cannot be
    // represented on the full-rate lag grid either. The coarse stage may only
    // lock onto a far harmonic multiple, so every reported frame must still
    // respect the [16, 880] invariant - never an out-of-range forgery.
    {
        const auto frames = runTone(2900.0);
        for (const auto& frame : frames)
        {
            if (!frame.valid)
                continue;
            expectTrue(
                frame.periodSamples
                    >= static_cast<float>(Detector::kMinFullLag)
                && frame.periodSamples
                    <= static_cast<float>(Detector::kMaxFullLag),
                "bounds: sub-limit harmonic lock stays within [16, 880]");
        }
    }
}

// Tracking-window boundary semantics (EH_OFFSET): the eight-lag
// neighborhood may overhang [16, 880]; only the nominal center (N/2,
// zero-based index 3) must stay inside. A slow period glide must therefore
// stay locked all the way to periods of exactly 16 and exactly 880 - steady
// regions the previous whole-array bounds pre-rejected - and every valid
// result must remain inside [16, 880] with no fabricated or clamped values.
void testDetectorTracksBoundaryCenterPeriods()
{
    constexpr int kSampleRate = 44100;
    constexpr int kPrefix =
        OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    using Detector = OpenTune::AutoTunePeriodDetector;

    // Lower boundary: seed at 23.0 samples (an off-grid start whose coarse
    // confirmation prefers the fundamental over the 48-sample octave by a
    // wide margin), glide to 17.0, hold. A steady lock at 17 requires window
    // bases 14/15 - neighborhoods with centers inside [16, 880] but whose
    // low lags overhang the bound; the previous whole-array rule (base >= 16)
    // rejected them outright and died once the period fell below ~19.
    {
        constexpr int kLeadIn = 192;
        constexpr int kGlide = 600;
        constexpr int kHold = 1600;
        constexpr int kNumInput = kLeadIn + kGlide + kHold;

        const std::vector<float> stream = makeHoldGlideHoldSine(
            23.0, 17.0, kPrefix + kLeadIn, kGlide, kHold, 0.5);
        const auto frames = Detector::analyze(
            stream.data(), kPrefix, stream.data() + kPrefix, kNumInput,
            static_cast<double>(kSampleRate));

        expectTrue(static_cast<int>(frames.size()) == kNumInput,
                   "boundary glide low: one result per input sample");
        float minValidPeriod = 1e9f;
        int tailLocks = 0;
        for (int i = 0; i < kNumInput; ++i)
        {
            const auto& frame = frames[static_cast<size_t>(i)];
            if (!frame.valid)
                continue;
            expectTrue(
                frame.periodSamples
                    >= static_cast<float>(Detector::kMinFullLag)
                && frame.periodSamples
                    <= static_cast<float>(Detector::kMaxFullLag),
                "boundary glide low: every valid period stays in [16, 880]");
            minValidPeriod = std::min(minValidPeriod, frame.periodSamples);
            if (i >= kNumInput - kHold && frame.periodSamples <= 18.0f)
                ++tailLocks;
        }
        expectTrue(minValidPeriod <= 17.5f,
                   "boundary glide low: tracking descends past the previous "
                   "whole-array floor (~19) toward the 16 limit");
        expectTrue(tailLocks >= 100,
                   "boundary glide low: near-16 period stays locked through "
                   "the held tail");
    }

    // Upper boundary: seed at 866.0 samples (inside the initial window, so
    // the first evaluation slides right instead of failing on an edge index),
    // glide to 878.5, hold. A steady lock there needs bases 874/875 - centers
    // inside [16, 880] whose high lags overhang toward 880; the previous
    // whole-array cap (base + 7 <= 880, so base <= 873) blocked that slide
    // and died once the period passed ~877.
    {
        constexpr int kLeadIn = 192;
        constexpr int kGlide = 1250;
        constexpr int kHold = 1800;
        constexpr int kNumInput = kLeadIn + kGlide + kHold;

        const std::vector<float> stream = makeHoldGlideHoldSine(
            866.0, 878.5, kPrefix + kLeadIn, kGlide, kHold, 0.5);
        const auto frames = Detector::analyze(
            stream.data(), kPrefix, stream.data() + kPrefix, kNumInput,
            static_cast<double>(kSampleRate));

        expectTrue(static_cast<int>(frames.size()) == kNumInput,
                   "boundary glide high: one result per input sample");
        float maxValidPeriod = 0.0f;
        int tailLocks = 0;
        for (int i = 0; i < kNumInput; ++i)
        {
            const auto& frame = frames[static_cast<size_t>(i)];
            if (!frame.valid)
                continue;
            expectTrue(
                frame.periodSamples
                    >= static_cast<float>(Detector::kMinFullLag)
                && frame.periodSamples
                    <= static_cast<float>(Detector::kMaxFullLag),
                "boundary glide high: every valid period stays in [16, 880]");
            maxValidPeriod = std::max(maxValidPeriod, frame.periodSamples);
            if (i >= kNumInput - kHold && frame.periodSamples >= 877.5f)
                ++tailLocks;
        }
        expectTrue(maxValidPeriod >= 877.2f,
                   "boundary glide high: tracking ascends past the previous "
                   "whole-array cap (~877) toward the 880 limit");
        expectTrue(tailLocks >= 100,
                   "boundary glide high: near-880 period stays locked through "
                   "the held tail");
    }
}

// 1000 Hz sine on 44.1 kHz has a 44.1-sample period that sits between the
// 8-sample coarse candidate quantisation grid points. A detector bug can lock
// onto the 88.2-sample second autocorrelation minimum (2× true period) and
// propagate it through the resampler so the rendered output jumps to ~2000 Hz.
// This test feeds the full lookbehind, originalF0 == correctedF0 (no shift
// requested), and verifies both the detector period and the output frequency.
void testDetectorShadowDoesNotRenderOctaveAtQuantizedBoundary()
{
    constexpr int kSampleRate = 44100;
    constexpr double kFreq = 1000.0;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 4096;

    // Phase-continuous stream: kPrefix lookbehind samples then the input.
    const std::vector<float> stream = makeSineSegment(
        kFreq, kSampleRate, -kPrefix, kPrefix + kNumSamples, 0.5);

    // ---- Detector period assertion ----
    const auto frames = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        static_cast<double>(kSampleRate));

    const double expectedPeriod = static_cast<double>(kSampleRate) / kFreq; // 44.1
    int validCount = 0;
    double maxPeriodErr = 0.0;
    for (const auto& f : frames) {
        if (!f.valid)
            continue;
        ++validCount;
        maxPeriodErr = std::max(maxPeriodErr,
            std::fabs(static_cast<double>(f.periodSamples) - expectedPeriod));
    }

    expectTrue(validCount > kNumSamples * 3 / 4,
               "octave guard: 1000 Hz achieves a valid lock on most samples");
    expectTrue(maxPeriodErr < 3.0,
               "octave guard: detected period stays close to 44.1 samples "
               "(must not lock onto the 88-sample 2× harmonic)");

    // ---- Rendered output frequency assertion ----
    // originalF0 == correctedF0: no pitch shift, so the output must still be
    // ~1000 Hz. If the resampler uses the doubled period the output would
    // render at ~2000 Hz.
    const float f0 = static_cast<float>(kFreq);
    const std::vector<float> tail = makeSineSegment(
        kFreq, kSampleRate, kPrefix + kNumSamples,
        Shifter::kLookaheadSamples, 0.5);

    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> out = shifter.shiftChunk(
        stream.data() + kPrefix, kNumSamples, &f0, &f0,
        1, 100.0, 0.0, tail.data(), Shifter::kLookaheadSamples,
        stream.data(), kPrefix,
        frames.data(), static_cast<int>(frames.size()));

    expectTrue(static_cast<int>(out.size()) == kNumSamples,
               "octave guard: output length preserved");
    expectTrue(allFinite(out),
               "octave guard: output is finite");

    // Measure zero-crossing frequency over the second half to avoid
    // startup transients.
    constexpr int kHalf = kNumSamples / 2;
    const int crossings = countPositiveZeroCrossings(out, kHalf, kNumSamples);
    const double duration = static_cast<double>(kNumSamples - kHalf) / kSampleRate;
    const double measuredFreq = static_cast<double>(crossings) / duration;

    // The measured frequency must be within ~10% of 1000 Hz. A 2× lock would
    // yield ~2000 Hz; accept anything 900..1100 Hz as correct.
    expectTrue(measuredFreq > 900.0 && measuredFreq < 1100.0,
               "octave guard: rendered output frequency is within 10% of 1000 Hz");
}

// 500 Hz sine on 44.1 kHz has an 88.2-sample period �?exactly the 2× of the
// 1000 Hz test case. A detector bug that unconditionally halves the period
// would lock onto ~44.1 and render ~1000 Hz. This test verifies the detector
// reports the true ~88.2-sample period and the rendered output stays at 500 Hz.
void testDetectorShadowDoesNotHalvePeriod()
{
    constexpr int kSampleRate = 44100;
    constexpr double kFreq = 500.0;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 4096;

    const std::vector<float> stream = makeSineSegment(
        kFreq, kSampleRate, -kPrefix, kPrefix + kNumSamples, 0.5);

    // ---- Detector period assertion ----
    const auto frames = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        static_cast<double>(kSampleRate));

    const double expectedPeriod = static_cast<double>(kSampleRate) / kFreq; // 88.2
    int validCount = 0;
    double maxPeriodErr = 0.0;
    for (const auto& f : frames) {
        if (!f.valid)
            continue;
        ++validCount;
        maxPeriodErr = std::max(maxPeriodErr,
            std::fabs(static_cast<double>(f.periodSamples) - expectedPeriod));
    }

    expectTrue(validCount > kNumSamples * 3 / 4,
               "500 Hz shadow: achieves a valid lock on most samples");
    expectTrue(maxPeriodErr < expectedPeriod * 0.12,
               "500 Hz shadow: detected period stays within 12% of 88.2 "
               "(must not lock onto ~44.1 half-period)");

    // ---- Rendered output frequency assertion ----
    const float f0 = static_cast<float>(kFreq);
    const std::vector<float> tail = makeSineSegment(
        kFreq, kSampleRate, kPrefix + kNumSamples,
        Shifter::kLookaheadSamples, 0.5);

    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> out = shifter.shiftChunk(
        stream.data() + kPrefix, kNumSamples, &f0, &f0,
        1, 100.0, 0.0, tail.data(), Shifter::kLookaheadSamples,
        stream.data(), kPrefix,
        frames.data(), static_cast<int>(frames.size()));

    expectTrue(static_cast<int>(out.size()) == kNumSamples,
               "500 Hz shadow: output length preserved");
    expectTrue(allFinite(out),
               "500 Hz shadow: output is finite");

    constexpr int kHalf = kNumSamples / 2;
    const int crossings = countPositiveZeroCrossings(out, kHalf, kNumSamples);
    const double duration = static_cast<double>(kNumSamples - kHalf) / kSampleRate;
    const double measuredFreq = static_cast<double>(crossings) / duration;

    // Must be near 500 Hz; a period-halved lock would yield ~1000 Hz.
    expectTrue(measuredFreq > 400.0 && measuredFreq < 600.0,
               "500 Hz shadow: rendered output frequency is ~500 Hz, not ~1000 Hz");
}

// Weak-fundamental regression: a mixture whose 220 Hz fundamental is
// 20 dB below its 440 Hz harmonic (0.05 vs 0.5). The detector must lock onto
// the true 220 Hz period (~200.45 samples at 44.1 kHz), not the dominant 440 Hz
// harmonic (~100.23 samples). Feeding the full lookbehind and verifying the
// majority of valid frames land near Fs/220.
void testDetectorWeakFundamentalLocksToTruePeriod()
{
    constexpr double kSampleRate = 44100.0;
    constexpr double kFundamental = 220.0;          // weak component
    constexpr double kHarmonic = 440.0;              // dominant component
    constexpr double kAmpFundamental = 0.05;         // 20 dB below harmonic
    constexpr double kAmpHarmonic = 0.5;
    constexpr int kPrefix =
        OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 4096;

    const double expectedPeriod = kSampleRate / kFundamental; // ~200.45

    // Phase-continuous mixed waveform: accumulate independent phases so there
    // is no discontinuity at the prefix/input seam.
    const int total = kPrefix + kNumSamples;
    std::vector<float> stream(static_cast<size_t>(total));
    double phase220 = 0.0;
    double phase440 = 0.0;
    for (int n = -kPrefix; n < kNumSamples; ++n) {
        phase220 += kTwoPi * kFundamental / kSampleRate;
        phase440 += kTwoPi * kHarmonic / kSampleRate;
        const float sample = static_cast<float>(
            kAmpFundamental * std::sin(phase220)
            + kAmpHarmonic * std::sin(phase440));
        stream[static_cast<size_t>(n + kPrefix)] = sample;
    }

    const auto frames = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        kSampleRate);

    expectTrue(static_cast<int>(frames.size()) == kNumSamples,
               "weak fundamental: one result per input sample");

    int validCount = 0;
    int nearFundamental = 0;
    int nearHarmonic = 0;
    for (const auto& f : frames) {
        if (!f.valid)
            continue;
        ++validCount;
        const double err =
            std::fabs(static_cast<double>(f.periodSamples) - expectedPeriod);
        if (err < 8.0)
            ++nearFundamental;
        if (std::fabs(static_cast<double>(f.periodSamples)
                      - kSampleRate / kHarmonic) < 8.0)
            ++nearHarmonic;
    }

    expectTrue(validCount > kNumSamples / 2,
               "weak fundamental: majority of frames are valid");
    expectTrue(nearFundamental > validCount / 2,
               "weak fundamental: most valid frames lock near Fs/220 (~200), "
               "not Fs/440 (~100)");
    expectTrue(nearHarmonic < validCount / 4,
               "weak fundamental: few frames lock onto the 440 Hz harmonic");
}

// Regression test for the half-period waveform symmetry check: a 1000 Hz sine
// with linearly increasing amplitude (0.1 �?1.0) over the coarse window.
// Without the symmetry check, the 2P candidate (88 samples) would win because
// EH(2P) benefits from the amplitude ramp's non-stationarity. With the check,
// the two P-long halves are waveform-similar (high CC) so the detector must
// lock onto the true 44.1-sample period.
void testDetectorResistsOctaveOnAmplitudeRamp()
{
    constexpr int kSampleRate = 44100;
    constexpr double kFreq = 1000.0;
    constexpr int kPrefix = OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 4096;

    const int total = kPrefix + kNumSamples;
    std::vector<float> stream(static_cast<size_t>(total));
    for (int n = -kPrefix; n < kNumSamples; ++n) {
        const double t = static_cast<double>(n + kPrefix)
            / static_cast<double>(total);
        const double amplitude = 0.1 + 0.9 * t;
        const double phase = 2.0 * 3.14159265358979323846 * kFreq
            * static_cast<double>(n) / static_cast<double>(kSampleRate);
        stream[static_cast<size_t>(n + kPrefix)] =
            static_cast<float>(amplitude * std::sin(phase));
    }

    const auto frames = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        static_cast<double>(kSampleRate));

    const double expectedPeriod = static_cast<double>(kSampleRate) / kFreq; // 44.1
    int validCount = 0;
    int nearFundamental = 0;
    int nearOctave = 0;
    for (const auto& f : frames) {
        if (!f.valid)
            continue;
        ++validCount;
        const double err = std::fabs(
            static_cast<double>(f.periodSamples) - expectedPeriod);
        if (err < 4.0)
            ++nearFundamental;
        if (std::fabs(static_cast<double>(f.periodSamples)
                      - 2.0 * expectedPeriod) < 4.0)
            ++nearOctave;
    }

    expectTrue(validCount > kNumSamples / 2,
               "amplitude ramp: majority of frames are valid");
    expectTrue(nearFundamental > validCount / 2,
               "amplitude ramp: most valid frames lock near Fs/1000 (~44), "
               "not the 88-sample 2× harmonic");
    expectTrue(nearOctave < validCount / 10,
               "amplitude ramp: very few frames lock onto the 2× period");

    const std::vector<float> f0Hint(16, static_cast<float>(kFreq));
    const auto guidedFrames = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        static_cast<double>(kSampleRate), f0Hint.data(),
        static_cast<int>(f0Hint.size()), 100.0);
    int guidedValidCount = 0;
    int guidedNearFundamental = 0;
    int guidedNearOctave = 0;
    for (const auto& frame : guidedFrames) {
        if (!frame.valid)
            continue;
        ++guidedValidCount;
        if (std::fabs(static_cast<double>(frame.periodSamples) - expectedPeriod)
            < 4.0)
            ++guidedNearFundamental;
        if (std::fabs(static_cast<double>(frame.periodSamples)
                      - 2.0 * expectedPeriod) < 4.0)
            ++guidedNearOctave;
    }

    expectTrue(guidedValidCount > kNumSamples / 2,
               "amplitude ramp prior: qualified candidates remain valid");
    expectTrue(guidedNearFundamental > guidedValidCount / 2,
               "amplitude ramp prior: closer fundamental wins over the lower "
               "periodicity score at 2× period");
    expectTrue(guidedNearOctave < guidedValidCount / 10,
               "amplitude ramp prior: octave candidate is corrected by F0");
}

void testDetectorPriorSelectsAndTracksWaveformOctaves()
{
    constexpr int kSampleRate = 44100;
    constexpr double kFrequency = 220.0;
    constexpr int kPrefix =
        OpenTune::AutoTunePeriodDetector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 5000;
    constexpr double kFirstSampleFramePhase = 3.5;
    constexpr double kFrameRate = 100.0;
    constexpr int kRampStart = kPrefix - 1000;
    constexpr int kRampEnd = kPrefix + 1000;

    const int totalSamples = kPrefix + kNumSamples;
    std::vector<float> stream(static_cast<size_t>(totalSamples));
    for (int n = -kPrefix; n < kNumSamples; ++n) {
        const int index = n + kPrefix;
        double amplitude = 0.5;
        if (index >= kRampStart && index < kRampEnd) {
            const double ramp = static_cast<double>(index - kRampStart)
                / static_cast<double>(kRampEnd - kRampStart);
            amplitude += 0.7 * ramp;
        }
        else if (index >= kRampEnd) {
            amplitude = 1.2;
        }
        const double phase = kTwoPi * kFrequency * n / kSampleRate;
        stream[static_cast<size_t>(index)] =
            static_cast<float>(amplitude * std::sin(phase));
    }

    const auto detector = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        static_cast<double>(kSampleRate));
    const double period = static_cast<double>(kSampleRate) / kFrequency;

    const auto averagePeriod = [](const auto& frames, int begin, int end,
                                  int& validCount) {
        double sum = 0.0;
        validCount = 0;
        for (int i = begin; i < end; ++i) {
            const auto& frame = frames[static_cast<size_t>(i)];
            if (!frame.valid)
                continue;
            sum += static_cast<double>(frame.periodSamples);
            ++validCount;
        }
        return validCount > 0 ? sum / static_cast<double>(validCount) : 0.0;
    };

    int noHintCount = 0;
    const double noHintPeriod = averagePeriod(detector, 500, 1500, noHintCount);
    expectTrue(noHintCount > 800,
               "octave prior: no-hint tone remains waveform-confirmed");
    expectTrue(std::fabs(noHintPeriod - period) < 3.0,
               "octave prior: no hint retains the lower-EH fundamental");

    // Verify that both periods pass the same full-rate confirmation while
    // their E/H scores differ beyond the old tie-only prior margin.
    const auto confirmedScore = [&](int endIndex, int centerLag,
                                    double& score, double& energy) {
        score = std::numeric_limits<double>::infinity();
        energy = 0.0;
        for (int lag = centerLag - 4; lag <= centerLag + 4; ++lag) {
            double candidateEnergy = 0.0;
            double correlation = 0.0;
            const int energyBegin = endIndex - 2 * lag + 1;
            const int correlationBegin = endIndex - lag + 1;
            for (int i = energyBegin; i <= endIndex; ++i) {
                const double value = stream[static_cast<size_t>(i)];
                candidateEnergy += value * value;
            }
            for (int i = correlationBegin; i <= endIndex; ++i) {
                correlation += static_cast<double>(stream[static_cast<size_t>(i)])
                    * static_cast<double>(stream[static_cast<size_t>(i - lag)]);
            }
            const double candidateScore = candidateEnergy - 2.0 * correlation;
            if (candidateEnergy > 1.0e-6
                && candidateScore <= 0.1 * candidateEnergy
                && candidateScore < score) {
                score = candidateScore;
                energy = candidateEnergy;
            }
        }
        return std::isfinite(score);
    };

    double fundamentalScore = 0.0;
    double fundamentalEnergy = 0.0;
    double octaveScore = 0.0;
    double octaveEnergy = 0.0;
    const int firstPriorUpdateEnd = kPrefix + 225;
    const bool fundamentalConfirmed = confirmedScore(
        firstPriorUpdateEnd, static_cast<int>(std::lround(period)),
        fundamentalScore, fundamentalEnergy);
    const bool octaveConfirmed = confirmedScore(
        firstPriorUpdateEnd, static_cast<int>(std::lround(2.0 * period)),
        octaveScore, octaveEnergy);
    expectTrue(fundamentalConfirmed && octaveConfirmed,
               "octave prior: both P and 2P pass waveform confirmation");
    expectTrue(std::fabs(fundamentalScore - octaveScore)
                   > 0.005 * std::max(fundamentalEnergy, octaveEnergy),
               "octave prior: P and 2P E/H scores are not tied");

    constexpr int kNumHintFrames = 20;
    std::vector<float> hints(static_cast<size_t>(kNumHintFrames), 224.0f);
    for (int frame = 0; frame < 3; ++frame)
        hints[static_cast<size_t>(frame)] = 108.0f;
    hints[3] = 0.0f;
    for (int frame = 4; frame < 8; ++frame)
        hints[static_cast<size_t>(frame)] = 108.0f;

    const auto guided = OpenTune::AutoTunePeriodDetector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        static_cast<double>(kSampleRate), hints.data(), kNumHintFrames,
        kFrameRate, kFirstSampleFramePhase);

    // Global hint frames 4 and 8 begin between tracking updates. The held
    // sample keeps P; the next update selects the waveform-confirmed octave.
    expectTrue(!guided[224].trackingUpdated
                   && std::fabs(guided[224].periodSamples - period) < 3.0,
               "octave prior: a held sample preserves the confirmed period");
    expectTrue(guided[225].trackingUpdated
                   && std::fabs(guided[225].periodSamples - 2.0 * period) < 3.0,
               "octave prior: the first update selects the closer 2P candidate");
    expectTrue(guided[1985].trackingUpdated
                   && std::fabs(guided[1985].periodSamples - period) < 3.0,
               "octave prior: the first update after the prior changes selects P");

    int noPriorCount = 0;
    const double noPriorPeriod = averagePeriod(guided, 30, 180, noPriorCount);
    expectTrue(noPriorCount > 120,
               "octave prior: unvoiced global hint frame behaves as no prior");
    expectTrue(std::fabs(noPriorPeriod - period) < 3.0,
               "octave prior: no-prior region retains the fundamental");

    int octaveCount = 0;
    const double octavePeriod = averagePeriod(guided, 500, 1500, octaveCount);
    expectTrue(octaveCount > 800,
               "octave prior: voiced prior keeps the 2P candidate valid");
    expectTrue(std::fabs(octavePeriod - 2.0 * period) < 3.0,
               "octave prior: closer 2P prior wins despite its different E/H score");
    expectTrue(std::fabs(octavePeriod - kSampleRate / 108.0) > 3.0,
               "octave prior: output remains the measured period, not the hint");

    int switchedCount = 0;
    const double switchedPeriod = averagePeriod(guided, 2500, 3400, switchedCount);
    expectTrue(switchedCount > 700,
               "octave prior: tracking remains valid after the prior changes");
    expectTrue(std::fabs(switchedPeriod - period) < 3.0,
               "octave prior: tracking switches back to P after the next update");
    expectTrue(std::fabs(switchedPeriod - kSampleRate / 224.0) > 1.5,
               "octave prior: switched result is not copied from the biased hint");
}

// A fractional harmonic tone with a short amplitude drop makes the rounded
// half-period lag fail confirmation while its immediate neighbor qualifies.
void testDetectorTrackingConfirmsNeighboringHalfPeriod()
{
    using Detector = OpenTune::AutoTunePeriodDetector;
    constexpr double kSampleRate = 44100.0;
    constexpr double kPeriod = 32.45;
    constexpr double kPhaseCycles = 0.09375;
    constexpr int kHarmonics = 12;
    constexpr int kPrefix = Detector::kRequiredLookbehindSamples;
    constexpr int kNumSamples = 5;

    double amplitudeSum = 0.0;
    for (int h = 1; h <= kHarmonics; ++h)
        amplitudeSum += 1.0 / h;

    std::vector<float> stream(static_cast<size_t>(kPrefix + kNumSamples));
    for (int n = -kPrefix; n < kNumSamples; ++n) {
        double waveform = 0.0;
        for (int h = 1; h <= kHarmonics; ++h)
            waveform += std::sin(kTwoPi * h * (kPhaseCycles + n / kPeriod)) / h;
        const double envelope = 1.0 - 0.4 * std::clamp(
            (n + 0.5 * kPeriod + 4.0) / 8.0, 0.0, 1.0);
        stream[static_cast<size_t>(n + kPrefix)] =
            static_cast<float>(waveform * envelope / amplitudeSum);
    }

    const float hints[] = {
        static_cast<float>(kSampleRate / (2.0 * kPeriod)),
        static_cast<float>(kSampleRate / kPeriod)
    };
    // The final lookbehind sample uses the 2P hint; input[0] starts hint frame 1.
    const auto frames = Detector::analyze(
        stream.data(), kPrefix, stream.data() + kPrefix, kNumSamples,
        kSampleRate, hints, 2, 100.0, 1.0);

    const auto& first = frames[0];
    expectTrue(first.valid,
               "tracking neighbor: first sample has a confirmed period");
    expectTrue(first.trackingUpdated,
               "tracking neighbor: first sample publishes a tracking update");
    expectTrue(std::fabs(first.periodSamples - kPeriod) < 0.3,
               "tracking neighbor: first update selects the true half-period");
    expectTrue(std::fabs(first.periodSamples - std::round(first.periodSamples)) > 0.1,
               "tracking neighbor: corrected period retains fractional precision");

    for (int i = 1; i < 5; ++i) {
        const auto& held = frames[static_cast<size_t>(i)];
        expectTrue(held.valid,
                   "tracking neighbor: held samples remain valid");
        expectTrue(!held.trackingUpdated,
                   "tracking neighbor: samples 1 through 4 hold the last update");
        expectTrue(held.periodSamples == first.periodSamples,
                   "tracking neighbor: held samples preserve the period exactly");
    }
}

// ---------------------------------------------------------------------------
// 75-cent routing contract: the threshold itself belongs to the light
// AutoTune path; anything beyond it selects the vocoder. Boundary ratios are
// computed in double from exp2 so the comparison is exact.
// ---------------------------------------------------------------------------
void testPitchShiftRateContractAt75Cents()
{
    const double upper75 = std::exp2(75.0 / 1200.0);
    const double lower75 = std::exp2(-75.0 / 1200.0);
    const double above75 = std::exp2(75.5 / 1200.0);
    const double below75 = std::exp2(-75.5 / 1200.0);

    expectTrue(OpenTune::isPitchShiftRateWithinLightContract(upper75),
               "contract: exactly +75 cents is inside");
    expectTrue(OpenTune::isPitchShiftRateWithinLightContract(lower75),
               "contract: exactly -75 cents is inside");
    expectTrue(!OpenTune::isPitchShiftRateWithinLightContract(above75),
               "contract: +75.5 cents is outside");
    expectTrue(!OpenTune::isPitchShiftRateWithinLightContract(below75),
               "contract: -75.5 cents is outside");

    // 200 Hz keeps both float ratios on the inside of the exact boundary.
    const std::vector<float> original = {200.0f};
    const float atUpper = static_cast<float>(200.0 * upper75);
    const float atLower = static_cast<float>(200.0 * lower75);
    const float overUpper = static_cast<float>(200.0 * above75);
    const float overLower = static_cast<float>(200.0 * below75);

    expectTrue(!OpenTune::chunkNeedsVocoder(&atUpper, 1, original, 0),
               "routing: exactly +75 cents stays in AutoTune");
    expectTrue(!OpenTune::chunkNeedsVocoder(&atLower, 1, original, 0),
               "routing: exactly -75 cents stays in AutoTune");
    expectTrue(OpenTune::chunkNeedsVocoder(&overUpper, 1, original, 0),
               "routing: +75.5 cents selects the vocoder");
    expectTrue(OpenTune::chunkNeedsVocoder(&overLower, 1, original, 0),
               "routing: -75.5 cents selects the vocoder");
}

std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod>
makeLockedDetector(int numSamples, float periodSamples)
{
    std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod> detected(
        static_cast<size_t>(numSamples));
    for (int i = 0; i < numSamples; ++i) {
        detected[static_cast<size_t>(i)].periodSamples = periodSamples;
        detected[static_cast<size_t>(i)].valid = true;
        detected[static_cast<size_t>(i)].trackingUpdated = (i % 5 == 0);
    }
    return detected;
}

Shifter::OctaveRepairResult runFixedToneOctaveRepair(float detectorPeriod)
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192;
    constexpr int kHistorySamples = 256;
    constexpr double kFreq = 220.0;

    const std::vector<float> input = makeSineSegment(
        kFreq, kSampleRate, 0, kNumSamples, 0.5);
    const std::vector<float> history = makeSineSegment(
        kFreq, kSampleRate, -kHistorySamples, kHistorySamples, 0.5);
    const std::vector<float> tail = makeSineSegment(
        kFreq, kSampleRate, kNumSamples, Shifter::kLookaheadSamples, 0.5);
    const auto detected = makeLockedDetector(kNumSamples, detectorPeriod);

    const float f0 = static_cast<float>(kFreq);
    Shifter shifter(static_cast<double>(kSampleRate));
    return shifter.shiftChunkWithOctaveRepair(
        input.data(), kNumSamples, &f0, &f0, 1, 100.0, 0.0,
        tail.data(), Shifter::kLookaheadSamples,
        history.data(), kHistorySamples,
        detected.data(), kNumSamples, false);
}

// Detector locked an octave too low: period 400 is 2P for a 220 Hz tone.
// Repair must halve it and render the corrected 220 Hz target, never 440 Hz.
void testOctaveRepairScalesDoublePeriod()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192;
    const Shifter::OctaveRepairResult result =
        runFixedToneOctaveRepair(400.0f);

    expectTrue(result.success, "octave 2P: repair succeeds");
    expectTrue(result.octave == -1,
               "octave 2P: repair halves the detector period");
    expectTrue(static_cast<int>(result.audio.size()) == kNumSamples,
               "octave 2P: output length preserved");
    expectTrue(allFinite(result.audio), "octave 2P: output finite");
    if (static_cast<int>(result.audio.size()) != kNumSamples)
        return;

    constexpr int kBegin = 2048;
    constexpr int kEnd = kNumSamples;
    const int crossings =
        countPositiveZeroCrossings(result.audio, kBegin, kEnd);
    const double duration = static_cast<double>(kEnd - kBegin) / kSampleRate;
    const double expected = 220.0 * duration;
    expectTrue(std::fabs(static_cast<double>(crossings) - expected)
                   < expected * 0.1,
               "octave 2P: output is ~220 Hz, not ~440 Hz");
}

// Detector locked an octave too high: period 100 is P/2 for a 220 Hz tone.
// Repair must double it and render the corrected 220 Hz target.
void testOctaveRepairScalesHalfPeriod()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192;
    const Shifter::OctaveRepairResult result =
        runFixedToneOctaveRepair(100.0f);

    expectTrue(result.success, "octave P/2: repair succeeds");
    expectTrue(result.octave == 1,
               "octave P/2: repair doubles the detector period");
    expectTrue(static_cast<int>(result.audio.size()) == kNumSamples,
               "octave P/2: output length preserved");
    expectTrue(allFinite(result.audio), "octave P/2: output finite");
    if (static_cast<int>(result.audio.size()) != kNumSamples)
        return;

    constexpr int kBegin = 2048;
    constexpr int kEnd = kNumSamples;
    const int crossings =
        countPositiveZeroCrossings(result.audio, kBegin, kEnd);
    const double duration = static_cast<double>(kEnd - kBegin) / kSampleRate;
    const double expected = 220.0 * duration;
    expectTrue(std::fabs(static_cast<double>(crossings) - expected)
                   < expected * 0.1,
               "octave P/2: output is ~220 Hz");
}

void testOctaveRepairRejectsNoTrustedDetectorPeriod()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 4096;
    constexpr int kHistorySamples = 256;
    constexpr double kFreq = 220.0;

    const std::vector<float> input = makeSineSegment(
        kFreq, kSampleRate, 0, kNumSamples, 0.5);
    const std::vector<float> history = makeSineSegment(
        kFreq, kSampleRate, -kHistorySamples, kHistorySamples, 0.5);
    const std::vector<float> tail = makeSineSegment(
        kFreq, kSampleRate, kNumSamples, Shifter::kLookaheadSamples, 0.5);
    const std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod> detected(
        static_cast<size_t>(kNumSamples));

    const float f0 = static_cast<float>(kFreq);
    Shifter shifter(static_cast<double>(kSampleRate));
    const Shifter::OctaveRepairResult result =
        shifter.shiftChunkWithOctaveRepair(
            input.data(), kNumSamples, &f0, &f0, 1, 100.0, 0.0,
            tail.data(), Shifter::kLookaheadSamples,
            history.data(), kHistorySamples,
            detected.data(), kNumSamples, false);

    expectTrue(!result.success && result.audio.empty(),
               "untrusted detector: no valid periods means no DSP output");
}

void testOctaveRepairAcceptsLowFrequencyWithoutMeasurementWindow()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192;
    constexpr int kHistorySamples = 512;
    constexpr double kFrequency = 100.0;
    constexpr float kCorrectedFrequency = 103.0f;

    const std::vector<float> input = makeSineSegment(
        kFrequency, kSampleRate, 0, kNumSamples, 0.5);
    const std::vector<float> history = makeSineSegment(
        kFrequency, kSampleRate, -kHistorySamples, kHistorySamples, 0.5);
    const std::vector<float> tail = makeSineSegment(
        kFrequency, kSampleRate, kNumSamples, Shifter::kLookaheadSamples, 0.5);
    const auto detected = makeLockedDetector(
        kNumSamples, static_cast<float>(kSampleRate / kFrequency));

    const float originalF0 = static_cast<float>(kFrequency);
    Shifter shifter(static_cast<double>(kSampleRate));
    const Shifter::OctaveRepairResult result =
        shifter.shiftChunkWithOctaveRepair(
            input.data(), kNumSamples, &originalF0, &kCorrectedFrequency,
            1, 100.0, 0.0, tail.data(), Shifter::kLookaheadSamples,
            history.data(), kHistorySamples,
            detected.data(), kNumSamples, false);

    expectTrue(result.success,
               "low frequency: trusted 100 Hz period succeeds without a 512-sample measurement window");
    expectTrue(static_cast<int>(result.audio.size()) == kNumSamples,
               "low frequency: output length preserved");
}

void testOctaveRepairKeepsUnvoicedAndUneditedRegionsNeutral()
{
    constexpr int kSampleRate = 44100;
    constexpr double kFrameRate = 100.0;
    constexpr int kSamplesPerFrame = 441;
    constexpr int kNumFrames = 20;
    constexpr int kNumSamples = kSamplesPerFrame * kNumFrames;
    constexpr int kHalfSamples = kNumSamples / 2;
    constexpr int kHistorySamples = 256;

    std::vector<float> input(static_cast<size_t>(kNumSamples), 0.0f);
    for (int i = kHalfSamples; i < kNumSamples; ++i) {
        const double phase = kTwoPi * 220.0 * i / kSampleRate;
        input[static_cast<size_t>(i)] =
            static_cast<float>(0.5 * std::sin(phase));
    }
    const std::vector<float> history(static_cast<size_t>(kHistorySamples), 0.0f);
    const std::vector<float> tail = makeSineSegment(
        220.0, kSampleRate, kNumSamples, Shifter::kLookaheadSamples, 0.5);

    std::vector<float> originalF0(static_cast<size_t>(kNumFrames), 220.0f);
    std::vector<float> correctedF0(static_cast<size_t>(kNumFrames), 220.0f);
    for (int frame = 0; frame < kNumFrames / 2; ++frame) {
        originalF0[static_cast<size_t>(frame)] = 0.0f;
        correctedF0[static_cast<size_t>(frame)] = 225.0f;
    }
    for (int frame = kNumFrames / 2; frame < kNumFrames; ++frame)
        correctedF0[static_cast<size_t>(frame)] = 0.0f;

    const std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod> invalid(
        static_cast<size_t>(kNumSamples));
    Shifter shifter(static_cast<double>(kSampleRate));
    const Shifter::OctaveRepairResult result =
        shifter.shiftChunkWithOctaveRepair(
            input.data(), kNumSamples, originalF0.data(), correctedF0.data(),
            kNumFrames, kFrameRate, 0.0,
            tail.data(), Shifter::kLookaheadSamples,
            history.data(), kHistorySamples,
            invalid.data(), kNumSamples, false);

    expectTrue(result.success,
               "neutral regions: invalid detector periods are accepted for unvoiced and unedited F0");
    expectTrue(static_cast<int>(result.audio.size()) == kNumSamples,
               "neutral regions: output length preserved");
    if (static_cast<int>(result.audio.size()) != kNumSamples)
        return;
    double maxError = 0.0;
    for (int i = 0; i < kNumSamples; ++i) {
        maxError = std::max(maxError, std::fabs(static_cast<double>(
            result.audio[static_cast<size_t>(i)] - input[static_cast<size_t>(i)])));
    }
    expectTrue(maxError <= 1.0e-5,
               "neutral regions: output remains sample-exact without a confirmed period");
}

// A genuine 220 -> 440 Hz transposition with a stale detector period in the
// second half. Repair must scale only the second-half span: the first half
// stays at 220 Hz and the second half stays at 440 Hz. Scaling the whole
// detector block halves the first-half rate to 0.5 and breaks the 220 Hz
// output, which whole-chunk validation rejects.
void testOctaveRepairPreservesRealTransposition()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192;
    constexpr int kHalf = kNumSamples / 2;
    constexpr int kHistorySamples = 256;
    constexpr double kLowFreq = 220.0;
    constexpr double kHighFreq = 440.0;
    constexpr int kFrameRate = 100;

    const double oldPeriod = static_cast<double>(kSampleRate) / kLowFreq;

    // 440 * 2048 == 220 * 4096, so starting the 440 Hz half at firstSample
    // 2048 continues the phase of the 220 Hz half exactly.
    const std::vector<float> firstHalf = makeSineSegment(
        kLowFreq, kSampleRate, 0, kHalf, 0.5);
    const std::vector<float> secondHalf = makeSineSegment(
        kHighFreq, kSampleRate, 2048, kHalf, 0.5);

    std::vector<float> input(static_cast<size_t>(kNumSamples));
    std::copy(firstHalf.begin(), firstHalf.end(), input.begin());
    std::copy(secondHalf.begin(), secondHalf.end(), input.begin() + kHalf);

    const std::vector<float> history = makeSineSegment(
        kLowFreq, kSampleRate, -kHistorySamples, kHistorySamples, 0.5);
    const std::vector<float> tail = makeSineSegment(
        kHighFreq, kSampleRate, 2048 + kHalf,
        Shifter::kLookaheadSamples, 0.5);

    const int numF0Frames = static_cast<int>(std::ceil(
        static_cast<double>(kNumSamples) / kSampleRate * kFrameRate)) + 1;
    const double samplesPerFrame =
        static_cast<double>(kSampleRate) / kFrameRate;
    std::vector<float> originalF0(static_cast<size_t>(numF0Frames));
    std::vector<float> correctedF0(static_cast<size_t>(numF0Frames));
    for (int k = 0; k < numF0Frames; ++k) {
        const double frameCentre =
            (static_cast<double>(k) + 0.5) * samplesPerFrame;
        const float f0 = frameCentre < static_cast<double>(kHalf)
            ? static_cast<float>(kLowFreq)
            : static_cast<float>(kHighFreq);
        originalF0[static_cast<size_t>(k)] = f0;
        correctedF0[static_cast<size_t>(k)] = f0;
    }

    const auto detected = makeLockedDetector(
        kNumSamples, static_cast<float>(oldPeriod));

    Shifter shifter(static_cast<double>(kSampleRate));
    const Shifter::OctaveRepairResult result =
        shifter.shiftChunkWithOctaveRepair(
            input.data(), kNumSamples, originalF0.data(), correctedF0.data(),
            numF0Frames, static_cast<double>(kFrameRate), 0.0,
            tail.data(), Shifter::kLookaheadSamples,
            history.data(), kHistorySamples,
            detected.data(), kNumSamples, false);

    expectTrue(result.success, "real transposition: repair succeeds");
    expectTrue(result.octave == -1,
               "real transposition: repair halves only the stale 2P span");
    expectTrue(static_cast<int>(result.audio.size()) == kNumSamples,
               "real transposition: output length preserved");
    expectTrue(allFinite(result.audio), "real transposition: output finite");
    if (static_cast<int>(result.audio.size()) != kNumSamples)
        return;

    constexpr int kFirstBegin = 512;
    constexpr int kFirstEnd = 3500;
    const int firstCrossings = countPositiveZeroCrossings(
        result.audio, kFirstBegin, kFirstEnd);
    const double firstDuration =
        static_cast<double>(kFirstEnd - kFirstBegin) / kSampleRate;
    const double firstExpected = kLowFreq * firstDuration;
    expectTrue(std::fabs(static_cast<double>(firstCrossings) - firstExpected)
                   < firstExpected * 0.1,
               "real transposition: first half stays at 220 Hz");

    constexpr int kSecondBegin = 5000;
    constexpr int kSecondEnd = 7800;
    const int secondCrossings = countPositiveZeroCrossings(
        result.audio, kSecondBegin, kSecondEnd);
    const double secondDuration =
        static_cast<double>(kSecondEnd - kSecondBegin) / kSampleRate;
    const double secondExpected = kHighFreq * secondDuration;
    expectTrue(std::fabs(static_cast<double>(secondCrossings) - secondExpected)
                   < secondExpected * 0.1,
               "real transposition: second half stays at 440 Hz");
}

// The corrected target is fixed: a 220 Hz source corrected to 440 Hz asks for
// an octave of resampling that no detector-period scaling can deliver inside
// the light contract. Repair must fail instead of publishing 220 Hz.
void testOctaveRepairRejectsUnreachableTarget()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192;
    constexpr int kHistorySamples = 256;
    constexpr double kFreq = 220.0;

    const std::vector<float> input = makeSineSegment(
        kFreq, kSampleRate, 0, kNumSamples, 0.5);
    const std::vector<float> history = makeSineSegment(
        kFreq, kSampleRate, -kHistorySamples, kHistorySamples, 0.5);
    const std::vector<float> tail = makeSineSegment(
        kFreq, kSampleRate, kNumSamples, Shifter::kLookaheadSamples, 0.5);
    const auto detected = makeLockedDetector(kNumSamples, 200.0f);

    const float originalF0 = static_cast<float>(kFreq);
    const float correctedF0 = 440.0f;

    Shifter shifter(static_cast<double>(kSampleRate));
    const Shifter::OctaveRepairResult result =
        shifter.shiftChunkWithOctaveRepair(
            input.data(), kNumSamples, &originalF0, &correctedF0, 1, 100.0,
            0.0, tail.data(), Shifter::kLookaheadSamples,
            history.data(), kHistorySamples,
            detected.data(), kNumSamples, false);

    expectTrue(!result.success,
               "unreachable target: octave repair cannot reach 440 Hz from "
               "a 220 Hz source inside the light contract");
}

// A real 220 Hz input with originalF0 == correctedF0 == 220 Hz but a detector
// period of 210 samples (210 Hz) requests 220 * 210 / 44100 = +80.5 cents.
// Holding the previous rate would leave the output +80.5 cents away from the
// target, outside the light contract, so the light render is rejected.
void testOctaveRepairRejectsDetectorRateAbove75Cents()
{
    constexpr int kSampleRate = 44100;
    constexpr double kFreq = 220.0;
    constexpr float kDetectorPeriod = 210.0f;

    const double requestedRate =
        kFreq * static_cast<double>(kDetectorPeriod) / kSampleRate;
    expectTrue(!OpenTune::isPitchShiftRateWithinLightContract(requestedRate),
               "above 75: detector rate is outside the light contract");
    expectTrue(!OpenTune::isPitchShiftRateWithinLightContract(
                   requestedRate * 0.5)
                   && !OpenTune::isPitchShiftRateWithinLightContract(
                       requestedRate * 2.0),
               "above 75: detector rate is not octave-repairable");

    const Shifter::OctaveRepairResult result =
        runFixedToneOctaveRepair(kDetectorPeriod);

    expectTrue(!result.success || result.audio.empty(),
               "above 75: unrepairable hold miss is not published");
}

// A 500-cent target step with a detector period that keeps reporting the
// previous note requests 293.66 * 200 / 44100 = +500 cents. The target and
// original tracks step together, so the runtime routes the chunk into the
// light path; the shifter must not publish the stale-period render.
void testTargetStepWithStalePeriodRejectsLightRender()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192;
    constexpr int kHistorySamples = 256;
    constexpr int kFrameRate = 100;
    constexpr int kStepFrame = 10;
    constexpr double kLowFreq = 220.0;
    constexpr double kHighFreq = 293.66;

    const std::vector<float> input = makeSineSegment(
        200.0, kSampleRate, 0, kNumSamples, 0.5);
    const std::vector<float> history = makeSineSegment(
        200.0, kSampleRate, -kHistorySamples, kHistorySamples, 0.5);
    const std::vector<float> tail = makeSineSegment(
        200.0, kSampleRate, kNumSamples, Shifter::kLookaheadSamples, 0.5);

    const int numF0Frames = 32;
    const double samplesPerFrame = static_cast<double>(kSampleRate) / kFrameRate;
    std::vector<float> originalF0(static_cast<size_t>(numF0Frames));
    std::vector<float> correctedF0(static_cast<size_t>(numF0Frames));
    for (int k = 0; k < numF0Frames; ++k) {
        const bool afterStep = k >= kStepFrame;
        originalF0[static_cast<size_t>(k)] =
            static_cast<float>(afterStep ? kHighFreq : kLowFreq);
        correctedF0[static_cast<size_t>(k)] = originalF0[static_cast<size_t>(k)];
    }
    expectTrue(!OpenTune::chunkNeedsVocoder(
                   correctedF0.data(), numF0Frames, originalF0, 0),
               "target step: both tracks step together inside the light contract");
    expectTrue(static_cast<double>(kStepFrame) * samplesPerFrame
                   < static_cast<double>(kNumSamples),
               "target step: setup places the step inside the chunk");

    const auto detected = makeLockedDetector(
        kNumSamples, static_cast<float>(kSampleRate / kLowFreq));

    Shifter shifter(static_cast<double>(kSampleRate));
    const Shifter::OctaveRepairResult result =
        shifter.shiftChunkWithOctaveRepair(
            input.data(), kNumSamples, originalF0.data(), correctedF0.data(),
            numF0Frames, static_cast<double>(kFrameRate), 0.0,
            tail.data(), Shifter::kLookaheadSamples,
            history.data(), kHistorySamples,
            detected.data(), kNumSamples, false);

    expectTrue(!result.success || result.audio.empty(),
               "target step: stale-period request is not published");
}

// Low-frequency target change that the measurement window cannot see: the
// target moves 100 -> 103 Hz, then 100 -> 97 Hz while the detector period
// moves 441 -> 500. Holding 1.03 leaves the output ~104 cents above the 97 Hz
// target; no measurement window fits the 454-sample target period, so only
// the render-side contract can reject it.
void testLowFrequencyHoldRejectsUnverifiableTarget()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192;
    constexpr int kHistorySamples = 256;
    constexpr int kFrameRate = 100;
    constexpr int kStepFrame = 10;
    constexpr double kOriginalF0 = 100.0;
    constexpr double kFirstTargetF0 = 103.0;
    constexpr double kSecondTargetF0 = 97.0;

    const std::vector<float> input = makeSineSegment(
        kOriginalF0, kSampleRate, 0, kNumSamples, 0.5);
    const std::vector<float> history = makeSineSegment(
        kOriginalF0, kSampleRate, -kHistorySamples, kHistorySamples, 0.5);
    const std::vector<float> tail = makeSineSegment(
        kOriginalF0, kSampleRate, kNumSamples, Shifter::kLookaheadSamples, 0.5);

    const int numF0Frames = 32;
    const double samplesPerFrame = static_cast<double>(kSampleRate) / kFrameRate;
    std::vector<float> originalF0(static_cast<size_t>(numF0Frames),
                                  static_cast<float>(kOriginalF0));
    std::vector<float> correctedF0(static_cast<size_t>(numF0Frames));
    for (int k = 0; k < numF0Frames; ++k) {
        correctedF0[static_cast<size_t>(k)] = static_cast<float>(
            k >= kStepFrame ? kSecondTargetF0 : kFirstTargetF0);
    }
    expectTrue(!OpenTune::chunkNeedsVocoder(
                   correctedF0.data(), numF0Frames, originalF0, 0),
               "low frequency: both targets stay inside the light contract");

    const int stepSample = static_cast<int>(
        static_cast<double>(kStepFrame) * samplesPerFrame);
    std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod> detected(
        static_cast<size_t>(kNumSamples));
    for (int i = 0; i < kNumSamples; ++i) {
        auto& frame = detected[static_cast<size_t>(i)];
        frame.valid = true;
        frame.trackingUpdated = (i % 5 == 0);
        frame.periodSamples = static_cast<float>(i < stepSample
            ? kSampleRate / kOriginalF0
            : kSampleRate / (kOriginalF0 * 0.882));
    }

    Shifter shifter(static_cast<double>(kSampleRate));
    const Shifter::OctaveRepairResult result =
        shifter.shiftChunkWithOctaveRepair(
            input.data(), kNumSamples, originalF0.data(), correctedF0.data(),
            numF0Frames, static_cast<double>(kFrameRate), 0.0,
            tail.data(), Shifter::kLookaheadSamples,
            history.data(), kHistorySamples,
            detected.data(), kNumSamples, false);

    expectTrue(!result.success || result.audio.empty(),
               "low frequency: unverifiable held target is not published");
}

void testLightPitchRoutingBounds()
{
    const std::vector<float> original = {220.0f, 220.0f, 220.0f, 0.0f, 220.0f};
    const float below = 220.0f * std::exp2(74.0f / 1200.0f);
    const float above = 220.0f * std::exp2(76.0f / 1200.0f);
    const float corrected[] = {below, above, 220.0f, 220.0f, above};

    expectTrue(!OpenTune::chunkNeedsVocoder(corrected, 1, original, 0),
               "routing: 74 cents stays in AutoTune");
    expectTrue(OpenTune::chunkNeedsVocoder(corrected + 1, 1, original, 1),
               "routing: 76 cents uses the vocoder");
    expectTrue(OpenTune::chunkNeedsVocoder(corrected, 2, original, 0),
               "routing: any frame above 75 cents selects the vocoder");
    expectTrue(!OpenTune::chunkNeedsVocoder(corrected + 2, 1, original, 2),
               "routing: neutral frame stays in AutoTune");
    expectTrue(!OpenTune::chunkNeedsVocoder(corrected + 3, 1, original, 3),
               "routing: unvoiced frame stays in AutoTune");
    expectTrue(OpenTune::chunkNeedsVocoder(corrected + 4, 1, original, 4),
               "routing: global F0 start offset is respected");
}

// Hop-held detector samples between tracking-update events carry a stale
// period. The stale 230-sample period is outside ±75 cents (220 Hz target ->
// +238 cents) and appears only on trackingUpdated == false samples, while
// trackingUpdated == true samples keep the legal 200-sample period
// (-3.9 cents). shiftChunk must judge the rate contract on the update events
// and keep rendering instead of returning an empty chunk.
void testShiftChunkSkipsStaleHeldPeriodForRateContract()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192;
    constexpr int kHistorySamples = 256;
    constexpr double kFreq = 220.0;
    constexpr float kUpdatedPeriod = 200.0f;
    constexpr float kStalePeriod = 230.0f;

    const double updatedRate =
        kFreq * static_cast<double>(kUpdatedPeriod) / kSampleRate;
    const double staleRate =
        kFreq * static_cast<double>(kStalePeriod) / kSampleRate;
    expectTrue(OpenTune::isPitchShiftRateWithinLightContract(updatedRate),
               "stale held: update-event periods stay inside the ±75 contract");
    expectTrue(!OpenTune::isPitchShiftRateWithinLightContract(staleRate),
               "stale held: setup stale period is outside the ±75 contract");

    const std::vector<float> input = makeSineSegment(
        kFreq, kSampleRate, 0, kNumSamples, 0.5);
    const std::vector<float> history = makeSineSegment(
        kFreq, kSampleRate, -kHistorySamples, kHistorySamples, 0.5);
    const std::vector<float> tail = makeSineSegment(
        kFreq, kSampleRate, kNumSamples, Shifter::kLookaheadSamples, 0.5);

    std::vector<OpenTune::AutoTunePeriodDetector::DetectedPeriod> detected(
        static_cast<size_t>(kNumSamples));
    for (int i = 0; i < kNumSamples; ++i) {
        auto& frame = detected[static_cast<size_t>(i)];
        frame.valid = true;
        frame.trackingUpdated = (i % 5 == 0);
        frame.periodSamples =
            frame.trackingUpdated ? kUpdatedPeriod : kStalePeriod;
    }

    const float f0 = static_cast<float>(kFreq);
    Shifter shifter(static_cast<double>(kSampleRate));
    const std::vector<float> out = shifter.shiftChunk(
        input.data(), kNumSamples, &f0, &f0, 1, 100.0, 0.0,
        tail.data(), Shifter::kLookaheadSamples,
        history.data(), kHistorySamples,
        detected.data(), kNumSamples);

    expectTrue(static_cast<int>(out.size()) == kNumSamples,
               "stale held: stale non-update periods must not reject the chunk");
    expectTrue(allFinite(out), "stale held: all outputs finite");
}

} // namespace

int main()
{
    testIdentityRampSampleAlignment();
    testIdentityRampAcrossIndependentChunks();
    testNonIdentityShiftKeepsLengthFiniteAndSmooth(true);
    testNonIdentityShiftKeepsLengthFiniteAndSmooth(false);
    testNonIntegerPeriodInputsStayFinite();
    testFloatingCyclePeriodProducesTargetRate();
    testCycleTransitionsSuppressAddedHighFrequencies();
    testResamplingPreservesHighHarmonicsAndRejectsAliases();
    testCycleTransitionRateReversals();
    testNonIdentityDoesNotMoveChunkStart();
    testF0FramePhaseUsesLaterFrames();
    testTailChunksWithShortLookahead();
    testDetectorFixedSineValidPeriod();
    testDetectorRejectsNoise();
    testDetectorTracksLowAndHighTones();
    testDetectorDropsOutAcrossNoise();
    testShortUvGapWithPositiveCorrectedF0();
    testDetectorRequiresCompleteHistory();
    testShifterUsesDetectorValidityForUv();
    testShifterUsesDetectorShadowSource();
    testShifterStaysNeutralWhenDetectorInvalid();
    testDetectorMarksTrackingUpdateEvents();
    testShifterUpdatesRateOnlyOnDetectorEvents();
    testShifterKeepsAddressContinuityThroughVoicedFailure();
    testDetectorFullRateLagBounds();
    testDetectorTracksBoundaryCenterPeriods();
    testDetectorShadowDoesNotRenderOctaveAtQuantizedBoundary();
    testDetectorShadowDoesNotHalvePeriod();
    testDetectorWeakFundamentalLocksToTruePeriod();
    testDetectorResistsOctaveOnAmplitudeRamp();
    testDetectorPriorSelectsAndTracksWaveformOctaves();
    testDetectorTrackingConfirmsNeighboringHalfPeriod();
    testPitchShiftRateContractAt75Cents();
    testOctaveRepairScalesDoublePeriod();
    testOctaveRepairScalesHalfPeriod();
    testOctaveRepairRejectsNoTrustedDetectorPeriod();
    testOctaveRepairAcceptsLowFrequencyWithoutMeasurementWindow();
    testOctaveRepairKeepsUnvoicedAndUneditedRegionsNeutral();
    testOctaveRepairPreservesRealTransposition();
    testOctaveRepairRejectsUnreachableTarget();
    testOctaveRepairRejectsDetectorRateAbove75Cents();
    testTargetStepWithStalePeriodRejectsLightRender();
    testLowFrequencyHoldRejectsUnverifiableTarget();
    testLightPitchRoutingBounds();
    testShiftChunkSkipsStaleHeldPeriodForRateContract();

    if (g_failures == 0) {
        std::printf("All AutoTunePitchShifter tests passed.\n");
        return 0;
    }
    std::printf("%d test assertion(s) failed.\n", g_failures);
    return 1;
}
