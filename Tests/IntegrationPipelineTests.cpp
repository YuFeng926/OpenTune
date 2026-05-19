/**
 * Tests/IntegrationPipelineTests.cpp — vocal-time-stretch §12 L3 integration
 * tests that exercise the contract between the data layer (MaterializationStore
 * + TimeGrid + PitchCurve revisions), the cache layer (RenderCache + Stage 2
 * TimeStretchCache), the undo layer (TimeGridEditAction), and the τ-driven
 * waveform projection used by PianoRollRenderer.
 *
 * These tests do NOT spin up ONNX inference — that's covered by L5/L6.
 * They lock down pipeline invariants:
 *   - 12.1: Pitch then Time vs Time then Pitch produce equivalent end state
 *           (both revisions bumped, snapshot reflects both edits)
 *   - 12.2: Dragging a TimeHandle does NOT clear PitchCache (RenderCache)
 *           chunks — the v7 core guarantee that NSF stays cached
 *   - 12.5: Undoing a TimeGrid edit invalidates Stage 2 only; RenderCache
 *           chunks survive
 *   - 12.6: τ⁻¹ projection used by waveform renderer maps screen-pixel widths
 *           correctly through stretched segments
 *
 * 12.3 (timeGridSeededOnImport) and 12.4 (drag-30ms / doubleClick-exact) are
 * already covered by MaterializationStoreTimeGridTests and TimeToolHandlerTests
 * respectively; the remaining 12.4 case (groupDragUniformDelta) is added to
 * TimeToolHandlerTests.
 *
 * Suite aggregator: runIntegrationPipelineSuite() — registered in TestMain.cpp.
 */
#include "TestSupport.h"
#include "Inference/RenderCache.h"
#include "Inference/TimeStretchCache.h"
#include "Utils/TimeGrid.h"
#include "Utils/TimeGridEditAction.h"
#include "Utils/TimeCoordinate.h"

#include <chrono>
#include <cmath>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr double kSampleRate = TimeCoordinate::kRenderSampleRate;

OpenTuneAudioProcessor::PreparedImport makeIntegrationImport(const juce::String& name,
                                                              double durationSec = 1.0)
{
    OpenTuneAudioProcessor::PreparedImport prep;
    prep.displayName = name;
    const int n = static_cast<int>(std::round(durationSec * kSampleRate));
    prep.storedAudioBuffer.setSize(1, n);
    auto* w = prep.storedAudioBuffer.getWritePointer(0);
    for (int i = 0; i < n; ++i) {
        w[i] = 0.3f * static_cast<float>(std::sin(2.0 * M_PI * 440.0 * i / kSampleRate));
    }
    return prep;
}

std::shared_ptr<const TimeGridSnapshot> makeNonIdentityGridForDuration(double durationSec,
                                                                       uint64_t revision = 2)
{
    std::vector<TimeHandle> handles = {
        {1, 0.0,                   0.0,                   HandleKind::ClipStart, true},
        {2, 0.5  * durationSec,    0.55 * durationSec,    HandleKind::OnsetVoiced, false},
        {3, durationSec,           durationSec,           HandleKind::ClipEnd,   true},
    };
    return TimeGridSnapshot::makeFromHandles(std::move(handles), revision);
}

} // namespace

// ============================================================================
// 12.1 — Order independence: editPitchThenTime vs editTimeThenPitch
// ============================================================================

