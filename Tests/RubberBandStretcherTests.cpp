/**
 * Tests/RubberBandStretcherTests.cpp — Unit tests for §5 RubberBandStretcher wrapper.
 *
 * Spec coverage:
 *   - openspec/changes/vocal-time-stretch/specs/rubberband-time-stretcher/spec.md
 *
 * Suite aggregator: runRubberBandStretcherSuite() — registered in TestMain.cpp.
 */
#include "TestSupport.h"
#include "Inference/RubberBandStretcher.h"
#include "Utils/TimeGrid.h"

#include <cmath>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;

std::vector<float> makeSineTone(double freqHz, double durationSec, double amp = 0.4)
{
    const int n = static_cast<int>(std::round(durationSec * kSampleRate));
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        out[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * M_PI * freqHz * i / kSampleRate));
    }
    return out;
}

} // namespace

// ============================================================================
// Construction + initial phase
// ============================================================================

void runRubberBandStretcherConstructTest()
{
    constexpr const char* testName = "RubberBandStretcher_Construct";

    RubberBandStretcher rb(kSampleRate, /*channels=*/1);
    if (rb.phase() != RubberBandStretcher::Phase::Idle) {
        logFail(testName, "freshly-constructed stretcher should be in Idle phase");
        return;
    }
    logPass(testName);
}

// ============================================================================
// buildKeyframesFromTimeGrid
// ============================================================================

void runRubberBandStretcherKeyframeMapMatchesHandlesTest()
{
    constexpr const char* testName = "RubberBandStretcher_KeyframeMapMatchesHandles";

    RubberBandStretcher rb(kSampleRate, 1);

    auto snap = TimeGridSnapshot::makeIdentity(12.0);
    if (!snap) {
        logFail(testName, "makeIdentity failed");
        return;
    }

    auto kf = rb.buildKeyframesFromTimeGrid(*snap);

    if (kf.size() != snap->handles().size()) {
        logFail(testName, "keyframe count != handle count");
        return;
    }

    // Identity grid: all handle src→out should map src_sample → src_sample
    for (const auto& h : snap->handles()) {
        const size_t expectedSample = static_cast<size_t>(std::llround(h.source_seconds * kSampleRate));
        auto it = kf.find(expectedSample);
        if (it == kf.end()) {
            logFail(testName, ("missing keyframe for handle src_seconds="
                               + std::to_string(h.source_seconds)).c_str());
            return;
        }
        if (it->second != expectedSample) {
            logFail(testName, "identity grid keyframe value should equal key");
            return;
        }
    }

    logPass(testName);
}

