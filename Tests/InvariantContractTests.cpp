/**
 * Tests/InvariantContractTests.cpp — vocal-time-stretch §13 L4 contract /
 * invariant tests. These lock down the **hard invariants** from DESIGN.md v7
 * §1 (Hard constraints) and design.md §Decisions:
 *
 *   13.1 TimeGrid invariants under random fuzz: locked endpoints, total
 *        duration preserved, strict monotonicity (src AND output)
 *   13.2 τ identity ⇔ all output_seconds == source_seconds (biconditional)
 *   13.3 dual-bypass bit-exactness: identity TimeGrid + identity PitchCurve
 *        ⇒ readPlaybackAudio output equals dry source samples
 *   13.4 TimeGridEditAction constructor stores affected range verbatim
 *        (anchored — already covered in TimeGridTests but re-asserted here)
 *   13.5 ARA region length invariant: τ(end_src) - τ(start_src) ≡ src duration
 *        regardless of internal handle drag (locked endpoints)
 *   13.6 RubberBandStretcher reset (via beginRebuild) re-applies KeyFrameMap
 *        — verifies that after a previous rebuild, calling beginRebuild again
 *        with a different keyframe map transitions cleanly back to Studying.
 *
 * Suite aggregator: runInvariantContractSuite() — registered in TestMain.cpp.
 */
#include "TestSupport.h"
#include "Inference/RubberBandStretcher.h"
#include "Utils/TimeGrid.h"
#include "Utils/TimeGridEditAction.h"
#include "Utils/TimeCoordinate.h"

#include <cmath>
#include <random>
#include <vector>

namespace {

constexpr double kSampleRate = TimeCoordinate::kRenderSampleRate;

// Build a random valid TimeGrid: ClipStart locked, K interior handles with
// strictly-monotonic source (jittered linearly), output drift bounded so
// strict monotonicity holds, ClipEnd locked.  Returns nullptr if the random
// configuration would violate constraints (caller retries).
std::shared_ptr<const TimeGridSnapshot> tryRandomGrid(std::mt19937& rng,
                                                       double durationSec,
                                                       int interiorHandles)
{
    std::uniform_real_distribution<double> srcJitter(0.05, 0.15);   // 50–150 ms gaps
    std::uniform_real_distribution<double> outDelta(-0.02, 0.02);    // ±20 ms drift

    std::vector<TimeHandle> handles;
    handles.push_back({1, 0.0, 0.0, HandleKind::ClipStart, true});

    double srcCursor = 0.0;
    for (int i = 0; i < interiorHandles; ++i) {
        srcCursor += srcJitter(rng);
        if (srcCursor >= durationSec - 0.05) return nullptr;
        const double outVal = juce::jlimit(handles.back().output_seconds + 0.030,
                                            durationSec - 0.030,
                                            srcCursor + outDelta(rng));
        if (outVal <= handles.back().output_seconds + 1e-6) return nullptr;
        handles.push_back({static_cast<uint64_t>(i + 2),
                            srcCursor, outVal,
                            HandleKind::OnsetVoiced, false});
    }
    handles.push_back({static_cast<uint64_t>(interiorHandles + 2),
                       durationSec, durationSec, HandleKind::ClipEnd, true});

    return TimeGridSnapshot::makeFromHandles(std::move(handles), /*revision=*/1);
}

} // namespace

// ============================================================================
// 13.1 — TimeGrid invariants under random fuzz
// ============================================================================