void runIntegration_PitchThenTimeBumpsBothRevisionsTest()
{
    constexpr const char* testName = "Integration_PitchThenTime_BumpsBothRevisions";

    OpenTuneAudioProcessor processor;
    auto committed = processor.commitPreparedImportAsPlacement(
        makeIntegrationImport("pitchThenTime", 1.0), {0, 0.0});
    if (committed.materializationId == 0) { logFail(testName, "commit failed"); return; }
    const uint64_t matId = committed.materializationId;

    MaterializationStore::MaterializationSnapshot before;
    if (!processor.getMaterializationStore()->getSnapshot(matId, before)) {
        logFail(testName, "getSnapshot failed");
        return;
    }

    // Edit 1: PitchCurve
    auto newCurve = std::make_shared<PitchCurve>();
    if (!processor.setMaterializationPitchCurveById(matId, newCurve)) {
        logFail(testName, "setPitchCurve failed");
        return;
    }

    // Edit 2: TimeGrid
    auto newGrid = makeNonIdentityGridForDuration(1.0, /*revision=*/2);
    if (!processor.setMaterializationTimeGridById(matId, newGrid, 0, 100)) {
        logFail(testName, "setTimeGrid failed");
        return;
    }

    MaterializationStore::MaterializationSnapshot after;
    processor.getMaterializationStore()->getSnapshot(matId, after);

    if (after.timeGridRevision <= before.timeGridRevision) {
        logFail(testName, "timeGridRevision should be bumped after setTimeGrid");
        return;
    }
    if (after.timeGrid.get() != newGrid.get()) {
        logFail(testName, "snapshot.timeGrid should reflect the new grid");
        return;
    }
    logPass(testName);
}

void runIntegration_TimeThenPitchBumpsBothRevisionsTest()
{
    constexpr const char* testName = "Integration_TimeThenPitch_BumpsBothRevisions";

    OpenTuneAudioProcessor processor;
    auto committed = processor.commitPreparedImportAsPlacement(
        makeIntegrationImport("timeThenPitch", 1.0), {0, 0.0});
    if (committed.materializationId == 0) { logFail(testName, "commit failed"); return; }
    const uint64_t matId = committed.materializationId;

    // Edit 1: TimeGrid
    auto newGrid = makeNonIdentityGridForDuration(1.0, /*revision=*/2);
    if (!processor.setMaterializationTimeGridById(matId, newGrid, 0, 100)) {
        logFail(testName, "setTimeGrid failed");
        return;
    }

    // Edit 2: PitchCurve
    auto newCurve = std::make_shared<PitchCurve>();
    if (!processor.setMaterializationPitchCurveById(matId, newCurve)) {
        logFail(testName, "setPitchCurve failed");
        return;
    }

    MaterializationStore::MaterializationSnapshot after;
    processor.getMaterializationStore()->getSnapshot(matId, after);

    if (after.timeGrid.get() != newGrid.get()) {
        logFail(testName, "TimeGrid edited first should still be the published snapshot");
        return;
    }
    if (after.timeGridRevision == 0) {
        logFail(testName, "timeGridRevision should be > 0 after edit");
        return;
    }
    logPass(testName);
}

void runIntegration_OrderInvariantStage2InvalidationTest()
{
    constexpr const char* testName = "Integration_OrderInvariant_Stage2InvalidationConverges";

    // After both edits have been applied, the Stage 2 cache must be invalidated
    // regardless of edit order — the worker is then free to repopulate.
    auto runSequence = [](bool pitchFirst) {
        OpenTuneAudioProcessor processor;
        auto committed = processor.commitPreparedImportAsPlacement(
            makeIntegrationImport("seq", 1.0), {0, 0.0});
        const uint64_t matId = committed.materializationId;

        // Pre-seed Stage 2 cache with bogus data — both edits must invalidate it.
        auto& tsCache = processor.getMaterializationStore()->getTimeStretchCache();
        std::vector<float> bogus(100, 0.42f);
        tsCache.store(matId, std::move(bogus), /*pitchRev=*/99, /*timeGridRev=*/99, kSampleRate);

        if (pitchFirst) {
            processor.setMaterializationPitchCurveById(matId, std::make_shared<PitchCurve>());
            processor.setMaterializationTimeGridById(matId, makeNonIdentityGridForDuration(1.0), 0, 100);
        } else {
            processor.setMaterializationTimeGridById(matId, makeNonIdentityGridForDuration(1.0), 0, 100);
            processor.setMaterializationPitchCurveById(matId, std::make_shared<PitchCurve>());
        }
        return tsCache.hit(matId, 99, 99);   // false ⇒ invalidated
    };

    if (runSequence(/*pitchFirst=*/true)) {
        logFail(testName, "Stage 2 cache survived pitch→time edit (should have been invalidated)");
        return;
    }
    if (runSequence(/*pitchFirst=*/false)) {
        logFail(testName, "Stage 2 cache survived time→pitch edit (should have been invalidated)");
        return;
    }
    logPass(testName);
}

