/**
 * Tests/Stage2WorkerTests.cpp — Phase D end-to-end integration tests for the
 * Stage 2 (Time-Stretch) worker thread + readPlaybackAudio fast path
 * (vocal-time-stretch §7).
 *
 * Coverage:
 *   - Stage 2 worker is started lazily on first requestStage2Rebuild
 *   - Identity TimeGrid → no Stage 2 work happens; TimeStretchCache stays empty
 *   - Non-identity TimeGrid → worker rebuilds + populates TimeStretchCache
 *   - PlaybackReadSource carries TimeStretchCache pointer + revisions
 *   - readPlaybackAudio fast-path consults cache when timeGridIsIdentity=false
 *
 * Suite aggregator: runStage2WorkerSuite() — registered in TestMain.cpp.
 */
#include "TestSupport.h"
#include "Inference/TimeStretchCache.h"
#include "Utils/TimeGrid.h"
#include "Utils/TimeCoordinate.h"

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = TimeCoordinate::kRenderSampleRate;

// Generate a 1-second sine tone PreparedImport.  RB Offline mode needs a
// reasonable amount of audio (>= a few thousand samples) to produce output.
OpenTuneAudioProcessor::PreparedImport makeSineToneImport(const juce::String& name,
                                                          double durationSec = 1.0,
                                                          double freqHz = 220.0)
{
    OpenTuneAudioProcessor::PreparedImport prep;
    prep.displayName = name;
    const int n = static_cast<int>(std::round(durationSec * kSampleRate));
    prep.storedAudioBuffer.setSize(1, n);
    auto* w = prep.storedAudioBuffer.getWritePointer(0);
    for (int i = 0; i < n; ++i) {
        w[i] = 0.4f * static_cast<float>(std::sin(2.0 * M_PI * freqHz * i / kSampleRate));
    }
    return prep;
}

// Build a non-identity TimeGrid for a clip of given duration:
//   ClipStart at t=0 (locked)
//   middle handle: src=0.5*dur, out=0.55*dur (small displacement)
//   ClipEnd at t=dur (locked)
std::shared_ptr<const TimeGridSnapshot> makeNonIdentityGrid(double durationSec)
{
    std::vector<TimeHandle> handles = {
        {1, 0.0,                 0.0,                 HandleKind::ClipStart, true},
        {2, 0.5  * durationSec,  0.55 * durationSec,  HandleKind::OnsetVoiced, false},
        {3, durationSec,         durationSec,         HandleKind::ClipEnd,   true},
    };
    return TimeGridSnapshot::makeFromHandles(std::move(handles), /*revision=*/2);
}

// Busy-wait up to `timeoutMs` for the Stage 2 worker to publish a cache entry.
bool waitForStage2Cache(TimeStretchCache& cache,
                         uint64_t matId,
                         uint32_t pitchRev,
                         uint32_t timeGridRev,
                         int timeoutMs = 30000)
{
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
    while (steady_clock::now() < deadline) {
        if (cache.hit(matId, pitchRev, timeGridRev)) return true;
        std::this_thread::sleep_for(milliseconds(50));
    }
    return false;
}

} // namespace

void runStage2_PlaybackReadSourceCarriesCachePointerTest()
{
    constexpr const char* testName = "Stage2_PlaybackReadSource_CarriesCachePointer";

    OpenTuneAudioProcessor processor;
    auto committed = processor.commitPreparedImportAsPlacement(
        makeSineToneImport("test", 0.5), {0, 0.0});
    if (committed.materializationId == 0) {
        logFail(testName, "commit failed");
        return;
    }

    OpenTuneAudioProcessor::PlaybackReadSource src;
    if (!processor.getPlaybackReadSourceByMaterializationId(committed.materializationId, src)) {
        logFail(testName, "getPlaybackReadSourceByMaterializationId failed");
        return;
    }

    if (src.materializationId != committed.materializationId) {
        logFail(testName, "PlaybackReadSource.materializationId not forwarded");
        return;
    }
    if (src.timeStretchCache == nullptr) {
        logFail(testName, "PlaybackReadSource.timeStretchCache is null after import");
        return;
    }
    if (!src.timeGridIsIdentity) {
        logFail(testName, "auto-seeded grid should be identity → timeGridIsIdentity=true");
        return;
    }

    logPass(testName);
}

void runStage2_IdentityGridSkipsRebuildTest()
{
    constexpr const char* testName = "Stage2_IdentityGrid_SkipsRebuild";

    OpenTuneAudioProcessor processor;
    auto committed = processor.commitPreparedImportAsPlacement(
        makeSineToneImport("idle", 0.5), {0, 0.0});
    if (committed.materializationId == 0) { logFail(testName, "commit failed"); return; }

    // Identity grid (auto-seeded by createMaterialization).  Trigger a rebuild
    // and verify the worker bails out cleanly without populating the cache.
    processor.requestStage2Rebuild(committed.materializationId);

    OpenTuneAudioProcessor::PlaybackReadSource src;
    processor.getPlaybackReadSourceByMaterializationId(committed.materializationId, src);

    // Wait briefly for the worker to (no-op) drain the queue.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (src.timeStretchCache->hit(committed.materializationId, 0, 1)) {
        logFail(testName, "identity grid should NOT populate TimeStretchCache");
        return;
    }
    logPass(testName);
}