void runInvariant_TimeGridInvariantsHoldUnderFuzzTest()
{
    constexpr const char* testName = "Invariant_TimeGrid_InvariantsHoldUnderFuzz";

    std::mt19937 rng(0xC0FFEE);
    constexpr int kSamples = 50;

    int generated = 0;
    int attempts = 0;
    while (generated < kSamples && attempts++ < kSamples * 5) {
        auto grid = tryRandomGrid(rng, /*durationSec=*/12.0, /*interior=*/8);
        if (grid == nullptr) continue;
        ++generated;

        const auto& h = grid->handles();
        // Endpoints locked
        if (!h.front().locked || !h.back().locked) {
            logFail(testName, "endpoints not both locked");
            return;
        }
        if (h.front().kind != HandleKind::ClipStart || h.back().kind != HandleKind::ClipEnd) {
            logFail(testName, "endpoint kinds incorrect");
            return;
        }
        // Strictly monotonic source AND output
        for (size_t i = 1; i < h.size(); ++i) {
            if (h[i].source_seconds <= h[i - 1].source_seconds) {
                logFail(testName, "source not strictly monotonic"); return;
            }
            if (h[i].output_seconds <= h[i - 1].output_seconds) {
                logFail(testName, "output not strictly monotonic"); return;
            }
        }
        // Total duration preserved (endpoints locked → total duration in src ≡ in out)
        const double totalSrc = h.back().source_seconds - h.front().source_seconds;
        const double totalOut = h.back().output_seconds - h.front().output_seconds;
        if (std::abs(totalSrc - totalOut) > 1e-6) {
            logFail(testName, "total duration not preserved across endpoints");
            return;
        }
    }
    if (generated < kSamples / 2) {
        logFail(testName, "too few valid grids generated by fuzzer (configuration too tight?)");
        return;
    }
    logPass(testName);
}

// ============================================================================
// 13.2 — τ identity ⇔ all output_seconds == source_seconds (biconditional)
// ============================================================================

void runInvariant_TauIdentityIffAllOutputsEqualSourceTest()
{
    constexpr const char* testName = "Invariant_TauIdentity_IffAllOutputsEqualSource";

    // Forward direction: identity grid → isIdentity() == true
    auto idGrid = TimeGridSnapshot::makeIdentity(5.0);
    if (!idGrid->isIdentity()) {
        logFail(testName, "makeIdentity grid must report isIdentity()==true");
        return;
    }

    // Reverse direction: any handle with out != src ⇒ isIdentity()==false
    std::vector<TimeHandle> non = {
        {1, 0.0, 0.0, HandleKind::ClipStart, true},
        {2, 2.5, 2.7, HandleKind::OnsetVoiced, false},   // out != src
        {3, 5.0, 5.0, HandleKind::ClipEnd, true},
    };
    auto nonGrid = TimeGridSnapshot::makeFromHandles(std::move(non), 1);
    if (nonGrid->isIdentity()) {
        logFail(testName, "grid with displaced middle handle must NOT report isIdentity()==true");
        return;
    }

    // Edge case: 3-handle grid where middle handle happens to be at src==out
    // but other handles displaced — wait, with locked endpoints that's already
    // identity at endpoints and middle.  Construct: all handles src==out exactly.
    std::vector<TimeHandle> allEq = {
        {1, 0.0, 0.0, HandleKind::ClipStart, true},
        {2, 2.5, 2.5, HandleKind::OnsetVoiced, false},
        {3, 5.0, 5.0, HandleKind::ClipEnd, true},
    };
    auto allEqGrid = TimeGridSnapshot::makeFromHandles(std::move(allEq), 1);
    if (!allEqGrid->isIdentity()) {
        logFail(testName, "grid where every handle has src==out must be identity");
        return;
    }
    // Cross-check: tauForward(t) == t and tauInverse(t) == t for sample inputs
    for (double t : {0.0, 1.25, 2.5, 3.75, 5.0}) {
        if (std::abs(allEqGrid->tauForward(t) - t) > 1e-9) {
            logFail(testName, "tauForward should be identity"); return;
        }
        if (std::abs(allEqGrid->tauInverse(t) - t) > 1e-9) {
            logFail(testName, "tauInverse should be identity"); return;
        }
    }
    logPass(testName);
}

// ============================================================================
// 13.3 — Dual-bypass bit-exactness: identity grid + identity pitch ⇒
//        readPlaybackAudio returns dry source samples
// ============================================================================

