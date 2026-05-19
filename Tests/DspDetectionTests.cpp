/**
 * Tests/DspDetectionTests.cpp — Unit tests for §4 detection pipeline:
 *   - OnsetDetector (Bello 2005 complex-domain ODF)
 *   - PhonemeClassifier (DSP three-class)
 *   - WordSegmenter (Tier 1 + Tier 2 NMS)
 *
 * Spec coverage:
 *   - openspec/changes/vocal-time-stretch/specs/vocal-onset-detector/spec.md
 *   - openspec/changes/vocal-time-stretch/specs/phoneme-classifier/spec.md
 *   - openspec/changes/vocal-time-stretch/specs/word-segmenter/spec.md
 *
 * Suite aggregator: runDspDetectionSuite() — registered in TestMain.cpp.
 */
#include "TestSupport.h"
#include "DSP/OnsetDetector.h"
#include "DSP/PhonemeClassifier.h"
#include "DSP/WordSegmenter.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kSr = 44100;

// ─── Audio synthesis helpers ────────────────────────────────────────────

std::vector<float> makeSineTone(double freqHz, double durationSec, double amp = 0.5)
{
    const int n = static_cast<int>(std::round(durationSec * kSr));
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        out[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * M_PI * freqHz * i / kSr));
    }
    return out;
}

std::vector<float> makeSilence(double durationSec)
{
    const int n = static_cast<int>(std::round(durationSec * kSr));
    return std::vector<float>(static_cast<size_t>(n), 0.0f);
}

std::vector<float> makeClickTrain(int numClicks, double clickIntervalSec, double clickWidthSec = 0.01,
                                   double totalDurationSec = -1.0)
{
    if (totalDurationSec < 0) {
        totalDurationSec = clickIntervalSec * (numClicks + 1);
    }
    auto out = makeSilence(totalDurationSec);
    const int clickWidthSamples = static_cast<int>(clickWidthSec * kSr);

    for (int c = 0; c < numClicks; ++c) {
        const double clickTimeSec = (c + 1) * clickIntervalSec;
        const int clickStartSample = static_cast<int>(clickTimeSec * kSr);
        for (int i = 0; i < clickWidthSamples && clickStartSample + i < (int) out.size(); ++i) {
            // Half-cosine envelope click (broadband transient)
            const float env = 0.9f * static_cast<float>(std::sin(M_PI * static_cast<double>(i) / clickWidthSamples));
            out[static_cast<size_t>(clickStartSample + i)] = env;
        }
    }
    return out;
}

} // namespace

// ============================================================================
// OnsetDetector tests
// ============================================================================

void runOnsetDetectorSustainedToneNoOnsetsTest()
{
    constexpr const char* testName = "OnsetDetector_SustainedToneNoOnsets";

    OnsetDetector det;
    OnsetDetectorConfig cfg;
    cfg.sampleRate = kSr;
    cfg.nFft = 2048;
    cfg.hopLength = 441;
    det.configure(cfg);

    auto audio = makeSineTone(220.0, 5.0, 0.5);  // 5 sec sustained 220 Hz
    auto result = det.detect(audio.data(), audio.size());

    if (result.onsetFrames100fps.size() > 1) {
        // Allow at most 1 onset (boundary initialization artifact)
        logFail(testName, ("expected ≤ 1 onsets on sustained tone, got "
                           + std::to_string(result.onsetFrames100fps.size())).c_str());
        return;
    }

    logPass(testName);
}

void runOnsetDetectorClickProducesPeakTest()
{
    constexpr const char* testName = "OnsetDetector_ClickProducesODFPeak";

    OnsetDetector det;
    OnsetDetectorConfig cfg;
    cfg.sampleRate = kSr;
    cfg.nFft = 2048;
    cfg.hopLength = 441;
    cfg.alpha = 1.5f;
    cfg.minDistanceFrames = 3;
    det.configure(cfg);

    // 5 clicks at 1-second intervals (5 sec total).  Each click is a broadband
    // transient that should produce a clear ODF peak.
    auto audio = makeClickTrain(5, 1.0, 0.005, 6.0);
    auto result = det.detect(audio.data(), audio.size());

    // Expect at least 4 onsets detected (allow 1 to slip due to boundary)
    if (result.onsetFrames100fps.size() < 4) {
        logFail(testName, ("expected ≥ 4 onsets on click train, got "
                           + std::to_string(result.onsetFrames100fps.size())).c_str());
        return;
    }

    // Sanity: ODF should have non-zero values
    bool hasNonZeroOdf = false;
    for (float v : result.odfRaw) {
        if (v > 1.0f) { hasNonZeroOdf = true; break; }
    }
    if (!hasNonZeroOdf) {
        logFail(testName, "ODF should have values >1.0 on click input");
        return;
    }

    logPass(testName);
}

