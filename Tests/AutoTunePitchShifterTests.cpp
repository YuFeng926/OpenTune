/**
 * Tests/AutoTunePitchShifterTests.cpp — Unit tests for hybrid AutoTune/NSF-HiFiGAN
 * chunk rendering decision gate and AutoTunePitchShifter wrapper.
 *
 * Suite aggregator: runAutoTunePitchShifterSuite() — registered in TestMain.cpp.
 *
 * Coverage:
 *   - ChunkRenderStrategy: threshold logic, edge cases (unvoiced, boundary)
 *   - AutoTunePitchShifter: construction, identity passthrough, pitch shift
 */
#include "TestSupport.h"
#include "Inference/ChunkRenderStrategy.h"
#include "DSP/AutoTunePitchShifter.h"

#include <cmath>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kF0FrameRate = 100.0;  // 100 fps

/// Generate a sine tone at given frequency
std::vector<float> makeTestSine(double freqHz, double durationSec, double amp = 0.5)
{
    const int n = static_cast<int>(std::round(durationSec * kSampleRate));
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        out[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * kPi * freqHz * i / kSampleRate));
    }
    return out;
}

/// Count zero crossings in a signal (positive-going only)
int countZeroCrossings(const std::vector<float>& signal)
{
    int crossings = 0;
    for (size_t i = 1; i < signal.size(); ++i) {
        if (signal[i - 1] <= 0.0f && signal[i] > 0.0f) {
            ++crossings;
        }
    }
    return crossings;
}

} // namespace

// ============================================================================
// ChunkRenderStrategy Tests
// ============================================================================

void runChunkRenderStrategy_AllUnvoicedTest()
{
    constexpr const char* testName = "ChunkRenderStrategy_AllUnvoiced_NoVocoder";

    // All frames unvoiced (F0 = 0) → should NOT need vocoder
    std::vector<float> corrected(100, 0.0f);
    std::vector<float> original(100, 0.0);

    const bool result = OpenTune::chunkNeedsVocoder(
        corrected.data(), 100, original, 0);

    if (result) {
        logFail(testName, "all-unvoiced chunk should not need vocoder");
        return;
    }
    logPass(testName);
}

void runChunkRenderStrategy_SmallDeviationTest()
{
    constexpr const char* testName = "ChunkRenderStrategy_SmallDeviation_NoVocoder";

    // 30 cents deviation (< 100 threshold) → should NOT need vocoder
    const float originalHz = 440.0f;
    const float correctedHz = originalHz * std::pow(2.0f, 30.0f / 1200.0f);  // +30 cents

    std::vector<float> corrected(100, correctedHz);
    std::vector<float> original(100, originalHz);

    const bool result = OpenTune::chunkNeedsVocoder(
        corrected.data(), 100, original, 0);

    if (result) {
        logFail(testName, "30-cent deviation should not need vocoder");
        return;
    }
    logPass(testName);
}

void runChunkRenderStrategy_BelowNewThresholdTest()
{
    constexpr const char* testName = "ChunkRenderStrategy_BelowNewThreshold_NoVocoder";

    // 50 cents deviation (< 100 threshold) → should NOT need vocoder
    // (previously 50 was the threshold; now raised to 100)
    const float originalHz = 440.0f;
    const float correctedHz = originalHz * std::pow(2.0f, 50.0f / 1200.0f);  // +50 cents

    std::vector<float> corrected(100, correctedHz);
    std::vector<float> original(100, originalHz);

    const bool result = OpenTune::chunkNeedsVocoder(
        corrected.data(), 100, original, 0);

    if (result) {
        logFail(testName, "50-cent deviation should no longer need vocoder after threshold raised to 100");
        return;
    }
    logPass(testName);
}

void runChunkRenderStrategy_ExactThresholdTest()
{
    constexpr const char* testName = "ChunkRenderStrategy_ExactThreshold_NeedsVocoder";

    // Exactly 100 cents deviation (>= threshold) → NEEDS vocoder
    const float originalHz = 440.0f;
    const float correctedHz = originalHz * std::pow(2.0f, 100.0f / 1200.0f);  // +100 cents

    std::vector<float> corrected(100, correctedHz);
    std::vector<float> original(100, originalHz);

    const bool result = OpenTune::chunkNeedsVocoder(
        corrected.data(), 100, original, 0);

    if (!result) {
        logFail(testName, "100-cent deviation should need vocoder");
        return;
    }
    logPass(testName);
}

