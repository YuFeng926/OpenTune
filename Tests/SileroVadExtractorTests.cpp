/**
 * Tests/SileroVadExtractorTests.cpp — vocal-time-stretch §11.3 / §4.1 unit tests
 * for SileroVadExtractor lifecycle + Silero v5 ONNX inference.
 *
 * Coverage:
 *   - getModelType / getName / getModelSize / getHopSize / getTargetSampleRate
 *     return the spec'd constants
 *   - initialize() returns false on missing path; isReady stays false; extractVad
 *     returns empty (graceful degradation per §4.1)
 *   - initialize() returns true on the in-tree silero_vad.onnx (downloaded
 *     2026-05-12 from upstream master); isReady becomes true
 *   - extractVad on 1 sec of 16 kHz silence returns ~31 probabilities (one per
 *     32 ms hop) and all are well-defined floats in [0, 1]
 *   - extractVad on the same audio resampled at 44.1 kHz produces the same
 *     frame count (proves r8brain resample path works)
 *
 * Suite aggregator: runSileroVadExtractorSuite() — registered in TestMain.cpp.
 *
 * NOTE: When the model file is absent (e.g., dev environment without LFS), the
 * "ready" tests skip themselves with a logSection note rather than fail — these
 * tests are anchor-style: they verify the code path works AS LONG AS the model
 * is installed.
 */
#include "TestSupport.h"
#include "Inference/SileroVadExtractor.h"

#include <cmath>
#include <vector>

namespace {

// Locate the in-tree Silero VAD model.  Walks up from CWD looking for
// `models/silero_vad.onnx` next to a `CMakeLists.txt` (project root).
juce::String findSileroVadModel()
{
    auto cwd = juce::File::getCurrentWorkingDirectory();
    for (int depth = 0; depth < 8 && cwd.isDirectory(); ++depth) {
        if (cwd.getChildFile("CMakeLists.txt").existsAsFile()) {
            const auto candidate = cwd.getChildFile("models/silero_vad.onnx");
            if (candidate.existsAsFile()) {
                return candidate.getFullPathName();
            }
            return {};
        }
        const auto parent = cwd.getParentDirectory();
        if (parent == cwd) break;
        cwd = parent;
    }
    return {};
}

} // namespace

// ============================================================================
// Constant accessors — spec'd values do not regress
// ============================================================================

void runSileroVadExtractor_AccessorsReturnSpecdConstantsTest()
{
    constexpr const char* testName = "SileroVadExtractor_Accessors_ReturnSpecdConstants";

    SileroVadExtractor extractor;
    if (extractor.getHopSize() != 512) {
        logFail(testName, "hopSize must be 512 (32 ms @ 16 kHz)"); return;
    }
    if (extractor.getTargetSampleRate() != 16000) {
        logFail(testName, "targetSampleRate must be 16000"); return;
    }
    if (extractor.getModelType() != VadModelType::SileroV5) {
        logFail(testName, "modelType must be SileroV5"); return;
    }
    if (extractor.getName() != std::string("Silero VAD v5")) {
        logFail(testName, "name must be 'Silero VAD v5'"); return;
    }
    if (extractor.getModelSize() == 0) {
        logFail(testName, "modelSize must be non-zero (declared ~1.8 MB)"); return;
    }
    if (extractor.isReady()) {
        logFail(testName, "default-constructed extractor must NOT be ready");
        return;
    }
    logPass(testName);
}

// ============================================================================
// initialize() with missing path → graceful failure (no exception, no crash)
// ============================================================================

void runSileroVadExtractor_InitializeMissingPathFailsGracefullyTest()
{
    constexpr const char* testName = "SileroVadExtractor_Initialize_MissingPathFailsGracefully";

    SileroVadExtractor extractor;
    const bool ok = extractor.initialize("/this/path/does/not/exist/silero_vad.onnx");
    if (ok) {
        logFail(testName, "initialize must return false for missing model file");
        return;
    }
    if (extractor.isReady()) {
        logFail(testName, "isReady() must remain false after failed initialize");
        return;
    }
    // extractVad must safely return empty when not ready (no crash, no UB)
    std::vector<float> tone(16000, 0.1f);
    auto probs = extractor.extractVad(tone.data(), tone.size(), 16000);
    if (!probs.empty()) {
        logFail(testName, "extractVad must return empty when extractor is not ready");
        return;
    }
    logPass(testName);
}