void runOnsetDetectorMinDistanceFiltersTest()
{
    constexpr const char* testName = "OnsetDetector_MinDistanceFiltersDenseOnsets";

    OnsetDetector det;
    OnsetDetectorConfig cfg;
    cfg.sampleRate = kSr;
    cfg.nFft = 2048;
    cfg.hopLength = 441;
    cfg.minDistanceFrames = 3;  // 30 ms minimum
    det.configure(cfg);

    // Two clicks 20 ms apart (less than min distance) — only one should survive
    auto audio = makeSilence(0.5);
    const int click1 = static_cast<int>(0.10 * kSr);
    const int click2 = click1 + static_cast<int>(0.020 * kSr);  // 20 ms later
    const int wid = static_cast<int>(0.005 * kSr);
    for (int i = 0; i < wid; ++i) {
        audio[static_cast<size_t>(click1 + i)] = 0.9f * std::sin(M_PI * i / wid);
        audio[static_cast<size_t>(click2 + i)] = 0.9f * std::sin(M_PI * i / wid);
    }

    auto result = det.detect(audio.data(), audio.size());

    if (result.onsetFrames100fps.size() > 1) {
        logFail(testName, ("two close clicks should yield ≤ 1 onset, got "
                           + std::to_string(result.onsetFrames100fps.size())).c_str());
        return;
    }
    logPass(testName);
}

// ============================================================================
// PhonemeClassifier tests
// ============================================================================

void runPhonemeClassifierVoicedFusionTest()
{
    constexpr const char* testName = "PhonemeClassifier_VoicedFusion";

    PhonemeClassifier pc;
    PhonemeClassifierConfig cfg;
    cfg.sampleRate = kSr;
    cfg.frameRateHz = 100;
    pc.configure(cfg);

    // Synth audio: 1 second of a 220 Hz sustained tone (mimics voiced)
    auto audio = makeSineTone(220.0, 1.0, 0.4);

    // Mock RMVPE uv = ~0.05 (voiced; uv is unvoiced probability so low = voiced)
    std::vector<float> uvProb(100, 0.05f);
    // Mock Silero VAD = ~0.9 (high speech probability)
    std::vector<float> vadProb(100, 0.9f);

    auto result = pc.classify(audio.data(), audio.size(),
                               uvProb.data(), uvProb.size(),
                               vadProb.data(), vadProb.size());

    if (result.classes.empty()) {
        logFail(testName, "classification result is empty");
        return;
    }

    // Voiced score = 0.6 * (1 - 0.05) + 0.4 * 0.9 = 0.57 + 0.36 = 0.93 > 0.55
    // Expect ≥ 90% Voiced classification
    int voicedCount = 0;
    for (auto c : result.classes) {
        if (c == PhonemeClass::Voiced) ++voicedCount;
    }
    const float voicedPct = static_cast<float>(voicedCount) / static_cast<float>(result.classes.size());
    if (voicedPct < 0.9f) {
        logFail(testName, ("expected ≥ 90% Voiced, got " + std::to_string(voicedPct * 100.0f) + "%").c_str());
        return;
    }

    logPass(testName);
}