void runChunkRenderStrategy_LargeDeviationTest()
{
    constexpr const char* testName = "ChunkRenderStrategy_LargeDeviation_NeedsVocoder";

    // 200 cents (2 semitones) → definitely needs vocoder
    const float originalHz = 440.0f;
    const float correctedHz = originalHz * std::pow(2.0f, 200.0f / 1200.0f);

    std::vector<float> corrected(100, correctedHz);
    std::vector<float> original(100, originalHz);

    const bool result = OpenTune::chunkNeedsVocoder(
        corrected.data(), 100, original, 0);

    if (!result) {
        logFail(testName, "200-cent deviation should need vocoder");
        return;
    }
    logPass(testName);
}

void runChunkRenderStrategy_MixedFramesOneExceedsTest()
{
    constexpr const char* testName = "ChunkRenderStrategy_MixedFrames_OneExceeds";

    // 99 frames at 60 cents, 1 frame at 120 cents → needs vocoder
    const float originalHz = 440.0f;
    const float smallShift = originalHz * std::pow(2.0f, 60.0f / 1200.0f);
    const float largeShift = originalHz * std::pow(2.0f, 120.0f / 1200.0f);

    std::vector<float> corrected(100, smallShift);
    corrected[50] = largeShift;  // single frame exceeds
    std::vector<float> original(100, originalHz);

    const bool result = OpenTune::chunkNeedsVocoder(
        corrected.data(), 100, original, 0);

    if (!result) {
        logFail(testName, "single frame exceeding threshold should trigger vocoder");
        return;
    }
    logPass(testName);
}

void runChunkRenderStrategy_OffsetStartFrameTest()
{
    constexpr const char* testName = "ChunkRenderStrategy_OffsetStartFrame";

    // Test with f0StartFrame offset into the original array
    const float originalHz = 440.0f;
    const float correctedHz = originalHz * std::pow(2.0f, 30.0f / 1200.0f);  // 30 cents

    std::vector<float> corrected(50, correctedHz);
    std::vector<float> original(200, originalHz);  // larger array

    const bool result = OpenTune::chunkNeedsVocoder(
        corrected.data(), 50, original, 100);  // start at frame 100

    if (result) {
        logFail(testName, "30-cent deviation with offset should not need vocoder");
        return;
    }
    logPass(testName);
}

void runChunkRenderStrategy_OutOfBoundsFrameSkippedTest()
{
    constexpr const char* testName = "ChunkRenderStrategy_OutOfBoundsFrameSkipped";

    // f0StartFrame + numF0Frames exceeds original size → out-of-bounds frames skipped
    const float originalHz = 440.0f;
    const float correctedHz = originalHz * std::pow(2.0f, 120.0f / 1200.0f);  // 120 cents

    std::vector<float> corrected(100, correctedHz);
    std::vector<float> original(50, originalHz);  // only 50 frames available

    // Start at 0, but corrected has 100 frames while original only has 50
    // Frames 50-99 should be skipped (out of bounds), frames 0-49 have 120 cents → needs vocoder
    const bool result = OpenTune::chunkNeedsVocoder(
        corrected.data(), 100, original, 0);

    if (!result) {
        logFail(testName, "in-bounds frames with 120-cent deviation should need vocoder");
        return;
    }
    logPass(testName);
}

void runChunkRenderStrategy_NegativeDeviationTest()
{
    constexpr const char* testName = "ChunkRenderStrategy_NegativeDeviation_NeedsVocoder";

    // -120 cents (pitch DOWN) → abs(deviation) = 120 > 100 → needs vocoder
    const float originalHz = 440.0f;
    const float correctedHz = originalHz * std::pow(2.0f, -120.0f / 1200.0f);

    std::vector<float> corrected(100, correctedHz);
    std::vector<float> original(100, originalHz);

    const bool result = OpenTune::chunkNeedsVocoder(
        corrected.data(), 100, original, 0);

    if (!result) {
        logFail(testName, "120-cent downward deviation should need vocoder");
        return;
    }
    logPass(testName);
}

// ============================================================================
// AutoTunePitchShifter Tests
// ============================================================================