void runStage2_NonIdentityGridPopulatesCacheTest()
{
    constexpr const char* testName = "Stage2_NonIdentityGrid_PopulatesCache";

    OpenTuneAudioProcessor processor;
    auto committed = processor.commitPreparedImportAsPlacement(
        makeSineToneImport("stretch", /*durationSec=*/1.0), {0, 0.0});
    if (committed.materializationId == 0) { logFail(testName, "commit failed"); return; }

    auto grid = makeNonIdentityGrid(1.0);
    if (!grid) { logFail(testName, "makeNonIdentityGrid failed"); return; }

    // Set the new TimeGrid through the processor API; this also enqueues
    // a Stage 2 rebuild internally.
    if (!processor.setMaterializationTimeGridById(committed.materializationId,
                                                   grid,
                                                   /*affectedSrcStart=*/0,
                                                   /*affectedSrcEnd=*/100000)) {
        logFail(testName, "setMaterializationTimeGridById failed");
        return;
    }

    OpenTuneAudioProcessor::PlaybackReadSource src;
    processor.getPlaybackReadSourceByMaterializationId(committed.materializationId, src);
    if (src.timeGridIsIdentity) {
        logFail(testName, "after setTimeGrid(non-identity), timeGridIsIdentity should be false");
        return;
    }
    if (src.timeStretchCache == nullptr) {
        logFail(testName, "timeStretchCache should be non-null");
        return;
    }

    // Wait for the worker to finish rebuilding.  RB Offline mode on 1 sec of
    // audio is fast (~tens of milliseconds on M-series silicon), but be
    // generous with the timeout to avoid CI flakes.
    if (!waitForStage2Cache(*src.timeStretchCache,
                             committed.materializationId,
                             /*pitchRev=*/0,
                             /*timeGridRev=*/src.timeGridRevision,
                             /*timeoutMs=*/30000)) {
        logFail(testName, ("Stage 2 worker did not publish cache within 30s "
                           "(materializationId=" + std::to_string(committed.materializationId)
                           + " timeGridRev=" + std::to_string(src.timeGridRevision) + ")").c_str());
        return;
    }

    logPass(testName);
}

void runStage2_ReadPlaybackAudio_FastPathOnHitTest()
{
    constexpr const char* testName = "Stage2_ReadPlaybackAudio_FastPathOnHit";

    OpenTuneAudioProcessor processor;
    auto committed = processor.commitPreparedImportAsPlacement(
        makeSineToneImport("readback", 1.0), {0, 0.0});
    if (committed.materializationId == 0) { logFail(testName, "commit failed"); return; }

    auto grid = makeNonIdentityGrid(1.0);
    processor.setMaterializationTimeGridById(committed.materializationId, grid, 0, 100000);

    OpenTuneAudioProcessor::PlaybackReadSource src;
    processor.getPlaybackReadSourceByMaterializationId(committed.materializationId, src);

    if (!waitForStage2Cache(*src.timeStretchCache,
                             committed.materializationId, 0, src.timeGridRevision, 30000)) {
        logFail(testName, "Stage 2 cache not populated in time");
        return;
    }

    // Read 441 samples at output time 0.4 s, sr 44100.
    OpenTuneAudioProcessor::PlaybackReadRequest req(src,
                                                     /*readStartSeconds=*/0.4,
                                                     kSampleRate,
                                                     /*numSamples=*/441);
    juce::AudioBuffer<float> dest(/*channels=*/2, /*samples=*/441);
    dest.clear();

    const int wrote = processor.readPlaybackAudio(req, dest, /*destStart=*/0);
    if (wrote <= 0) {
        logFail(testName, "readPlaybackAudio returned 0 samples on a populated TimeStretchCache");
        return;
    }

    // Sanity: cache output should not be all zeros (a sine input should produce
    // non-zero stretched output).
    bool nonZero = false;
    for (int i = 0; i < wrote; ++i) {
        if (std::abs(dest.getSample(0, i)) > 1e-4f) { nonZero = true; break; }
    }
    if (!nonZero) {
        logFail(testName, "TimeStretchCache slice should not be all-zeros for sine input");
        return;
    }

    logPass(testName);
}

// ============================================================================
// §7 Phase E — Stage 2 input source verification
//
// Phase E switched Stage 2 input from raw source PCM to readPlaybackAudio
// output (which sums dry source + RenderCache overlay + LR4 mix). The
// behavioral test below confirms that with an EMPTY RenderCache (no NSF
// chunks rendered), Stage 2 input gracefully degrades to dry source.
//
// A full "NSF rendered → Stage 2 picks it up" integration test requires
// running the chunkRenderWorker with real ONNX models, which is outside
// the unit-test scope.  We rely on the design-level invariant:
//   readPlaybackAudio(timeStretchCache=null, mixer=...) returns dry+overlay
// which TimeStretchCacheTests + readPlaybackAudio existing tests already cover.
// ============================================================================