void runPhonemeClassifierUvSemanticInversionPreventedTest()
{
    constexpr const char* testName = "PhonemeClassifier_UvSemanticInversionPrevented";

    // ⭐ CRITICAL anchor test: ensures (1 - uv_prob) is used as voicing component.
    // If anyone refactors to use uv_prob directly (semantic inversion), this
    // test catches it.

    PhonemeClassifier pc;
    PhonemeClassifierConfig cfg;
    cfg.sampleRate = kSr;
    cfg.frameRateHz = 100;
    pc.configure(cfg);

    auto audio = makeSineTone(220.0, 1.0, 0.4);

    // Mock RMVPE uv = 0.95 (unvoiced; uv high = unvoiced)
    std::vector<float> uvProb(100, 0.95f);
    // Mock Silero VAD = 0.05 (low speech probability)
    std::vector<float> vadProb(100, 0.05f);

    auto result = pc.classify(audio.data(), audio.size(),
                               uvProb.data(), uvProb.size(),
                               vadProb.data(), vadProb.size());

    if (result.classes.empty()) {
        logFail(testName, "classification result is empty");
        return;
    }

    // V/U fusion: 0.6 * (1 - 0.95) + 0.4 * 0.05 = 0.03 + 0.02 = 0.05 < 0.55
    // → unvoiced → either Sibilant or Silence based on RMS/HFR/SC/ZCR.
    // Expect 0% Voiced classification.
    int voicedCount = 0;
    for (auto c : result.classes) {
        if (c == PhonemeClass::Voiced) ++voicedCount;
    }
    if (voicedCount > 0) {
        logFail(testName, ("uv=0.95 (unvoiced) frames should NOT be classified as Voiced, but got "
                           + std::to_string(voicedCount) + " Voiced frames "
                           + "— this means UV semantic inversion bug has reappeared!").c_str());
        return;
    }

    logPass(testName);
}

void runPhonemeClassifierSilenceDetectionTest()
{
    constexpr const char* testName = "PhonemeClassifier_SilenceDetection";

    PhonemeClassifier pc;
    PhonemeClassifierConfig cfg;
    cfg.sampleRate = kSr;
    cfg.frameRateHz = 100;
    pc.configure(cfg);

    // Pure silence audio, low uv (≈0 since RMVPE probably output uv=1 for silence,
    // but in this test we control independently)
    auto audio = makeSilence(1.0);
    std::vector<float> uvProb(100, 0.95f);    // unvoiced
    std::vector<float> vadProb(100, 0.05f);   // no speech

    auto result = pc.classify(audio.data(), audio.size(),
                               uvProb.data(), uvProb.size(),
                               vadProb.data(), vadProb.size());

    // Silence audio → RMS very low → should be classified as Silence (or only Silence)
    int silenceCount = 0;
    for (auto c : result.classes) {
        if (c == PhonemeClass::Silence) ++silenceCount;
    }
    if (silenceCount < (int) (result.classes.size() * 0.95f)) {
        logFail(testName, ("expected ≥ 95% Silence, got " +
                           std::to_string(100.0f * silenceCount / result.classes.size()) + "%").c_str());
        return;
    }

    logPass(testName);
}

// ============================================================================
// WordSegmenter tests
// ============================================================================

void runWordSegmenterTier1ClassTransitionsTest()
{
    constexpr const char* testName = "WordSegmenter_Tier1ClassTransitions";

    // Build a phoneme-class sequence with 3 transitions:
    //   [Voiced × 100, Sibilant × 50, Voiced × 100, Silence × 50, Voiced × 100]
    std::vector<PhonemeClass> classes;
    classes.insert(classes.end(), 100, PhonemeClass::Voiced);
    classes.insert(classes.end(), 50, PhonemeClass::Sibilant);   // V→Sib transition at 100
    classes.insert(classes.end(), 100, PhonemeClass::Voiced);   // Sib→V at 150
    classes.insert(classes.end(), 50, PhonemeClass::Silence);   // V→Sil at 250
    classes.insert(classes.end(), 100, PhonemeClass::Voiced);   // Sil→V at 300

    WordSegmenter ws;
    WordSegmenterConfig cfg;
    cfg.frameRateHz = 100;
    cfg.nmsRadiusFrames = 5;
    cfg.minHandleGapMs = 30;
    ws.configure(cfg);

    auto result = ws.segment(classes, /*onsetFrames100fps=*/{});

    // Expect exactly 4 Tier-1 handles (4 transitions)
    if (result.seedHandleFrames.size() != 4) {
        logFail(testName, ("expected 4 Tier-1 handles, got "
                           + std::to_string(result.seedHandleFrames.size())).c_str());
        return;
    }

    // Verify kinds:
    //   transition to Sibilant → OnsetSibilant
    //   transition to Voiced → OnsetVoiced (twice)
    //   transition to Silence → OnsetSilence
    if (result.seedHandleKinds[0] != HandleKind::OnsetSibilant) {
        logFail(testName, "first transition to Sibilant should yield OnsetSibilant kind");
        return;
    }
    if (result.seedHandleKinds[1] != HandleKind::OnsetVoiced) {
        logFail(testName, "Sib→Voiced transition should yield OnsetVoiced kind");
        return;
    }
    if (result.seedHandleKinds[2] != HandleKind::OnsetSilence) {
        logFail(testName, "V→Silence transition should yield OnsetSilence kind");
        return;
    }
    if (result.seedHandleKinds[3] != HandleKind::OnsetVoiced) {
        logFail(testName, "Sil→Voiced transition should yield OnsetVoiced kind");
        return;
    }

    logPass(testName);
}