void runAutoTunePitchShifter_ConstructTest()
{
    constexpr const char* testName = "AutoTunePitchShifter_Construct";

    OpenTune::AutoTunePitchShifter shifter(kSampleRate);

    // Verify construction succeeds and shiftChunk with empty input doesn't crash
    std::vector<float> emptyF0;
    auto output = shifter.shiftChunk(nullptr, 0, nullptr, nullptr, 0, kF0FrameRate);
    if (!output.empty()) {
        logFail(testName, "zero-length input should produce empty output");
        return;
    }
    logPass(testName);
}

void runAutoTunePitchShifter_IdentityPassthroughTest()
{
    constexpr const char* testName = "AutoTunePitchShifter_IdentityPassthrough";

    // When corrected F0 == original F0, output should closely match input
    OpenTune::AutoTunePitchShifter shifter(kSampleRate);

    const double durationSec = 0.1;  // 100ms — use 4410 samples (0.1s * 44100)
    auto input = makeTestSine(440.0, durationSec);
    const int numSamples = static_cast<int>(input.size());

    // F0 frames for 100ms at 100fps = 10 frames
    const int numF0Frames = static_cast<int>(durationSec * kF0FrameRate);
    std::vector<float> originalF0(static_cast<size_t>(numF0Frames), 440.0f);
    std::vector<float> correctedF0(static_cast<size_t>(numF0Frames), 440.0f);  // same = identity

    auto output = shifter.shiftChunk(
        input.data(), numSamples,
        originalF0.data(), correctedF0.data(),
        numF0Frames, kF0FrameRate);

    // Check output length matches input
    if (static_cast<int>(output.size()) != numSamples) {
        logFail(testName, ("output length mismatch: expected " + std::to_string(numSamples)
            + " got " + std::to_string(output.size())).c_str());
        return;
    }

    // Check output is not all zeros (signal passed through)
    float maxAbs = 0.0f;
    for (float s : output) {
        maxAbs = std::max(maxAbs, std::abs(s));
    }
    if (maxAbs < 0.01f) {
        logFail(testName, "output is near-silent, expected signal passthrough");
        return;
    }

    // Skip first 200 samples for latency settling; compare with 5-sample delay
    // compensation (outputAddr_ starts at -5.0, so output is inherently delayed by 5 samples)
    constexpr int kAlgorithmDelay = 5;
    constexpr int kSettlingSamples = 200;
    if (numSamples > kSettlingSamples + kAlgorithmDelay) {
        float maxError = 0.0f;
        for (int i = kSettlingSamples + kAlgorithmDelay; i < numSamples; ++i) {
            maxError = std::max(maxError, std::abs(output[static_cast<size_t>(i)]
                                                   - input[static_cast<size_t>(i - kAlgorithmDelay)]));
        }
        if (maxError > 0.05f) {
            logFail(testName, ("identity passthrough error too large after settling: " + std::to_string(maxError)).c_str());
            return;
        }
    }

    logPass(testName);
}

void runAutoTunePitchShifter_OutputLengthPreservedTest()
{
    constexpr const char* testName = "AutoTunePitchShifter_OutputLengthPreserved";

    // Even with pitch shift, output length must equal input length
    OpenTune::AutoTunePitchShifter shifter(kSampleRate);

    const double durationSec = 0.2;
    auto input = makeTestSine(440.0, durationSec);
    const int numSamples = static_cast<int>(input.size());

    const int numF0Frames = static_cast<int>(durationSec * kF0FrameRate);
    std::vector<float> originalF0(static_cast<size_t>(numF0Frames), 440.0f);
    // Shift up by 40 cents
    const float shiftedHz = 440.0f * std::pow(2.0f, 40.0f / 1200.0f);
    std::vector<float> correctedF0(static_cast<size_t>(numF0Frames), shiftedHz);

    auto output = shifter.shiftChunk(
        input.data(), numSamples,
        originalF0.data(), correctedF0.data(),
        numF0Frames, kF0FrameRate);

    if (static_cast<int>(output.size()) != numSamples) {
        logFail(testName, ("output length mismatch: expected " + std::to_string(numSamples)
            + " got " + std::to_string(output.size())).c_str());
        return;
    }
    logPass(testName);
}