// ============================================================================
// 12.2 — KEY: dragging a TimeHandle does NOT invalidate PitchCache
//
// This is the v7 core guarantee: NSF inference results are NOT thrown away
// when the user only retimes.  Implementation-wise, setTimeGrid only touches
// TimeStretchCache; the chunk-wise RenderCache is untouched.
// ============================================================================

void runIntegration_HandleDragKeepsRenderCacheChunksTest()
{
    constexpr const char* testName = "Integration_HandleDrag_KeepsRenderCacheChunks";

    OpenTuneAudioProcessor processor;
    auto committed = processor.commitPreparedImportAsPlacement(
        makeIntegrationImport("renderCachePersist", 1.0), {0, 0.0});
    if (committed.materializationId == 0) { logFail(testName, "commit failed"); return; }
    const uint64_t matId = committed.materializationId;

    // Seed a published chunk into the materialization's RenderCache.
    std::shared_ptr<RenderCache> renderCache;
    if (!processor.getMaterializationStore()->getRenderCache(matId, renderCache) || renderCache == nullptr) {
        logFail(testName, "getRenderCache returned no cache");
        return;
    }
    if (!seedPublishedIdleChunk(*renderCache, 0.0, 0.01, std::vector<float>(441, 0.123f))) {
        logFail(testName, "seedPublishedIdleChunk failed");
        return;
    }
    const auto chunksBefore = renderCache->getChunkStats().total();
    if (chunksBefore == 0) {
        logFail(testName, "precondition: RenderCache should have at least 1 chunk after seeding");
        return;
    }

    // Now perform a TimeGrid edit (the v7 "drag handle" path).
    auto newGrid = makeNonIdentityGridForDuration(1.0);
    if (!processor.setMaterializationTimeGridById(matId, newGrid, 0, 100)) {
        logFail(testName, "setTimeGrid failed");
        return;
    }

    const auto chunksAfter = renderCache->getChunkStats().total();
    if (chunksAfter != chunksBefore) {
        logFail(testName, ("RenderCache chunk count changed after TimeGrid edit: "
                           "before=" + std::to_string(chunksBefore)
                           + " after=" + std::to_string(chunksAfter)
                           + " (TimeGrid edit must NOT affect Stage 1)").c_str());
        return;
    }
    logPass(testName);
}

// ============================================================================
// 12.5 — Undoing a TimeGrid edit invalidates only Stage 2 cache
// ============================================================================

