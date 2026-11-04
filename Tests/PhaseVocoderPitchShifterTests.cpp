// Regression tests for the content-anchored phase-vocoder pitch shifter.
// Independent standard-library harness; the CMake target links juce_dsp for
// the production FFT implementation.

#include "DSP/PhaseVocoderPitchShifter.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

// The test target links the production translation unit, so the signed
// interpolation tap is asserted directly.
namespace OpenTune {
namespace PhaseVocoderDetail {
std::complex<float> scaledBinPhasor(float magnitude, double weight, double phase);
}
}

namespace {

using Shifter = OpenTune::PhaseVocoderPitchShifter;

constexpr double kTwoPi = 6.283185307179586;
int g_failures = 0;

void expectTrue(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::vector<float> makeSine(double hz, double sampleRate, int numSamples, double amp)
{
    std::vector<float> out(static_cast<size_t>(numSamples));
    double phase = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        out[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(phase));
        phase += kTwoPi * hz / sampleRate;
    }
    return out;
}

std::vector<float> makeNoise(int numSamples, uint32_t seed)
{
    std::vector<float> out(static_cast<size_t>(numSamples));
    uint32_t state = seed;
    for (int i = 0; i < numSamples; ++i) {
        state = state * 1664525u + 1013904223u;
        out[static_cast<size_t>(i)] =
            (static_cast<float>((state >> 8) & 0xFFFFu) / 32768.0f - 1.0f) * 0.5f;
    }
    return out;
}

double measureFrequency(const std::vector<float>& signal, int begin, int end,
                        double sampleRate)
{
    double first = -1.0;
    double last = -1.0;
    int count = 0;
    for (int i = begin + 1; i < end; ++i) {
        const double a = signal[static_cast<size_t>(i - 1)];
        const double b = signal[static_cast<size_t>(i)];
        if (a <= 0.0 && b > 0.0) {
            const double frac = -a / (b - a);
            const double position = static_cast<double>(i - 1) + frac;
            if (first < 0.0) first = position;
            last = position;
            ++count;
        }
    }
    if (count < 2 || !(last > first)) return 0.0;
    return sampleRate * static_cast<double>(count - 1) / (last - first);
}

double cents(double measured, double target)
{
    return 1200.0 * std::log2(measured / target);
}

std::vector<float> constantF0(int numFrames, float hz)
{
    return std::vector<float>(static_cast<size_t>(numFrames), hz);
}

double rmsOf(const std::vector<float>& s, int begin, int end)
{
    double sum = 0.0;
    for (int i = begin; i < end; ++i) {
        const double value = static_cast<double>(s[static_cast<size_t>(i)]);
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<double>(end - begin));
}

double envelopeRatio(const std::vector<float>& s, int begin, int end)
{
    constexpr int kWindow = 1024;
    constexpr int kHop = 512;
    std::vector<double> rms;
    for (int start = begin; start + kWindow <= end; start += kHop)
        rms.push_back(rmsOf(s, start, start + kWindow));
    std::sort(rms.begin(), rms.end());
    const size_t low = rms.size() * 5 / 100;
    const size_t high = rms.size() * 95 / 100;
    if (rms[high] <= 0.0) return 0.0;
    return rms[low] / rms[high];
}

double amplitudeAt(const std::vector<float>& s, int begin, int end,
                   double hz, double sampleRate)
{
    const int count = end - begin;
    const double omega = kTwoPi * hz / sampleRate;
    double re = 0.0;
    double im = 0.0;
    double weightSum = 0.0;
    for (int i = 0; i < count; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i)
                                               / static_cast<double>(count - 1));
        const double value = static_cast<double>(s[static_cast<size_t>(begin + i)]);
        re += w * value * std::cos(omega * static_cast<double>(i));
        im -= w * value * std::sin(omega * static_cast<double>(i));
        weightSum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / weightSum;
}

std::vector<float> makeHarmonicTone(double f0, double sampleRate, int numSamples,
                                    int harmonics)
{
    std::vector<float> out(static_cast<size_t>(numSamples));
    double phase = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        double value = 0.0;
        for (int h = 1; h <= harmonics; ++h) {
            const double amplitude = 0.25 / std::pow(static_cast<double>(h), 1.2);
            value += amplitude * std::sin(static_cast<double>(h) * phase + 0.37 * h);
        }
        out[static_cast<size_t>(i)] = static_cast<float>(value);
        phase += kTwoPi * f0 / sampleRate;
    }
    return out;
}