void runAutoTunePitchShifter_UnvoicedFramesPassthroughTest()
{
    constexpr const char* testName = "AutoTunePitchShifter_UnvoicedFramesPassthrough";

    // When F0 = 0 (unvoiced), resampleRate = 1.0 → passthrough
    OpenTune::AutoTunePitchShifter shifter(kSampleRate);

    const double durationSec = 0.1;
    auto input = makeTestSine(440.0, durationSec);
    const int numSamples = static_cast<int>(input.size());

    const int numF0Frames = static_cast<int>(durationSec * kF0FrameRate);
    std::vector<float> originalF0(static_cast<size_t>(numF0Frames), 0.0f);   // all unvoiced
    std::vector<float> correctedF0(static_cast<size_t>(numF0Frames), 0.0f);  // all unvoiced

    auto output = shifter.shiftChunk(
        input.data(), numSamples,
        originalF0.data(), correctedF0.data(),
        numF0Frames, kF0FrameRate);

    if (static_cast<int>(output.size()) != numSamples) {
        logFail(testName, "output length mismatch");
        return;
    }

    // Output should have signal (passthrough at ratio 1.0)
    float maxAbs = 0.0f;
    for (float s : output) {
        maxAbs = std::max(maxAbs, std::abs(s));
    }
    if (maxAbs < 0.01f) {
        logFail(testName, "unvoiced passthrough should still produce signal");
        return;
    }
    logPass(testName);
}

void runAutoTunePitchShifter_PitchShiftProducesSignalTest()
{
    constexpr const char* testName = "AutoTunePitchShifter_PitchShiftProducesSignal";

    // Shift up by 40 cents → output should have signal and differ from input
    OpenTune::AutoTunePitchShifter shifter(kSampleRate);

    const double durationSec = 0.2;
    auto input = makeTestSine(440.0, durationSec);
    const int numSamples = static_cast<int>(input.size());

    const int numF0Frames = static_cast<int>(durationSec * kF0FrameRate);
    std::vector<float> originalF0(static_cast<size_t>(numF0Frames), 440.0f);
    const float shiftedHz = 440.0f * std::pow(2.0f, 40.0f / 1200.0f);
    std::vector<float> correctedF0(static_cast<size_t>(numF0Frames), shiftedHz);

    auto output = shifter.shiftChunk(
        input.data(), numSamples,
        originalF0.data(), correctedF0.data(),
        numF0Frames, kF0FrameRate);

    // Output should have signal
    float maxAbs = 0.0f;
    for (float s : output) {
        maxAbs = std::max(maxAbs, std::abs(s));
    }
    if (maxAbs < 0.01f) {
        logFail(testName, "pitch-shifted output should have signal");
        return;
    }

    // Output should differ from input (not bit-exact passthrough)
    // RMS difference > 0.01
    float sumSq = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        const float diff = output[static_cast<size_t>(i)] - input[static_cast<size_t>(i)];
        sumSq += diff * diff;
    }
    const float rmsDiff = std::sqrt(sumSq / static_cast<float>(numSamples));
    if (rmsDiff <= 0.01f) {
        logFail(testName, "pitch-shifted output should differ from input (RMS diff too small)");
        return;
    }
    logPass(testName);
}

void runAutoTunePitchShifter_ZeroLengthInputTest()
{
    constexpr const char* testName = "AutoTunePitchShifter_ZeroLengthInput";

    // Zero-length input should produce zero-length output without crash
    OpenTune::AutoTunePitchShifter shifter(kSampleRate);

    std::vector<float> originalF0;
    std::vector<float> correctedF0;
    float dummy = 0.0f;

    auto output = shifter.shiftChunk(
        &dummy, 0,
        originalF0.data(), correctedF0.data(),
        0, kF0FrameRate);

    if (!output.empty()) {
        logFail(testName, "zero-length input should produce empty output");
        return;
    }
    logPass(testName);
}