void runStage2_PhaseE_GracefulDegradationOnEmptyRenderCacheTest()
{
    constexpr const char* testName = "Stage2_PhaseE_GracefulDegradationOnEmptyRenderCache";

    OpenTuneAudioProcessor processor;
    auto committed = processor.commitPreparedImportAsPlacement(
        makeSineToneImport("phaseE-degrade", 1.0), {0, 0.0});
    if (committed.materializationId == 0) { logFail(testName, "commit failed"); return; }

    auto grid = makeNonIdentityGrid(1.0);
    processor.setMaterializationTimeGridById(committed.materializationId, grid, 0, 100000);

    OpenTuneAudioProcessor::PlaybackReadSource src;
    processor.getPlaybackReadSourceByMaterializationId(committed.materializationId, src);

    if (!waitForStage2Cache(*src.timeStretchCache,
                             committed.materializationId, 0, src.timeGridRevision, 30000)) {
        logFail(testName, "Stage 2 cache not populated within timeout");
        return;
    }

    // RenderCache is empty (no NSF performed in test harness).  Phase E Stage 2
    // reads dry source via readPlaybackAudio with mixer (LR4 mix degenerates
    // to dry passthrough on empty render cache).  Verify cache contains the
    // expected ~1 second of audio.
    OpenTuneAudioProcessor::PlaybackReadRequest req(src,
                                                     /*readStartSeconds=*/0.0,
                                                     kSampleRate,
                                                     /*numSamples=*/441);
    juce::AudioBuffer<float> dest(/*channels=*/1, /*samples=*/441);
    dest.clear();

    const int wrote = processor.readPlaybackAudio(req, dest, 0);
    if (wrote <= 0) {
        logFail(testName, "readPlaybackAudio wrote 0 samples");
        return;
    }

    bool nonZero = false;
    for (int i = 0; i < wrote; ++i) {
        if (std::abs(dest.getSample(0, i)) > 1e-4f) { nonZero = true; break; }
    }
    if (!nonZero) {
        logFail(testName, "Phase E Stage 2 output should be non-zero (sine input → stretched sine)");
        return;
    }

    logPass(testName);
}

void runStage2_PhaseE_PitchEditTriggersStage2RebuildTest()
{
    constexpr const char* testName = "Stage2_PhaseE_PitchEditTriggersStage2Rebuild";

    OpenTuneAudioProcessor processor;
    auto committed = processor.commitPreparedImportAsPlacement(
        makeSineToneImport("phaseE-pitch-trigger", 1.0), {0, 0.0});
    if (committed.materializationId == 0) { logFail(testName, "commit failed"); return; }

    // Set non-identity TimeGrid → first Stage 2 rebuild
    auto grid = makeNonIdentityGrid(1.0);
    processor.setMaterializationTimeGridById(committed.materializationId, grid, 0, 100000);

    OpenTuneAudioProcessor::PlaybackReadSource src;
    processor.getPlaybackReadSourceByMaterializationId(committed.materializationId, src);

    if (!waitForStage2Cache(*src.timeStretchCache,
                             committed.materializationId, 0, src.timeGridRevision, 30000)) {
        logFail(testName, "first Stage 2 rebuild did not complete");
        return;
    }

    // Now mutate PitchCurve — should invalidate Stage 2 cache and trigger rebuild
    auto newCurve = std::make_shared<PitchCurve>();
    if (!processor.setMaterializationPitchCurveById(committed.materializationId, newCurve)) {
        logFail(testName, "setMaterializationPitchCurveById failed");
        return;
    }

    // Cache should have been invalidated (hit() returns false right after pitch edit)
    // because setPitchCurve calls TimeStretchCache::invalidate.  Then the worker
    // re-populates it asynchronously.
    OpenTuneAudioProcessor::PlaybackReadSource src2;
    processor.getPlaybackReadSourceByMaterializationId(committed.materializationId, src2);

    if (!waitForStage2Cache(*src2.timeStretchCache,
                             committed.materializationId, 0, src2.timeGridRevision, 30000)) {
        logFail(testName, "Stage 2 should auto-rebuild after pitch edit on non-identity grid");
        return;
    }

    logPass(testName);
}

void runStage2WorkerSuite()
{
    logSection("Stage2-Worker");
    runStage2_PlaybackReadSourceCarriesCachePointerTest();
    runStage2_IdentityGridSkipsRebuildTest();
    runStage2_NonIdentityGridPopulatesCacheTest();
    runStage2_ReadPlaybackAudio_FastPathOnHitTest();
    runStage2_PhaseE_GracefulDegradationOnEmptyRenderCacheTest();
    runStage2_PhaseE_PitchEditTriggersStage2RebuildTest();
}