// Short Hann-windowed tone burst: 1 ms at 2 kHz, amplitude 0.5, centred on
// centreSample. Silence outside the burst isolates the transient detector.
std::vector<float> makeBurst(double sampleRate, int numSamples, int centreSample)
{
    constexpr double kFrequency = 2000.0;
    constexpr double kAmplitude = 0.5;
    const int burstLength = static_cast<int>(std::lround(sampleRate * 0.001));
    std::vector<float> out(static_cast<size_t>(numSamples), 0.0f);
    const int first = centreSample - burstLength / 2;
    for (int i = 0; i < burstLength; ++i) {
        const int index = first + i;
        if (index < 0 || index >= numSamples) continue;
        const double w = 0.5 - 0.5 * std::cos(
            kTwoPi * static_cast<double>(i) / static_cast<double>(burstLength - 1));
        out[static_cast<size_t>(index)] = static_cast<float>(
            kAmplitude * w
            * std::sin(kTwoPi * kFrequency * static_cast<double>(index) / sampleRate));
    }
    return out;
}

std::vector<float> makeGlide(double startHz, double endHz, double sampleRate,
                             int numSamples)
{
    std::vector<float> out(static_cast<size_t>(numSamples));
    const double duration = static_cast<double>(numSamples) / sampleRate;
    double phase = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double hz = startHz + (endHz - startHz) * t / duration;
        out[static_cast<size_t>(i)] = static_cast<float>(0.4 * std::sin(phase));
        phase += kTwoPi * hz / sampleRate;
    }
    return out;
}

std::vector<float> makeVibrato(double centerHz, double depthCents, double rateHz,
                               double sampleRate, int numSamples)
{
    std::vector<float> out(static_cast<size_t>(numSamples));
    double phase = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double hz = centerHz
            * std::exp2(depthCents * std::sin(kTwoPi * rateHz * t) / 1200.0);
        out[static_cast<size_t>(i)] = static_cast<float>(0.4 * std::sin(phase));
        phase += kTwoPi * hz / sampleRate;
    }
    return out;
}

void testIdentityAndSubranges()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 30000;
    const std::vector<float> source = makeNoise(kSamples, 123u);
    const int numFrames = kSamples / 441 + 2;
    const auto original = constantF0(numFrames, 220.0f);

    Shifter shifter(kSampleRate);
    const auto full = shifter.render(source.data(), kSamples, 0, kSamples,
                                     original.data(), original.data(),
                                     numFrames, 100.0);
    expectTrue(static_cast<int>(full.size()) == kSamples, "identity: full length");
    double maxErr = 0.0;
    for (int i = 0; i < kSamples; ++i)
        maxErr = std::max(maxErr, std::fabs(static_cast<double>(
            full[static_cast<size_t>(i)] - source[static_cast<size_t>(i)])));
    expectTrue(maxErr < 1e-4, "identity: exact source reproduction");

    constexpr int kOffset = 10003;
    constexpr int kCount = 7000;
    const auto part = shifter.render(source.data(), kSamples, kOffset, kCount,
                                     original.data(), original.data(),
                                     numFrames, 100.0);
    expectTrue(static_cast<int>(part.size()) == kCount, "identity: range length");
    double maxRangeErr = 0.0;
    for (int i = 0; i < kCount; ++i)
        maxRangeErr = std::max(maxRangeErr, std::fabs(static_cast<double>(
            part[static_cast<size_t>(i)]
            - full[static_cast<size_t>(kOffset + i)])));
    expectTrue(maxRangeErr < 1e-4, "identity: independent range equals full slice");
}