void runAutoTunePitchShifter_OctaveUpFrequencyTest()
{
    constexpr const char* testName = "AutoTunePitchShifter_OctaveUpFrequency";

    // origF0=220Hz, corrF0=440Hz (octave up)
    // Use 44100 samples (1 second) of 220Hz sine
    // Expected output: ~440Hz → ~880 zero crossings per second (440Hz * 2 crossings/cycle)
    OpenTune::AutoTunePitchShifter shifter(kSampleRate);

    const double durationSec = 1.0;
    auto input = makeTestSine(220.0, durationSec);
    const int numSamples = static_cast<int>(input.size());

    const int numF0Frames = static_cast<int>(durationSec * kF0FrameRate);
    std::vector<float> originalF0(static_cast<size_t>(numF0Frames), 220.0f);
    std::vector<float> correctedF0(static_cast<size_t>(numF0Frames), 440.0f);  // octave up

    auto output = shifter.shiftChunk(
        input.data(), numSamples,
        originalF0.data(), correctedF0.data(),
        numF0Frames, kF0FrameRate);

    if (static_cast<int>(output.size()) != numSamples) {
        logFail(testName, "output length mismatch");
        return;
    }

    // Count zero crossings (skip first 200 samples for latency)
    std::vector<float> settledOutput(output.begin() + 200, output.end());
    const int zeroXings = countZeroCrossings(settledOutput);

    // Expected: ~880 crossings (440Hz * 2 crossings/cycle), scaled to settled duration
    const double settledDurationSec = static_cast<double>(settledOutput.size()) / kSampleRate;
    const int expectedXings = static_cast<int>(440.0 * settledDurationSec);
    const int tolerance = static_cast<int>(static_cast<double>(expectedXings) * 0.15);

    if (zeroXings < expectedXings - tolerance || zeroXings > expectedXings + tolerance) {
        logFail(testName, ("octave-up frequency not approximately doubled: expected ~"
            + std::to_string(expectedXings) + " crossings, got " + std::to_string(zeroXings)
            + " (tolerance +- " + std::to_string(tolerance) + ")").c_str());
        return;
    }

    logPass(testName);
}

void runAutoTunePitchShifter_SemitoneDownTest()
{
    constexpr const char* testName = "AutoTunePitchShifter_SemitoneDown";

    // origF0=440Hz, corrF0=415.3Hz (1 semitone down)
    // Verify output differs from input and length is preserved
    OpenTune::AutoTunePitchShifter shifter(kSampleRate);

    const double durationSec = 0.2;
    auto input = makeTestSine(440.0, durationSec);
    const int numSamples = static_cast<int>(input.size());

    const int numF0Frames = static_cast<int>(durationSec * kF0FrameRate);
    std::vector<float> originalF0(static_cast<size_t>(numF0Frames), 440.0f);
    std::vector<float> correctedF0(static_cast<size_t>(numF0Frames), 415.3f);  // 1 semitone down

    auto output = shifter.shiftChunk(
        input.data(), numSamples,
        originalF0.data(), correctedF0.data(),
        numF0Frames, kF0FrameRate);

    // Length must be preserved
    if (static_cast<int>(output.size()) != numSamples) {
        logFail(testName, ("output length mismatch: expected " + std::to_string(numSamples)
            + " got " + std::to_string(output.size())).c_str());
        return;
    }

    // Output should differ from input
    float diffSum = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        diffSum += std::abs(output[static_cast<size_t>(i)] - input[static_cast<size_t>(i)]);
    }
    if (diffSum < 1.0f) {
        logFail(testName, "semitone-down output should differ from input");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite Aggregator
// ============================================================================

void runAutoTunePitchShifterSuite()
{
    std::cout << "\n=== AutoTune PitchShifter ===" << std::endl;

    // ChunkRenderStrategy tests
    runChunkRenderStrategy_AllUnvoicedTest();
    runChunkRenderStrategy_SmallDeviationTest();
    runChunkRenderStrategy_BelowNewThresholdTest();
    runChunkRenderStrategy_ExactThresholdTest();
    runChunkRenderStrategy_LargeDeviationTest();
    runChunkRenderStrategy_MixedFramesOneExceedsTest();
    runChunkRenderStrategy_OffsetStartFrameTest();
    runChunkRenderStrategy_OutOfBoundsFrameSkippedTest();
    runChunkRenderStrategy_NegativeDeviationTest();

    // AutoTunePitchShifter tests
    runAutoTunePitchShifter_ConstructTest();
    runAutoTunePitchShifter_IdentityPassthroughTest();
    runAutoTunePitchShifter_OutputLengthPreservedTest();
    runAutoTunePitchShifter_UnvoicedFramesPassthroughTest();
    runAutoTunePitchShifter_PitchShiftProducesSignalTest();
    runAutoTunePitchShifter_ZeroLengthInputTest();
    runAutoTunePitchShifter_OctaveUpFrequencyTest();
    runAutoTunePitchShifter_SemitoneDownTest();
}