void runInvariant_DualBypassEqualsSourcePCMTest()
{
    constexpr const char* testName = "Invariant_BypassInvariant_DualBypassEqualsSourcePCM";

    // Build a 0.2s tone with distinct samples.
    OpenTuneAudioProcessor::PreparedImport prep;
    prep.displayName = "bypass-source";
    constexpr int N = 8192;
    prep.storedAudioBuffer.setSize(1, N);
    auto* w = prep.storedAudioBuffer.getWritePointer(0);
    for (int i = 0; i < N; ++i) {
        w[i] = 0.25f * static_cast<float>(std::sin(2.0 * M_PI * 220.0 * i / kSampleRate));
    }

    OpenTuneAudioProcessor processor;
    auto committed = processor.commitPreparedImportAsPlacement(std::move(prep), {0, 0.0});
    if (committed.materializationId == 0) { logFail(testName, "commit failed"); return; }

    // No edits applied. Default state: identity TimeGrid + identity PitchCurve
    // (both auto-seeded), RenderCache empty, TimeStretchCache empty.
    OpenTuneAudioProcessor::PlaybackReadSource src;
    if (!processor.getPlaybackReadSourceByMaterializationId(committed.materializationId, src)) {
        logFail(testName, "getPlaybackReadSource failed");
        return;
    }
    if (!src.timeGridIsIdentity) {
        logFail(testName, "freshly-committed materialization must have identity TimeGrid");
        return;
    }

    // Read the full clip back at the source sample rate.
    OpenTuneAudioProcessor::PlaybackReadRequest req(src,
                                                     /*readStartSeconds=*/0.0,
                                                     kSampleRate,
                                                     /*numSamples=*/N);
    juce::AudioBuffer<float> dest(/*channels=*/1, /*samples=*/N);
    dest.clear();
    const int wrote = processor.readPlaybackAudio(req, dest, /*destStart=*/0);
    if (wrote != N) {
        logFail(testName, ("expected " + std::to_string(N) + " samples, got "
                           + std::to_string(wrote)).c_str());
        return;
    }

    // Compare each sample against the source buffer (bit-exact dry passthrough).
    std::shared_ptr<const juce::AudioBuffer<float>> sourceBuf;
    if (!processor.getMaterializationStore()->getAudioBuffer(committed.materializationId, sourceBuf)) {
        logFail(testName, "getAudioBuffer failed"); return;
    }
    const auto* sw = sourceBuf->getReadPointer(0);
    const auto* dw = dest.getReadPointer(0);
    for (int i = 0; i < N; ++i) {
        if (dw[i] != sw[i]) {
            logFail(testName, ("sample " + std::to_string(i)
                               + " differs: source=" + std::to_string(sw[i])
                               + " readback=" + std::to_string(dw[i])).c_str());
            return;
        }
    }
    logPass(testName);
}

// ============================================================================
// 13.4 — TimeGridEditAction constructor verbatim range (anchor re-assertion)
// ============================================================================

void runInvariant_TimeGridEditActionConstructorVerbatimTest()
{
    constexpr const char* testName = "Invariant_TimeGridEditActionInvariant_ConstructorVerbatimRange";

    OpenTuneAudioProcessor processor;
    auto a = TimeGridSnapshot::makeIdentity(10.0);
    auto b = TimeGridSnapshot::makeIdentity(10.0);

    // Pass a range that has NO basis in any snapshot — it must be returned verbatim.
    constexpr int64_t kStart = 7777;
    constexpr int64_t kEnd   = 9999;
    TimeGridEditAction action(processor, /*matId=*/123, "Anchor",
                              a, b, kStart, kEnd);

    if (action.getAffectedSrcStartFrame() != kStart || action.getAffectedSrcEndFrame() != kEnd) {
        logFail(testName, "constructor must store affected range verbatim, not derive from snapshot");
        return;
    }
    logPass(testName);
}

// ============================================================================
// 13.5 — ARA region length invariant
// ============================================================================