void testSteadyShiftFollowsTarget()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 44100;
    constexpr double kSourceHz = 220.0;
    constexpr double kTargetHz = 226.5; // +50 cents

    const std::vector<float> source = makeSine(kSourceHz, kSampleRate, kSamples, 0.4);
    const int numFrames = kSamples / 441 + 3;
    const auto original = constantF0(numFrames, static_cast<float>(kSourceHz));
    const auto target = constantF0(numFrames, static_cast<float>(kTargetHz));

    Shifter shifter(kSampleRate);
    const auto out = shifter.render(source.data(), kSamples, 0, kSamples,
                                    original.data(), target.data(),
                                    numFrames, 100.0);
    expectTrue(static_cast<int>(out.size()) == kSamples, "steady: length");
    const double measured = measureFrequency(out, 8000, kSamples - 8000, kSampleRate);
    const double errorCents = cents(measured, kTargetHz);
    std::printf("STEADY source=%.2f target=%.2f measured=%.2f cents=%.2f\n",
                kSourceHz, kTargetHz, measured, errorCents);
    expectTrue(std::fabs(errorCents) < 5.0, "steady: within 5 cents of target");
}

void testIndependentRangeMatchesFull()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 60000;
    constexpr double kSourceHz = 307.8;
    constexpr double kTargetHz = 300.0;

    const std::vector<float> source = makeSine(kSourceHz, kSampleRate, kSamples, 0.35);
    const int numFrames = kSamples / 441 + 3;
    const auto original = constantF0(numFrames, static_cast<float>(kSourceHz));
    const auto target = constantF0(numFrames, static_cast<float>(kTargetHz));

    Shifter shifter(kSampleRate);
    const auto full = shifter.render(source.data(), kSamples, 0, kSamples,
                                     original.data(), target.data(),
                                     numFrames, 100.0);
    constexpr int kOffset = 11111;
    constexpr int kCount = 15000;
    const auto part = shifter.render(source.data(), kSamples, kOffset, kCount,
                                     original.data(), target.data(),
                                     numFrames, 100.0);

    const double errorCents = cents(
        measureFrequency(part, 2000, kCount - 2000, kSampleRate), kTargetHz);
    std::printf("RANGE offset=%d cents=%.2f\n", kOffset, errorCents);
    expectTrue(std::fabs(errorCents) < 5.0, "range: follows target pitch");

    double meanDiff = 0.0;
    for (int i = 2000; i < kCount - 2000; ++i)
        meanDiff += std::fabs(static_cast<double>(
            part[static_cast<size_t>(i)]
            - full[static_cast<size_t>(kOffset + i)]));
    meanDiff /= static_cast<double>(kCount - 4000);
    std::printf("RANGE meanAbsDiff=%.6f\n", meanDiff);
    expectTrue(meanDiff < 1e-3, "range: independent render equals full slice");
}

void testUnvoicedStaysNeutral()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 22050;
    constexpr int kUvBegin = 8000;
    constexpr int kUvEnd = 14000;

    const std::vector<float> source = makeSine(200.0, kSampleRate, kSamples, 0.4);
    const int numFrames = kSamples / 441 + 3;
    std::vector<float> original(static_cast<size_t>(numFrames), 200.0f);
    std::vector<float> target(static_cast<size_t>(numFrames), 210.0f);
    const int firstUvFrame = static_cast<int>(kUvBegin / 441.0);
    const int lastUvFrame = static_cast<int>(kUvEnd / 441.0);
    for (int f = firstUvFrame; f <= lastUvFrame && f < numFrames; ++f) {
        original[static_cast<size_t>(f)] = 0.0f;
        target[static_cast<size_t>(f)] = 0.0f;
    }

    Shifter shifter(kSampleRate);
    const auto out = shifter.render(source.data(), kSamples, 0, kSamples,
                                    original.data(), target.data(),
                                    numFrames, 100.0);
    double maxErr = 0.0;
    for (int i = kUvBegin + 2000; i < kUvEnd - 2000; ++i)
        maxErr = std::max(maxErr, std::fabs(static_cast<double>(
            out[static_cast<size_t>(i)] - source[static_cast<size_t>(i)])));
    std::printf("UV maxAbsDiff=%.6f\n", maxErr);
    expectTrue(maxErr < 1e-3, "UV: neutral passthrough inside unvoiced region");
}