void runWordSegmenterTier2InternalOnsetsTest()
{
    constexpr const char* testName = "WordSegmenter_Tier2InternalOnsetsInVoicedSustain";

    // 200 frames of all-Voiced (no class transitions = no Tier 1)
    std::vector<PhonemeClass> classes(200, PhonemeClass::Voiced);

    // Tier-2 onset at frame 100 (interior, far from any boundary)
    std::vector<int> onsetFrames = {100};

    WordSegmenter ws;
    WordSegmenterConfig cfg;
    cfg.frameRateHz = 100;
    cfg.nmsRadiusFrames = 5;
    ws.configure(cfg);

    auto result = ws.segment(classes, onsetFrames);

    if (result.seedHandleFrames.size() != 1) {
        logFail(testName, ("expected 1 InternalOnset handle, got "
                           + std::to_string(result.seedHandleFrames.size())).c_str());
        return;
    }
    if (result.seedHandleKinds[0] != HandleKind::InternalOnset) {
        logFail(testName, "Tier-2 onset should yield InternalOnset kind");
        return;
    }
    if (result.seedHandleFrames[0] != 100) {
        logFail(testName, ("expected onset at frame 100, got "
                           + std::to_string(result.seedHandleFrames[0])).c_str());
        return;
    }

    logPass(testName);
}

void runWordSegmenterTier1WinsOverTier2Test()
{
    constexpr const char* testName = "WordSegmenter_Tier1WinsOverTier2";

    // [Voiced × 100, Sibilant × 100] with Tier-1 transition at frame 100.
    // Tier-2 onset placed at frame 102 (within ±5 NMS) — should be suppressed.
    std::vector<PhonemeClass> classes;
    classes.insert(classes.end(), 100, PhonemeClass::Voiced);
    classes.insert(classes.end(), 100, PhonemeClass::Sibilant);

    std::vector<int> onsetFrames = {102};

    WordSegmenter ws;
    WordSegmenterConfig cfg;
    cfg.frameRateHz = 100;
    cfg.nmsRadiusFrames = 5;
    ws.configure(cfg);

    auto result = ws.segment(classes, onsetFrames);

    // Tier-1 at 100 should win; Tier-2 at 102 suppressed
    if (result.seedHandleFrames.size() != 1) {
        logFail(testName, ("expected 1 handle (Tier-1 wins), got "
                           + std::to_string(result.seedHandleFrames.size())).c_str());
        return;
    }
    if (result.seedHandleFrames[0] != 100 || result.seedHandleKinds[0] != HandleKind::OnsetSibilant) {
        logFail(testName, "Tier-1 OnsetSibilant at frame 100 should be the kept handle");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runDspDetectionSuite()
{
    logSection("DSP-Detection");
    runOnsetDetectorSustainedToneNoOnsetsTest();
    runOnsetDetectorClickProducesPeakTest();
    runOnsetDetectorMinDistanceFiltersTest();
    runPhonemeClassifierVoicedFusionTest();
    runPhonemeClassifierUvSemanticInversionPreventedTest();
    runPhonemeClassifierSilenceDetectionTest();
    runWordSegmenterTier1ClassTransitionsTest();
    runWordSegmenterTier2InternalOnsetsTest();
    runWordSegmenterTier1WinsOverTier2Test();
}
