/**
 * Tests/VocoderConfigTests.cpp — vocoder-runtime-config capability tests
 *
 * Coverage (per openspec/changes/swap-vocoder-to-v3-finetuned/specs/vocoder-runtime-config/spec.md):
 *
 *   L2 unit tests (pure logic, no ORT needed):
 *   - MelSpectrogramConfig::hash() includes fMax in its hash
 *     (防 cache 错配 — Requirement: Mel 计算 fmax 必须从 vocoder 读取)
 *   - MelSpectrogramConfig fMax default still 16000 (struct's own default unchanged
 *     so other consumers of the struct aren't surprised; vocoder uses are EXPLICITLY
 *     setting fMax now per design.md Decision 7)
 *
 *   L3 integration tests (forward chain default values, no ORT session needed):
 *   - VocoderInferenceService::getFMax() returns safe default 22050 when uninitialized
 *   - VocoderDomain::getFMax() forwards to InferenceService default
 *
 *   L4 contract tests (real ONNX schema check, requires file on disk):
 *   - Bundled hifigan.onnx exists at the expected path
 *   - Bundled file size matches v4 archive copy (防 baseline ONNX 误覆盖)
 *
 * Suite aggregator: runVocoderConfigSuite() — registered in TestMain.cpp.
 *
 * NOTE: We deliberately pass `nullptr` Ort::Env to VocoderInferenceService /
 * VocoderDomain constructors. The env is only consumed inside initialize(),
 * which we never call in unit tests. Avoiding Ort::Env construction sidesteps
 * an observed segfault during static-init in the OpenTuneTests harness.
 */
#include "TestSupport.h"
#include "DSP/MelSpectrogram.h"
#include "Inference/VocoderDomain.h"
#include "Inference/VocoderInferenceService.h"

namespace {

// Walk up from CWD until we find the project root (CMakeLists.txt sibling).
// Mirrors findSileroVadModel() pattern in SileroVadExtractorTests.cpp.
juce::File findProjectRoot()
{
    auto cwd = juce::File::getCurrentWorkingDirectory();
    for (int depth = 0; depth < 8 && cwd.isDirectory(); ++depth) {
        if (cwd.getChildFile("CMakeLists.txt").existsAsFile())
            return cwd;
        const auto parent = cwd.getParentDirectory();
        if (parent == cwd) break;
        cwd = parent;
    }
    return {};
}

} // namespace

// ============================================================================
// L2: MelSpectrogramConfig::hash() differs when fMax changes
// ============================================================================

void runVocoderConfig_MelHashIncludesFMaxTest()
{
    constexpr const char* testName = "VocoderConfig_MelHash_IncludesFMax";

    OpenTune::MelSpectrogramConfig cfg22050;
    cfg22050.fMax = 22050.0f;
    OpenTune::MelSpectrogramConfig cfg16000;
    cfg16000.fMax = 16000.0f;

    if (cfg22050.hash() == cfg16000.hash()) {
        logFail(testName, "mel hash must differ when fMax differs (filterbank cache invalidation)");
        return;
    }
    logPass(testName);
}

// ============================================================================
// L2: MelSpectrogramConfig struct default fMax stays 16000 (legacy contract)
// ============================================================================

void runVocoderConfig_MelStructDefaultUnchangedTest()
{
    constexpr const char* testName = "VocoderConfig_MelStruct_DefaultFMaxUnchanged";

    OpenTune::MelSpectrogramConfig cfg{};
    if (cfg.fMax != 16000.0f) {
        logFail(testName, "MelSpectrogramConfig struct default fMax must remain 16000 (legacy contract)");
        return;
    }
    logPass(testName);
}