void testLargePreviewShift()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 30000;
    constexpr double kSourceHz = 220.0;
    constexpr double kTargetHz = 440.0;

    const std::vector<float> source = makeSine(kSourceHz, kSampleRate, kSamples, 0.35);
    const int numFrames = kSamples / 441 + 3;
    const auto original = constantF0(numFrames, static_cast<float>(kSourceHz));
    const auto target = constantF0(numFrames, static_cast<float>(kTargetHz));

    Shifter shifter(kSampleRate);
    const auto out = shifter.render(source.data(), kSamples, 0, kSamples,
                                    original.data(), target.data(),
                                    numFrames, 100.0);
    const double measured = measureFrequency(out, 6000, kSamples - 6000, kSampleRate);
    const double errorCents = cents(measured, kTargetHz);
    std::printf("OCTAVE measured=%.2f cents=%.2f\n", measured, errorCents);
    expectTrue(std::fabs(errorCents) < 10.0, "octave preview: within 10 cents");
}

void testEmptyAndShortRanges()
{
    constexpr int kSampleRate = 44100;
    const std::vector<float> source = makeSine(220.0, kSampleRate, 64, 0.4);
    const auto original = constantF0(2, 220.0f);
    Shifter shifter(kSampleRate);
    expectTrue(shifter.render(source.data(), 64, 0, 0, original.data(),
                              original.data(), 2, 100.0).empty(),
               "empty output count returns empty");
    const auto shortOut = shifter.render(source.data(), 64, 0, 64, original.data(),
                                         original.data(), 2, 100.0);
    expectTrue(shortOut.size() == 64, "short clip keeps requested length");
}

void testSteadyShiftPreservesLevel()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 88200;
    constexpr double kSourceHz = 220.0;
    constexpr double kTargetHz = 226.5; // +50 cents

    const std::vector<float> source = makeSine(kSourceHz, kSampleRate, kSamples, 0.4);
    const int numFrames = kSamples * 100 / kSampleRate + 3;
    const auto original = constantF0(numFrames, static_cast<float>(kSourceHz));
    const auto target = constantF0(numFrames, static_cast<float>(kTargetHz));

    Shifter shifter(kSampleRate);
    const auto out = shifter.render(source.data(), kSamples, 0, kSamples,
                                    original.data(), target.data(),
                                    numFrames, 100.0);
    const int begin = kSampleRate / 2;
    const int end = kSampleRate * 3 / 2;
    const double inRms = rmsOf(source, begin, end);
    const double outRms = rmsOf(out, begin, end);
    const double gainDb = 20.0 * std::log10(outRms / inRms);
    std::printf("LEVEL source=%.1f target=%.1f gainDb=%.3f\n",
                kSourceHz, kTargetHz, gainDb);
    expectTrue(std::fabs(gainDb) < 1.0, "level: steady +50 cents keeps RMS");
}

void testHalfBinShiftPreservesLevel()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 88200;
    constexpr double kSourceHz = 440.0;
    const double targetHz = kSourceHz + kSampleRate / (2.0 * Shifter::kFftSize);

    const std::vector<float> source = makeSine(kSourceHz, kSampleRate, kSamples, 0.4);
    const int numFrames = kSamples * 100 / kSampleRate + 3;
    const auto original = constantF0(numFrames, static_cast<float>(kSourceHz));
    const auto target = constantF0(numFrames, static_cast<float>(targetHz));

    Shifter shifter(kSampleRate);
    const auto out = shifter.render(source.data(), kSamples, 0, kSamples,
                                    original.data(), target.data(),
                                    numFrames, 100.0);
    const int begin = kSampleRate / 2;
    const int end = kSampleRate * 3 / 2;
    const double inRms = rmsOf(source, begin, end);
    const double outRms = rmsOf(out, begin, end);
    const double gainDb = 20.0 * std::log10(outRms / inRms);
    std::printf("HALFBIN source=%.1f target=%.3f gainDb=%.3f\n",
                kSourceHz, targetHz, gainDb);
    expectTrue(std::fabs(gainDb) < 1.0, "level: half-bin shift keeps RMS");
}