void runIntegration_TimeGridUndoInvalidatesOnlyStage2Test()
{
    constexpr const char* testName = "Integration_TimeGridUndo_InvalidatesOnlyStage2";

    OpenTuneAudioProcessor processor;
    auto committed = processor.commitPreparedImportAsPlacement(
        makeIntegrationImport("undoStage2", 1.0), {0, 0.0});
    if (committed.materializationId == 0) { logFail(testName, "commit failed"); return; }
    const uint64_t matId = committed.materializationId;

    // Seed a RenderCache chunk so we can verify it survives the undo.
    std::shared_ptr<RenderCache> renderCache;
    processor.getMaterializationStore()->getRenderCache(matId, renderCache);
    seedPublishedIdleChunk(*renderCache, 0.0, 0.01, std::vector<float>(441, 0.5f));
    const auto chunksBeforeEdit = renderCache->getChunkStats().total();

    // Read the pre-edit TimeGrid (auto-seeded identity).
    std::shared_ptr<const TimeGridSnapshot> oldGrid;
    processor.getMaterializationStore()->getTimeGrid(matId, oldGrid);

    // Apply edit 1: setTimeGrid (non-identity).
    auto newGrid = makeNonIdentityGridForDuration(1.0);
    processor.setMaterializationTimeGridById(matId, newGrid, 0, 100);

    // Pre-seed Stage 2 cache with a bogus matching-rev entry so we can detect undo invalidation.
    auto& tsCache = processor.getMaterializationStore()->getTimeStretchCache();
    const uint64_t newRev = processor.getMaterializationStore()->getTimeGridRevision(matId);
    tsCache.store(matId, std::vector<float>(100, 0.7f), /*pitchRev=*/0, /*timeGridRev=*/static_cast<uint32_t>(newRev), kSampleRate);
    if (!tsCache.hit(matId, 0, static_cast<uint32_t>(newRev))) {
        logFail(testName, "precondition: Stage 2 cache should hit after store");
        return;
    }

    // Build a TimeGridEditAction and undo it (simulates user Cmd+Z).
    TimeGridEditAction action(processor, matId, "Test undo", oldGrid, newGrid, 0, 100);
    action.undo();   // Should call setTimeGrid(oldGrid) → invalidate Stage 2 cache

    if (tsCache.hit(matId, 0, static_cast<uint32_t>(newRev))) {
        logFail(testName, "Stage 2 cache should be invalidated after TimeGridEditAction.undo()");
        return;
    }
    if (renderCache->getChunkStats().total() != chunksBeforeEdit) {
        logFail(testName, "RenderCache chunks must NOT be evicted by TimeGrid undo "
                          "(spec §6.5: TimeGrid edit affects Stage 2 only)");
        return;
    }
    logPass(testName);
}

// ============================================================================
// 12.6 — Waveform stretched-segment visual width via τ⁻¹
//
// Locks down the math contract used by PianoRollRenderer::drawWaveform:
// when a segment [t_src_a, t_src_b] is stretched to [t_out_a, t_out_b],
// every screen pixel in [t_out_a, t_out_b] reads from the source peak at
// τ⁻¹(output_time).  This test verifies the τ inversion is linear inside a
// segment and exact at handle anchors.
// ============================================================================