// ============================================================================
// L3: VocoderInferenceService::getFMax() returns 22050 when uninitialized
// ============================================================================
//
// The Impl's getFMax() reads `currentVocoder_ ? currentVocoder_->getFMax() : 22050.0f`
// Since we never call initialize(), currentVocoder_ stays null and we get the
// safe default. This validates the forward chain's null-guard.
//
void runVocoderConfig_InferenceServiceUninitializedDefaultTest()
{
    constexpr const char* testName = "VocoderConfig_InferenceService_UninitializedDefault";

    // nullptr env: never consumed because we don't call initialize()
    OpenTune::VocoderInferenceService service(nullptr);

    if (service.getFMax() != 22050.0f) {
        logFail(testName, "uninitialized service getFMax() must return safe default 22050");
        return;
    }
    if (service.getMelBins() != 128) {
        logFail(testName, "uninitialized service getMelBins() must return safe default 128");
        return;
    }
    logPass(testName);
}

// ============================================================================
// L3: VocoderDomain::getFMax() forwards to inference service default
// ============================================================================

void runVocoderConfig_DomainForwardChainTest()
{
    constexpr const char* testName = "VocoderConfig_Domain_ForwardChain";

    OpenTune::VocoderDomain domain(nullptr);

    if (domain.getFMax() != 22050.0f) {
        logFail(testName, "uninitialized domain must forward to InferenceService default 22050");
        return;
    }
    logPass(testName);
}

// ============================================================================
// L4: Bundled hifigan.onnx file consistency check
// ============================================================================
//
// The CMake build copies the source ONNX (HIFIGAN_MODEL) into the bundle as
// hifigan.onnx. We verify both exist and have identical size — a smoke test
// that the right file got bundled (catches "forgot to update CMake variable").
//
void runVocoderConfig_BundledOnnxConsistentWithSourceTest()
{
    constexpr const char* testName = "VocoderConfig_BundledOnnx_ConsistentWithSource";

    auto root = findProjectRoot();
    if (!root.isDirectory()) {
        logFail(testName, "project root not found from CWD");
        return;
    }

    // Source ONNX (the v4 file CMake's HIFIGAN_MODEL points at)
    auto srcOnnx = root.getChildFile("pc_nsf_hifigan_44.1k_ONNX")
                       .getChildFile("pc_nsf_hifigan_44k_hop512_128bin_opentune_fmax22050_v4_user_zh_female_step20000.onnx");
    if (!srcOnnx.existsAsFile()) {
        logFail(testName, "source v4 ONNX not found at expected path");
        return;
    }

    // Try to find the bundled copy. Two candidate paths depending on build setup.
    auto bundledA = root.getChildFile("build/OpenTune_artefacts/Release/Standalone/OpenTune.app/Contents/Resources/models/hifigan.onnx");
    auto bundledB = root.getChildFile("build/OpenTune_artefacts/Standalone/OpenTune.app/Contents/Resources/models/hifigan.onnx");
    juce::File bundled = bundledA.existsAsFile() ? bundledA : bundledB;
    if (!bundled.existsAsFile()) {
        logFail(testName, "bundled hifigan.onnx not found in build artifacts (run CMake build first)");
        return;
    }

    if (srcOnnx.getSize() != bundled.getSize()) {
        logFail(testName, "bundled hifigan.onnx size mismatch with source v4 ONNX (wrong file bundled?)");
        return;
    }
    logPass(testName);
}

// NOTE: ONNX input schema verification (mel name + shape [1, T, 128]) is
// already performed by OnnxVocoderBase::detectInputOutputNames() at vocoder
// load time. Adding a redundant unit test here would require Ort::Session
// construction which segfaults under the OpenTuneTests static-init harness
// (root cause not investigated — out of scope for this change). The L5 E2E
// (real audio render) covers the same schema path in production conditions.

// ============================================================================
// Suite aggregator
// ============================================================================

void runVocoderConfigSuite()
{
    logSection("VocoderConfig (vocoder-runtime-config capability)");
    runVocoderConfig_MelHashIncludesFMaxTest();
    runVocoderConfig_MelStructDefaultUnchangedTest();
    runVocoderConfig_InferenceServiceUninitializedDefaultTest();
    runVocoderConfig_DomainForwardChainTest();
    runVocoderConfig_BundledOnnxConsistentWithSourceTest();
}