void testHarmonicSpectrumPreservesShape()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 88200;
    constexpr double kSourceHz = 110.0;
    constexpr int kHarmonics = 12;
    const double targetHz = kSourceHz * std::exp2(50.0 / 1200.0);

    const std::vector<float> source =
        makeHarmonicTone(kSourceHz, kSampleRate, kSamples, kHarmonics);
    const int numFrames = kSamples * 100 / kSampleRate + 3;
    const auto original = constantF0(numFrames, static_cast<float>(kSourceHz));
    const auto target = constantF0(numFrames, static_cast<float>(targetHz));

    Shifter shifter(kSampleRate);
    const auto out = shifter.render(source.data(), kSamples, 0, kSamples,
                                    original.data(), target.data(),
                                    numFrames, 100.0);
    const int begin = kSampleRate / 2;
    const int end = kSampleRate * 3 / 2;
    double maxDeviationDb = 0.0;
    for (int h = 1; h <= kHarmonics; ++h) {
        const double expectedAmplitude = 0.25 / std::pow(static_cast<double>(h), 1.2);
        const double measured =
            amplitudeAt(out, begin, end, targetHz * h, kSampleRate);
        const double deviationDb =
            std::fabs(20.0 * std::log10(measured / expectedAmplitude));
        maxDeviationDb = std::max(maxDeviationDb, deviationDb);
    }
    std::printf("HARMONICS maxDeviationDb=%.3f\n", maxDeviationDb);
    expectTrue(maxDeviationDb < 1.5, "harmonics: every partial within 1.5 dB");
}

void testGlideEnvelopeStable()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 88200;
    constexpr double kStartHz = 150.0;
    constexpr double kEndHz = 310.0;
    constexpr double kDuration = 2.0;
    constexpr double kFrameRate = 100.0;
    const double shiftRatio = std::exp2(50.0 / 1200.0);

    const std::vector<float> source =
        makeGlide(kStartHz, kEndHz, kSampleRate, kSamples);
    const int numFrames = static_cast<int>(kDuration * kFrameRate) + 3;
    std::vector<float> original(static_cast<size_t>(numFrames));
    std::vector<float> target(static_cast<size_t>(numFrames));
    for (int f = 0; f < numFrames; ++f) {
        const double t = std::min(static_cast<double>(f) / kFrameRate, kDuration);
        const double hz = kStartHz + (kEndHz - kStartHz) * t / kDuration;
        original[static_cast<size_t>(f)] = static_cast<float>(hz);
        target[static_cast<size_t>(f)] = static_cast<float>(hz * shiftRatio);
    }

    Shifter shifter(kSampleRate);
    const auto out = shifter.render(source.data(), kSamples, 0, kSamples,
                                    original.data(), target.data(),
                                    numFrames, 100.0);
    const double ratio = envelopeRatio(out, kSampleRate / 4, kSampleRate * 7 / 4);
    std::printf("GLIDE envelopeRatio=%.4f\n", ratio);
    expectTrue(ratio > 0.90, "glide: output envelope stable");
}

void testVibratoTargetSteadyEnvelope()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 88200;
    constexpr double kCenterHz = 220.0;
    constexpr double kDepthCents = 50.0;
    constexpr double kRateHz = 5.0;
    constexpr double kDuration = 2.0;
    constexpr double kFrameRate = 100.0;

    const std::vector<float> source =
        makeVibrato(kCenterHz, kDepthCents, kRateHz, kSampleRate, kSamples);
    const int numFrames = static_cast<int>(kDuration * kFrameRate) + 3;
    std::vector<float> original(static_cast<size_t>(numFrames));
    for (int f = 0; f < numFrames; ++f) {
        const double t = static_cast<double>(f) / kFrameRate;
        original[static_cast<size_t>(f)] = static_cast<float>(
            kCenterHz
            * std::exp2(kDepthCents * std::sin(kTwoPi * kRateHz * t) / 1200.0));
    }
    const auto target = constantF0(numFrames, static_cast<float>(kCenterHz));

    Shifter shifter(kSampleRate);
    const auto out = shifter.render(source.data(), kSamples, 0, kSamples,
                                    original.data(), target.data(),
                                    numFrames, 100.0);
    const double ratio = envelopeRatio(out, kSampleRate / 4, kSampleRate * 7 / 4);
    std::printf("VIBRATO_SOURCE envelopeRatio=%.4f\n", ratio);
    expectTrue(ratio > 0.90, "vibrato: steady target envelope stable");
}