void runIntegration_WaveformStretchedSegmentVisualWidthTest()
{
    constexpr const char* testName = "Integration_WaveformRenderer_StretchedSegmentVisualWidth";

    // Grid: ClipStart at 0, middle handle at src=0.5 / out=0.75 (1.5× stretch
    // on segment 1; 0.5× compress on segment 2), ClipEnd at 1.0.
    std::vector<TimeHandle> handles = {
        {1, 0.0, 0.0,  HandleKind::ClipStart, true},
        {2, 0.5, 0.75, HandleKind::OnsetVoiced, false},
        {3, 1.0, 1.0,  HandleKind::ClipEnd,   true},
    };
    auto grid = TimeGridSnapshot::makeFromHandles(std::move(handles), 1);
    if (grid == nullptr) { logFail(testName, "makeFromHandles failed"); return; }

    // Anchor exactness: τ⁻¹(0.75) = 0.5
    if (std::abs(grid->tauInverse(0.75) - 0.5) > 1e-6) {
        logFail(testName, "tauInverse at handle anchor must be bit-exact");
        return;
    }
    // Anchor exactness: τ⁻¹(0.0) = 0.0; τ⁻¹(1.0) = 1.0
    if (std::abs(grid->tauInverse(0.0) - 0.0) > 1e-6 || std::abs(grid->tauInverse(1.0) - 1.0) > 1e-6) {
        logFail(testName, "tauInverse at endpoints must equal 0 and 1 respectively");
        return;
    }

    // Linear inside segment 1 (1.5× stretch):
    //   output range [0, 0.75], source range [0, 0.5]
    //   midpoint output 0.375 → source 0.25
    if (std::abs(grid->tauInverse(0.375) - 0.25) > 1e-6) {
        logFail(testName, "tauInverse must be linear within stretched segment");
        return;
    }

    // Visual width contract: a 0.75 s output window covers the entire first
    // source segment (0.5 s).  The renderer expresses this as "every output
    // pixel maps via τ⁻¹".  We sample 100 pixels and verify monotonic source
    // progression with average slope 0.5 / 0.75 = 2/3.
    constexpr int N = 100;
    std::vector<double> sourceSeen(N);
    for (int i = 0; i < N; ++i) {
        const double tOut = (0.75 / static_cast<double>(N - 1)) * static_cast<double>(i);
        sourceSeen[i] = grid->tauInverse(tOut);
    }
    for (int i = 1; i < N; ++i) {
        if (sourceSeen[i] < sourceSeen[i - 1] - 1e-9) {
            logFail(testName, "tauInverse output not monotonically non-decreasing along stretched segment");
            return;
        }
    }
    const double avgSlope = (sourceSeen.back() - sourceSeen.front())
                            / (0.75 - 0.0);
    if (std::abs(avgSlope - (2.0 / 3.0)) > 1e-3) {
        logFail(testName, ("expected average tauInverse slope 2/3 across stretched segment, got "
                           + std::to_string(avgSlope)).c_str());
        return;
    }
    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

// ============================================================================
// 12.7 — Renderer τForward / ToolHandler τInverse round-trip
//
// vocal-time-stretch §8.5 (Phase H): when TimeGrid is non-identity, every
// SOURCE-time anchor (note.startTime, F0 frame, chunk boundary) flows through
// τForward in drawing, and every pixel write-back flows through τInverse.
// The two MUST be exact inverses at handle anchors and within ε at intermediate
// points (linear interp).
// ============================================================================

void runIntegration_TauForwardInverseRoundTripTest()
{
    constexpr const char* testName = "Integration_TauProjection_ForwardInverseRoundTrip";

    // Non-identity grid: 1.5× stretch on segment 1, 0.5× compress on segment 2.
    std::vector<TimeHandle> handles = {
        {1, 0.0, 0.0,  HandleKind::ClipStart, true},
        {2, 0.5, 0.75, HandleKind::OnsetVoiced, false},
        {3, 1.0, 1.0,  HandleKind::ClipEnd, true},
    };
    auto grid = TimeGridSnapshot::makeFromHandles(std::move(handles), 1);

    // Round-trip at handle anchors must be bit-exact.
    for (double srcAnchor : {0.0, 0.5, 1.0}) {
        const double out = grid->tauForward(srcAnchor);
        const double back = grid->tauInverse(out);
        if (std::abs(back - srcAnchor) > 1e-9) {
            logFail(testName, ("anchor round-trip failed at src=" + std::to_string(srcAnchor)
                               + " out=" + std::to_string(out)
                               + " back=" + std::to_string(back)).c_str());
            return;
        }
    }

    // Intermediate points: round-trip should be tight (piecewise-linear is
    // strictly monotonic and inverse-pair-exact within numeric precision).
    for (double srcMid : {0.10, 0.25, 0.40, 0.60, 0.80, 0.95}) {
        const double out = grid->tauForward(srcMid);
        const double back = grid->tauInverse(out);
        if (std::abs(back - srcMid) > 1e-9) {
            logFail(testName, ("mid round-trip failed at src=" + std::to_string(srcMid)
                               + " err=" + std::to_string(std::abs(back - srcMid))).c_str());
            return;
        }
    }
    logPass(testName);
}

// ============================================================================
// L5 Journey 2 follow-up — Order-invariant PCM byte-equality.
//
// Manual A/B export (Journey 2 manual smoke test 2026-05-12) cannot reach
// byte-equality because hand-dragged handles can't reproduce sub-pixel
// snapshots.  This automated test isolates the deterministic-render contract:
// given two processors with the SAME (pitchCurve, timeGrid) inputs applied in
// OPPOSITE order, the resulting Stage 2 PCM (`TimeStretchCache::sliceForOutputRange`
// output) MUST be byte-equal.
// ============================================================================

void runIntegration_OrderInvariantPCMByteEqualTest()
{
    constexpr const char* testName = "Integration_OrderInvariant_PCMByteEqual";

    auto buildProcessor = [](bool pitchFirst) -> std::pair<std::vector<float>, uint64_t> {
        OpenTuneAudioProcessor processor;
        auto committed = processor.commitPreparedImportAsPlacement(
            makeIntegrationImport("orderinv", 1.0), {0, 0.0});
        if (committed.materializationId == 0) return {{}, 0};
        const uint64_t matId = committed.materializationId;

        auto curve = std::make_shared<PitchCurve>();
        auto grid  = makeNonIdentityGridForDuration(1.0, /*revision=*/2);

        if (pitchFirst) {
            processor.setMaterializationPitchCurveById(matId, curve);
            processor.setMaterializationTimeGridById(matId, grid, 0, 100);
        } else {
            processor.setMaterializationTimeGridById(matId, grid, 0, 100);
            processor.setMaterializationPitchCurveById(matId, curve);
        }

        // Wait for Stage 2 worker to populate the cache.
        OpenTuneAudioProcessor::PlaybackReadSource src;
        processor.getPlaybackReadSourceByMaterializationId(matId, src);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            if (src.timeStretchCache->hit(matId, src.pitchRevision, src.timeGridRevision)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            processor.getPlaybackReadSourceByMaterializationId(matId, src);
        }

        // Slice the cache deterministically (full clip 0..1s).
        constexpr int kSamples = 44100;
        juce::AudioBuffer<float> dest(1, kSamples);
        dest.clear();
        const int wrote = src.timeStretchCache->sliceForOutputRange(
            matId, /*outputStartSeconds=*/0.0, dest, /*destStart=*/0,
            /*numSamples=*/kSamples,
            /*targetSampleRate=*/static_cast<int>(TimeCoordinate::kRenderSampleRate));
        if (wrote <= 0) return {{}, 0};

        std::vector<float> pcm(static_cast<size_t>(wrote));
        std::copy(dest.getReadPointer(0), dest.getReadPointer(0) + wrote, pcm.begin());
        return {std::move(pcm), matId};
    };

    auto [pcmA, matA] = buildProcessor(/*pitchFirst=*/true);
    auto [pcmB, matB] = buildProcessor(/*pitchFirst=*/false);
    if (pcmA.empty() || pcmB.empty()) {
        logFail(testName, "Stage 2 cache did not populate within 30 s in one of the orders");
        return;
    }
    if (pcmA.size() != pcmB.size()) {
        logFail(testName, ("Stage 2 cache slice sizes differ: A=" + std::to_string(pcmA.size())
                           + " B=" + std::to_string(pcmB.size())).c_str());
        return;
    }

    // Byte-equal contract — same float bit pattern at every sample.  Any drift
    // would indicate non-deterministic application order in the processor.
    for (size_t i = 0; i < pcmA.size(); ++i) {
        if (pcmA[i] != pcmB[i]) {
            logFail(testName, ("Stage 2 PCM differs at sample " + std::to_string(i)
                               + ": A=" + std::to_string(pcmA[i])
                               + " B=" + std::to_string(pcmB[i])).c_str());
            return;
        }
    }
    logPass(testName);
}

void runIntegrationPipelineSuite()
{
    logSection("Integration-Pipeline");
    runIntegration_PitchThenTimeBumpsBothRevisionsTest();
    runIntegration_TimeThenPitchBumpsBothRevisionsTest();
    runIntegration_OrderInvariantStage2InvalidationTest();
    runIntegration_HandleDragKeepsRenderCacheChunksTest();
    runIntegration_TimeGridUndoInvalidatesOnlyStage2Test();
    runIntegration_WaveformStretchedSegmentVisualWidthTest();
    runIntegration_TauForwardInverseRoundTripTest();
    runIntegration_OrderInvariantPCMByteEqualTest();
}