void runInvariant_AraRegionLengthEqualsSourceDurationTest()
{
    constexpr const char* testName = "Invariant_AraRegionLength_OutputDurationEqualsSourceDuration";

    // Hand-craft non-identity grid with locked endpoints and asymmetric internal stretch.
    constexpr double srcEnd = 12.0;
    std::vector<TimeHandle> handles = {
        {1, 0.0,  0.0,  HandleKind::ClipStart, true},
        {2, 3.0,  4.5,  HandleKind::OnsetVoiced, false},     // 1.5× stretch
        {3, 9.0,  7.5,  HandleKind::OnsetSibilant, false},   // compressed
        {4, srcEnd, srcEnd, HandleKind::ClipEnd, true},
    };
    auto grid = TimeGridSnapshot::makeFromHandles(std::move(handles), 1);
    if (!grid) { logFail(testName, "makeFromHandles failed"); return; }

    const double srcDur = grid->handles().back().source_seconds - grid->handles().front().source_seconds;
    const double outDur = grid->handles().back().output_seconds - grid->handles().front().output_seconds;
    if (std::abs(srcDur - outDur) > 1e-6) {
        logFail(testName, "src/out duration mismatch — endpoints lock failed");
        return;
    }

    // τ projection of [0, srcEnd] must span [0, srcEnd] regardless of internal handle drift.
    const double tauStart = grid->tauForward(0.0);
    const double tauEnd   = grid->tauForward(srcEnd);
    if (std::abs(tauStart - 0.0) > 1e-9 || std::abs(tauEnd - srcEnd) > 1e-9) {
        logFail(testName, ("tauForward(srcEnd) should equal srcEnd; got "
                           + std::to_string(tauEnd)).c_str());
        return;
    }
    if (std::abs((tauEnd - tauStart) - srcDur) > 1e-9) {
        logFail(testName, "ARA-style region length (tauEnd - tauStart) must equal source duration");
        return;
    }
    logPass(testName);
}

// ============================================================================
// 13.6 — RubberBandStretcher reset (via beginRebuild) re-applies KeyFrameMap
// ============================================================================

void runInvariant_RubberBandResetReappliesKeyFrameMapTest()
{
    constexpr const char* testName = "Invariant_RubberBandStretcher_ResetReappliesKeyFrameMap";

    RubberBandStretcher rb(kSampleRate, /*channels=*/1);

    auto firstSnap = TimeGridSnapshot::makeIdentity(1.0);
    auto firstKF = rb.buildKeyframesFromTimeGrid(*firstSnap);
    rb.beginRebuild(/*timeRatio=*/1.0, firstKF);
    if (rb.phase() != RubberBandStretcher::Phase::Studying) {
        logFail(testName, "after first beginRebuild, expect Studying phase");
        return;
    }

    // Run a partial study cycle.
    std::vector<float> tone(static_cast<size_t>(kSampleRate / 4), 0.1f);
    rb.study(tone.data(), tone.size(), /*isLast=*/true);
    if (rb.phase() != RubberBandStretcher::Phase::Processing) {
        logFail(testName, "post-study should be in Processing phase");
        return;
    }

    // Now build a different keyframe map (non-identity 1.5× stretch on segment 1).
    std::vector<TimeHandle> stretched = {
        {1, 0.0, 0.0, HandleKind::ClipStart, true},
        {2, 0.5, 0.75, HandleKind::OnsetVoiced, false},
        {3, 1.0, 1.0, HandleKind::ClipEnd, true},
    };
    auto secondSnap = TimeGridSnapshot::makeFromHandles(std::move(stretched), 2);
    auto secondKF = rb.buildKeyframesFromTimeGrid(*secondSnap);
    if (secondKF == firstKF) {
        logFail(testName, "test setup error: keyframe maps should differ");
        return;
    }

    // Reset and reapply via beginRebuild — phase should drop back to Studying.
    rb.beginRebuild(/*timeRatio=*/1.0, secondKF);
    if (rb.phase() != RubberBandStretcher::Phase::Studying) {
        logFail(testName, "beginRebuild should reset stretcher to Studying phase, "
                          "regardless of prior Processing phase");
        return;
    }

    // Studying with an empty study() cycle should still allow process()/retrieve()
    // to consume the new map.
    rb.study(tone.data(), tone.size(), /*isLast=*/true);
    rb.process(tone.data(), tone.size(), /*isLast=*/true);
    if (rb.available() == 0) {
        logFail(testName, "after second rebuild + study + process, RB should produce output");
        return;
    }
    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runInvariantContractSuite()
{
    logSection("Invariant-Contract");
    runInvariant_TimeGridInvariantsHoldUnderFuzzTest();
    runInvariant_TauIdentityIffAllOutputsEqualSourceTest();
    runInvariant_DualBypassEqualsSourcePCMTest();
    runInvariant_TimeGridEditActionConstructorVerbatimTest();
    runInvariant_AraRegionLengthEqualsSourceDurationTest();
    runInvariant_RubberBandResetReappliesKeyFrameMapTest();
}