void testTargetVibratoEnvelope()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 88200;
    constexpr double kCenterHz = 220.0;
    constexpr double kDepthCents = 50.0;
    constexpr double kRateHz = 5.0;
    constexpr double kDuration = 2.0;
    constexpr double kFrameRate = 100.0;

    const std::vector<float> source = makeSine(kCenterHz, kSampleRate, kSamples, 0.4);
    const int numFrames = static_cast<int>(kDuration * kFrameRate) + 3;
    const auto original = constantF0(numFrames, static_cast<float>(kCenterHz));
    std::vector<float> target(static_cast<size_t>(numFrames));
    for (int f = 0; f < numFrames; ++f) {
        const double t = static_cast<double>(f) / kFrameRate;
        target[static_cast<size_t>(f)] = static_cast<float>(
            kCenterHz
            * std::exp2(kDepthCents * std::sin(kTwoPi * kRateHz * t) / 1200.0));
    }

    Shifter shifter(kSampleRate);
    const auto out = shifter.render(source.data(), kSamples, 0, kSamples,
                                    original.data(), target.data(),
                                    numFrames, 100.0);
    const double ratio = envelopeRatio(out, kSampleRate / 4, kSampleRate * 7 / 4);
    std::printf("VIBRATO_TARGET envelopeRatio=%.4f\n", ratio);
    expectTrue(ratio > 0.90, "vibrato: modulated target envelope stable");
}

void testSignedScatterTap()
{
    using OpenTune::PhaseVocoderDetail::scaledBinPhasor;

    constexpr float kMagnitude = 0.6f;
    constexpr double kPhase = 0.9;
    const auto positive = scaledBinPhasor(kMagnitude, 0.25, kPhase);
    const auto negative = scaledBinPhasor(kMagnitude, -0.25, kPhase);
    const auto unity = scaledBinPhasor(kMagnitude, 1.0, kPhase);

    expectTrue(std::fabs(positive.real() + negative.real()) < 1e-6f
                   && std::fabs(positive.imag() + negative.imag()) < 1e-6f,
               "scatter: negative Lanczos tap negates the complex contribution");

    const float expectedReal = kMagnitude * static_cast<float>(std::cos(kPhase));
    const float expectedImag = kMagnitude * static_cast<float>(std::sin(kPhase));
    expectTrue(std::fabs(unity.real() - expectedReal) < 1e-6f
                   && std::fabs(unity.imag() - expectedImag) < 1e-6f,
               "scatter: tap scales the phasor by the signed weight");
}

void testGlideAcrossPeakSwitchesTracksPitch()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 22050;
    constexpr double kStartHz = 400.0;
    constexpr double kEndHz = 560.0;
    constexpr double kDuration = 0.5;
    constexpr double kFrameRate = 100.0;
    const double shiftRatio = std::exp2(40.0 / 1200.0);

    const std::vector<float> source =
        makeGlide(kStartHz, kEndHz, kSampleRate, kSamples);
    const int numFrames = static_cast<int>(kDuration * kFrameRate) + 3;
    std::vector<float> original(static_cast<size_t>(numFrames));
    std::vector<float> target(static_cast<size_t>(numFrames));
    for (int f = 0; f < numFrames; ++f) {
        const double t = std::min(static_cast<double>(f) / kFrameRate, kDuration);
        const double hz = kStartHz + (kEndHz - kStartHz) * t / kDuration;
        original[static_cast<size_t>(f)] = static_cast<float>(hz);
        target[static_cast<size_t>(f)] = static_cast<float>(hz * shiftRatio);
    }

    Shifter shifter(kSampleRate);
    const auto out = shifter.render(source.data(), kSamples, 0, kSamples,
                                    original.data(), target.data(),
                                    numFrames, kFrameRate);
    constexpr int kMeasureBegin = 8000;
    constexpr int kMeasureEnd = 16000;
    const double measured =
        measureFrequency(out, kMeasureBegin, kMeasureEnd, kSampleRate);
    const double midpoint = 0.5 * (kMeasureBegin + kMeasureEnd);
    const double expected = (kStartHz
        + (kEndHz - kStartHz) * (midpoint / kSampleRate) / kDuration) * shiftRatio;
    const double errorCents = cents(measured, expected);
    std::printf("SWITCH_GLIDE expected=%.2f measured=%.2f cents=%.2f\n",
                expected, measured, errorCents);
    expectTrue(std::fabs(errorCents) < 10.0,
               "glide switch: frequency follows target across peak changes");
}

