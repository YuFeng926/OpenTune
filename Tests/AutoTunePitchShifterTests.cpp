// ==============================================================================
// Tests/AutoTunePitchShifterTests.cpp
//
// Self-contained standard-library tests for OpenTune::AutoTunePitchShifter.
// Deliberately free of JUCE / Tests/TestSupport dependencies so it builds and
// runs standalone via the OPENTUNE_BUILD_TESTS CMake option.
// ==============================================================================

#include "DSP/AutoTunePitchShifter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

// ---------------------------------------------------------------------------
// 1. Identity ramp (correctedF0 == originalF0) must reproduce the input
//    sample-by-sample within 1e-5. This explicitly rejects a 5-sample delayed
//    output: cancelling that latency is the whole point of the lookahead feed.
// ---------------------------------------------------------------------------
void testIdentityRampSampleAlignment()
{
    constexpr int kSampleRate = 44100;
    constexpr int kNumSamples = 8192; // crosses the 4096-sample ring buffer seam
    constexpr int kLook = Shifter::kLookaheadSamples;

    // Tail of the generated stream doubles as the clip lookahead samples.
    const std::vector<float> input = makeNoise(kNumSamples + kLook, 12345u);
    const float* lookahead = input.data() + kNumSamples;

    // Guard the test itself: the signal must actually change across a
    // 5-sample offset, otherwise alignment below could not detect latency.
    double fiveStepVariation = 0.0;
    for (int i = kLook; i < kNumSamples; ++i) {
        fiveStepVariation += std::fabs(static_cast<double>(
            input[static_cast<size_t>(i)] - input[static_cast<size_t>(i - kLook)]));
    }
    fiveStepVariation /= static_cast<double>(kNumSamples - kLook);
    expectTrue(fiveStepVariation > 0.05,
               "identity setup: test signal must vary over a 5-sample offset");

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
    // residual group delay such as the 5 lookahead samples would produce
    // O(0.1..1) sample errors on this noise signal. Tolerance 1e-5.
    expectTrue(maxErr <= 1e-5,
               "identity: output[i] must match input[i] within 1e-5 "
               "(rejects a 5-sample delayed result)");

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
    // Both corrections remain below the production 50-cent vocoder gate.
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

    // Every output sample is an interpolation/blend of past input, so the
    // magnitude cannot leave the input envelope by any meaningful amount.
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
// 5. Clip-final chunks with missing tail lookahead (0 or fewer than 5
//    available samples): output length preserved and fully finite.
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
                   "tail (2 of 5 lookahead): length preserved");
        expectTrue(allFinite(out),
                   "tail (2 of 5 lookahead): all outputs finite");
    }
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
    testNonIdentityDoesNotMoveChunkStart();
    testF0FramePhaseUsesLaterFrames();
    testTailChunksWithShortLookahead();

    if (g_failures == 0) {
        std::printf("All AutoTunePitchShifter tests passed.\n");
        return 0;
    }
    std::printf("%d test assertion(s) failed.\n", g_failures);
    return 1;
}