void runRubberBandStretcherIdentityKeyframeMapTest()
{
    constexpr const char* testName = "RubberBandStretcher_IdentityKeyframeMap";

    RubberBandStretcher rb(kSampleRate, 1);

    // Custom grid with non-identity middle handle but identity endpoints
    std::vector<TimeHandle> handles = {
        {1, 0.0,  0.0,   HandleKind::ClipStart, true},
        {2, 4.0,  4.0,   HandleKind::OnsetVoiced, false},  // identity here
        {3, 12.0, 12.0,  HandleKind::ClipEnd, true},
    };
    auto snap = TimeGridSnapshot::makeFromHandles(handles, 1);
    if (!snap) {
        logFail(testName, "makeFromHandles failed");
        return;
    }

    auto kf = rb.buildKeyframesFromTimeGrid(*snap);

    // For identity handle (src==out), keyframe value should equal key
    const size_t midSample = static_cast<size_t>(std::llround(4.0 * kSampleRate));
    auto it = kf.find(midSample);
    if (it == kf.end() || it->second != midSample) {
        logFail(testName, "identity middle handle should map src_sample→src_sample");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Full rebuild cycle: beginRebuild → study → process → retrieve
// ============================================================================

void runRubberBandStretcherFullCycleIdentityTest()
{
    constexpr const char* testName = "RubberBandStretcher_FullCycle_IdentityRatio";

    RubberBandStretcher rb(kSampleRate, 1);

    // Build identity TimeGrid (1 second clip, no stretching)
    auto snap = TimeGridSnapshot::makeIdentity(1.0);
    if (!snap) { logFail(testName, "makeIdentity failed"); return; }

    auto kf = rb.buildKeyframesFromTimeGrid(*snap);

    // Begin rebuild with ratio=1.0
    rb.beginRebuild(/*timeRatio=*/1.0, kf);
    if (rb.phase() != RubberBandStretcher::Phase::Studying) {
        logFail(testName, "after beginRebuild, should be in Studying phase");
        return;
    }

    // Generate 1 second of 220 Hz sine
    auto audio = makeSineTone(220.0, 1.0);

    // Pass 1 — Study (single block, isLast=true)
    rb.study(audio.data(), audio.size(), /*isLast=*/true);
    if (rb.phase() != RubberBandStretcher::Phase::Processing) {
        logFail(testName, "after study(isLast=true), should be in Processing phase");
        return;
    }

    // Pass 2 — Process (single block, isLast=true)
    rb.process(audio.data(), audio.size(), /*isLast=*/true);

    // Drain output via retrieve
    std::vector<float> output(audio.size() * 2, 0.0f);  // generous buffer
    size_t totalRetrieved = 0;
    int safetyIterations = 200;
    while (rb.available() > 0 && safetyIterations-- > 0) {
        const size_t want = std::min<size_t>(rb.available(), output.size() - totalRetrieved);
        if (want == 0) break;
        const size_t got = rb.retrieve(output.data() + totalRetrieved, want);
        if (got == 0) break;
        totalRetrieved += got;
    }

    // For identity ratio with offline mode, output samples should approximately
    // equal input samples (RB does padding/delay compensation in offline mode).
    if (totalRetrieved < audio.size() * 0.7) {
        logFail(testName, ("expected ~" + std::to_string(audio.size())
                           + " output samples, got " + std::to_string(totalRetrieved)).c_str());
        return;
    }

    // Sanity: output should not be all zeros
    bool nonZero = false;
    for (size_t i = 0; i < totalRetrieved; ++i) {
        if (std::abs(output[i]) > 1e-4f) { nonZero = true; break; }
    }
    if (!nonZero) {
        logFail(testName, "output should not be all zeros");
        return;
    }

    logPass(testName);
}

// ============================================================================
// State-machine guards
// ============================================================================

void runRubberBandStretcherProcessRejectedInIdleTest()
{
    constexpr const char* testName = "RubberBandStretcher_ProcessRejectedInIdle";

    RubberBandStretcher rb(kSampleRate, 1);

    auto audio = makeSineTone(220.0, 0.1);

    // Calling process() in Idle phase should be a no-op (warning logged).
    // We can't easily catch warnings, but we can check that available() is 0 after.
    rb.process(audio.data(), audio.size(), true);
    if (rb.available() != 0) {
        logFail(testName, "process() in Idle should not produce output");
        return;
    }
    if (rb.phase() != RubberBandStretcher::Phase::Idle) {
        logFail(testName, "phase should remain Idle after invalid process() call");
        return;
    }

    logPass(testName);
}

// ============================================================================
// tune-rubberband-options-for-vocal: option-bit assertions
//
// Verify the vocal-tuned RB option configuration (FINAL post-L5-round-2 config).
// Bit values copied from ThirdParty/rubberband-4.0.0/rubberband/RubberBandStretcher.h
// enums. Hard-coded here rather than including the RB header — OpenTuneTests target
// doesn't have rubberband on its include path, and the bit values are part of RB's
// stable ABI.
//
// FINAL chosen config (v5; phase-purity-first; WindowStandard retained after v4 WindowShort
// trial showed no audible improvement):
//   TransientsSmooth (0x200)        ⭐ never resets phase
//   DetectorCompound (0x000)        default — irrelevant under Smooth
//   WindowStandard   (0x000)        default — full R3 multi-resolution processing
//   EngineFiner, FormantPreserved, PitchHighQuality, ProcessOffline kept
//
// NOTE: SmoothingOn/Off and PhaseLaminar/Independent are R2-only (R3 ignores them per RB
// header lines 240, 311). PitchHigh{Speed,Quality,Consistency} are not adjustable in offline
// mode (line 338). Earlier test iterations asserted these flags but the assertions were
// against no-op state under our R3+Offline config — they passed but verified nothing audible.
// We keep PitchHighQuality in the bitmap for forward-compat; no longer assert SmoothingOff.
//
// Bit constants:
//   OptionProcessRealTime   = 0x00000001  (banned; Offline = 0)
//   OptionTransientsMixed   = 0x00000100  (banned)
//   OptionTransientsSmooth  = 0x00000200  ⭐ desired
//   OptionDetectorPercussive= 0x00000400  (banned)
//   OptionDetectorSoft      = 0x00000800  (banned — caused consonant chorus, v1)
//   OptionWindowShort       = 0x00100000  ⭐ desired (R3 single-resolution)
//   OptionWindowLong        = 0x00200000  (R3 ignores, treated as Standard)
//   OptionFormantPreserved  = 0x01000000  ⭐ desired
//   OptionPitchHighQuality  = 0x02000000
//   OptionEngineFiner       = 0x20000000  ⭐ desired (R3)
// ============================================================================

namespace {

constexpr int kRBOptionProcessRealTime    = 0x00000001;
constexpr int kRBOptionTransientsMixed    = 0x00000100;
constexpr int kRBOptionTransientsSmooth   = 0x00000200;
constexpr int kRBOptionDetectorPercussive = 0x00000400;
constexpr int kRBOptionDetectorSoft       = 0x00000800;
constexpr int kRBOptionWindowShort        = 0x00100000;
constexpr int kRBOptionWindowLong         = 0x00200000;
constexpr int kRBOptionSmoothingOn        = 0x00800000;
constexpr int kRBOptionFormantPreserved   = 0x01000000;
constexpr int kRBOptionEngineFiner        = 0x20000000;

void runRubberBandOptions_R3EngineConfiguredTest()
{
    constexpr const char* testName = "RubberBandOptions_R3EngineConfigured";
    const int opts = RubberBandStretcher::getRubberBandOptions();
    if (!(opts & kRBOptionEngineFiner)) {
        logFail(testName, "OptionEngineFiner bit not set");
        return;
    }
    logPass(testName);
}

void runRubberBandOptions_OfflineModeConfiguredTest()
{
    constexpr const char* testName = "RubberBandOptions_OfflineModeConfigured";
    const int opts = RubberBandStretcher::getRubberBandOptions();
    if (opts & kRBOptionProcessRealTime) {
        logFail(testName, "RealTime bit set unexpectedly");
        return;
    }
    logPass(testName);
}

void runRubberBandOptions_FormantPreservedTest()
{
    constexpr const char* testName = "RubberBandOptions_FormantPreserved";
    const int opts = RubberBandStretcher::getRubberBandOptions();
    if (!(opts & kRBOptionFormantPreserved)) {
        logFail(testName, "OptionFormantPreserved bit not set");
        return;
    }
    logPass(testName);
}

void runRubberBandOptions_SmoothTransientsConfiguredTest()
{
    // FINAL config: TransientsSmooth (0x200) chosen for max phase continuity.
    // Mixed (0x100) was tried but consonant chorus persisted; user opted for full
    // phase purity at the cost of consonant sharpness.
    constexpr const char* testName = "RubberBandOptions_SmoothTransientsConfigured";
    const int opts = RubberBandStretcher::getRubberBandOptions();
    if (!(opts & kRBOptionTransientsSmooth)) {
        logFail(testName, "OptionTransientsSmooth bit (0x200) not set");
        return;
    }
    if (opts & kRBOptionTransientsMixed) {
        logFail(testName, "OptionTransientsMixed bit (0x100) set; should not be");
        return;
    }
    logPass(testName);
}

void runRubberBandOptions_CompoundDetectorConfiguredTest()
{
    // DetectorCompound = 0x00000000 (default). Verify presence by ensuring neither
    // Percussive (0x400) nor Soft (0x800) is set.
    // Soft was tried but caused chorus-like artifact on consonants; reverted to
    // Compound for correct consonant-onset sensitivity.
    constexpr const char* testName = "RubberBandOptions_CompoundDetectorConfigured";
    const int opts = RubberBandStretcher::getRubberBandOptions();
    if (opts & kRBOptionDetectorPercussive) {
        logFail(testName, "OptionDetectorPercussive bit (0x400) set; should not be");
        return;
    }
    if (opts & kRBOptionDetectorSoft) {
        logFail(testName, "OptionDetectorSoft bit (0x800) set; should not be (Soft caused consonant chorus)");
        return;
    }
    logPass(testName);
}

void runRubberBandOptions_WindowStandardConfiguredTest()
{
    // FINAL config (v5): WindowStandard (default = 0). v4 tried WindowShort to eliminate
    // R3 multi-resolution fusion artifact, but L5 user A/B showed no audible improvement,
    // so WindowStandard restored to retain R3's full quality multi-resolution processing.
    // Verify presence by ensuring neither Short (0x100000) nor Long (0x200000) is set.
    constexpr const char* testName = "RubberBandOptions_WindowStandardConfigured";
    const int opts = RubberBandStretcher::getRubberBandOptions();
    if (opts & kRBOptionWindowShort) {
        logFail(testName, "OptionWindowShort bit (0x100000) set; should not be");
        return;
    }
    if (opts & kRBOptionWindowLong) {
        logFail(testName, "OptionWindowLong bit (0x200000) set; should not be");
        return;
    }
    logPass(testName);
}

void runRubberBandOptions_BannedOptionsAbsentTest()
{
    // Verifies non-zero option bits banned by FINAL config (v5):
    //   - TransientsMixed (0x100):    tried v1/v2, didn't fully fix chorus, replaced with Smooth
    //   - DetectorPercussive (0x400): wrong direction (would chase transients)
    //   - DetectorSoft (0x800):       caused consonant chorus, v1
    //   - WindowShort (0x100000):     tried v4, no audible improvement, reverted
    //   - WindowLong (0x200000):      R3 ignores (treats as Standard); banned for clarity
    // SmoothingOn (0x800000): not asserted — R2-only (R3 ignores), so its presence
    // would have no audible effect.
    // R2 (EngineFaster=0) and FormantShifted (=0) are 0-valued defaults; their
    // absence is implied by EngineFiner / FormantPreserved being set.
    constexpr const char* testName = "RubberBandOptions_BannedOptionsAbsent";
    const int opts = RubberBandStretcher::getRubberBandOptions();
    if (opts & kRBOptionTransientsMixed) {
        logFail(testName, "TransientsMixed (0x100) banned but set");
        return;
    }
    if (opts & kRBOptionDetectorPercussive) {
        logFail(testName, "DetectorPercussive (0x400) banned but set");
        return;
    }
    if (opts & kRBOptionDetectorSoft) {
        logFail(testName, "DetectorSoft (0x800) banned but set");
        return;
    }
    if (opts & kRBOptionWindowShort) {
        logFail(testName, "WindowShort (0x100000) banned but set (v4 trial reverted)");
        return;
    }
    if (opts & kRBOptionWindowLong) {
        logFail(testName, "WindowLong (0x200000) banned but set");
        return;
    }
    logPass(testName);
}

} // namespace

// ============================================================================
// Suite aggregator
// ============================================================================

void runRubberBandStretcherSuite()
{
    logSection("RubberBandStretcher");
    runRubberBandStretcherConstructTest();
    runRubberBandStretcherKeyframeMapMatchesHandlesTest();
    runRubberBandStretcherIdentityKeyframeMapTest();
    runRubberBandStretcherFullCycleIdentityTest();
    runRubberBandStretcherProcessRejectedInIdleTest();
    // Option-bit configuration (tune-rubberband-options-for-vocal)
    runRubberBandOptions_R3EngineConfiguredTest();
    runRubberBandOptions_OfflineModeConfiguredTest();
    runRubberBandOptions_FormantPreservedTest();
    runRubberBandOptions_SmoothTransientsConfiguredTest();
    runRubberBandOptions_CompoundDetectorConfiguredTest();
    runRubberBandOptions_WindowStandardConfiguredTest();
    runRubberBandOptions_BannedOptionsAbsentTest();
}