void testTransientRangeMatchesFull()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 44100;
    constexpr int kBurstCentre = 22050;
    constexpr int kOffset = 15000;
    constexpr int kCount = 25000;
    const double targetHz = 2000.0 * std::exp2(50.0 / 1200.0);

    const std::vector<float> source = makeBurst(kSampleRate, kSamples, kBurstCentre);
    const int numFrames = kSamples * 100 / kSampleRate + 3;
    const auto original = constantF0(numFrames, 2000.0f);
    const auto target = constantF0(numFrames, static_cast<float>(targetHz));

    Shifter shifter(kSampleRate);
    const auto full = shifter.render(source.data(), kSamples, 0, kSamples,
                                     original.data(), target.data(),
                                     numFrames, 100.0);
    const auto part = shifter.render(source.data(), kSamples, kOffset, kCount,
                                     original.data(), target.data(),
                                     numFrames, 100.0);

    double maxDiff = 0.0;
    for (int i = kBurstCentre + Shifter::kFftSize; i < kOffset + kCount; ++i) {
        maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(
            part[static_cast<size_t>(i - kOffset)]
            - full[static_cast<size_t>(i)])));
    }
    std::printf("TRANSIENT_RANGE maxAbsDiff=%.6f\n", maxDiff);
    expectTrue(maxDiff < 1e-3,
               "transient: independent range equals full slice after event");
}

void testTransientAttackRetainsEnergy()
{
    constexpr int kSampleRate = 44100;
    constexpr int kSamples = 44100;
    constexpr int kBurstCentre = 22050;
    constexpr int kOneMs = kSampleRate / 1000;
    constexpr int kFiveMs = 5 * kOneMs;
    const double targetHz = 2000.0 * std::exp2(50.0 / 1200.0);

    const std::vector<float> source = makeBurst(kSampleRate, kSamples, kBurstCentre);
    const int numFrames = kSamples * 100 / kSampleRate + 3;
    const auto original = constantF0(numFrames, 2000.0f);
    const auto target = constantF0(numFrames, static_cast<float>(targetHz));

    Shifter shifter(kSampleRate);
    const auto out = shifter.render(source.data(), kSamples, 0, kSamples,
                                    original.data(), target.data(),
                                    numFrames, 100.0);

    const double sourceRms =
        rmsOf(source, kBurstCentre - kOneMs, kBurstCentre + kOneMs);
    const double outputRms =
        rmsOf(out, kBurstCentre - kOneMs, kBurstCentre + kOneMs);
    const double gainDb = 20.0 * std::log10(outputRms / sourceRms);
    const double quietRms =
        rmsOf(out, kBurstCentre - kFiveMs, kBurstCentre - kOneMs);
    const double quietDb = 20.0 * std::log10(quietRms);
    std::printf("TRANSIENT_ATTACK gainDb=%.3f quietDbfs=%.2f\n", gainDb, quietDb);
    expectTrue(std::fabs(gainDb) < 3.0, "transient: attack RMS within 3 dB");
    expectTrue(quietDb < -40.0, "transient: pre-attack output below -40 dBFS");
}

} // namespace

int main()
{
    testIdentityAndSubranges();
    testSteadyShiftFollowsTarget();
    testIndependentRangeMatchesFull();
    testUnvoicedStaysNeutral();
    testLargePreviewShift();
    testEmptyAndShortRanges();
    testSteadyShiftPreservesLevel();
    testHalfBinShiftPreservesLevel();
    testHarmonicSpectrumPreservesShape();
    testGlideEnvelopeStable();
    testVibratoTargetSteadyEnvelope();
    testTargetVibratoEnvelope();
    testSignedScatterTap();
    testGlideAcrossPeakSwitchesTracksPitch();
    testTransientRangeMatchesFull();
    testTransientAttackRetainsEnergy();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PhaseVocoderPitchShifterTests passed\n");
    return 0;
}