// ============================================================================
// initialize() + extractVad on installed model — anchor for the actual pipeline
// ============================================================================

void runSileroVadExtractor_InferenceProducesProbsForSilenceTest()
{
    constexpr const char* testName = "SileroVadExtractor_Inference_ProducesProbsForSilence";

    const auto modelPath = findSileroVadModel();
    if (modelPath.isEmpty()) {
        logFail(testName, "models/silero_vad.onnx not found in tree — install per "
                          "models/CHECKSUMS.md before running this test");
        return;
    }

    SileroVadExtractor extractor;
    if (!extractor.initialize(modelPath.toStdString())) {
        logFail(testName, "initialize on in-tree silero_vad.onnx failed");
        return;
    }
    if (!extractor.isReady()) {
        logFail(testName, "isReady() must be true after successful initialize");
        return;
    }

    // 1.0 second of silence at 16 kHz → 31 chunks (16000 / 512 = 31.25 floored)
    constexpr int kSr = 16000;
    constexpr int kSamples = kSr;     // 1 sec
    std::vector<float> silence(kSamples, 0.0f);

    auto probs = extractor.extractVad(silence.data(), silence.size(), kSr);
    if (probs.empty()) {
        logFail(testName, "extractVad returned empty on 1 sec of silence");
        return;
    }
    const size_t expected = kSamples / 512;   // 31
    if (probs.size() != expected) {
        logFail(testName, ("expected " + std::to_string(expected)
                           + " VAD frames, got " + std::to_string(probs.size())).c_str());
        return;
    }
    for (size_t i = 0; i < probs.size(); ++i) {
        const float p = probs[i];
        if (!std::isfinite(p) || p < 0.0f || p > 1.0f) {
            logFail(testName, ("VAD probability out of [0,1] at frame " + std::to_string(i)
                               + ": " + std::to_string(p)).c_str());
            return;
        }
    }
    logPass(testName);
}

// ============================================================================
// 44.1 kHz input → r8brain resample path → same frame count as native 16 kHz
// ============================================================================

void runSileroVadExtractor_Inference44k1ResamplePathTest()
{
    constexpr const char* testName = "SileroVadExtractor_Inference_44k1ResamplePath";

    const auto modelPath = findSileroVadModel();
    if (modelPath.isEmpty()) {
        logFail(testName, "models/silero_vad.onnx not found in tree");
        return;
    }

    SileroVadExtractor extractor;
    if (!extractor.initialize(modelPath.toStdString())) {
        logFail(testName, "initialize failed");
        return;
    }

    // 1.0 second @ 44100 Hz → resampled to 16 kHz → ~31 frames
    constexpr int kSr44 = 44100;
    std::vector<float> silence44(kSr44, 0.0f);

    auto probs = extractor.extractVad(silence44.data(), silence44.size(), kSr44);
    if (probs.empty()) {
        logFail(testName, "extractVad returned empty for 44.1 kHz input");
        return;
    }
    // Resampled to 16 kHz, should produce ~31 frames (allow ±1 due to r8brain edge handling)
    if (probs.size() < 30 || probs.size() > 32) {
        logFail(testName, ("expected ~31 VAD frames after 44.1k → 16k resample, got "
                           + std::to_string(probs.size())).c_str());
        return;
    }
    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runSileroVadExtractorSuite()
{
    logSection("SileroVadExtractor");
    runSileroVadExtractor_AccessorsReturnSpecdConstantsTest();
    runSileroVadExtractor_InitializeMissingPathFailsGracefullyTest();
    runSileroVadExtractor_InferenceProducesProbsForSilenceTest();
    runSileroVadExtractor_Inference44k1ResamplePathTest();
}
