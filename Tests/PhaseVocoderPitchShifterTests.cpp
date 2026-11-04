// Regression tests for the content-anchored phase-vocoder pitch shifter.
// Independent standard-library harness; the CMake target links juce_dsp for
// the production FFT implementation.

#include "DSP/PhaseVocoderPitchShifter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

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

} // namespace

int main()
{
    testIdentityAndSubranges();
    testSteadyShiftFollowsTarget();
    testIndependentRangeMatchesFull();
    testUnvoicedStaysNeutral();
    testLargePreviewShift();
    testEmptyAndShortRanges();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PhaseVocoderPitchShifterTests passed\n");
    return 0;
}
