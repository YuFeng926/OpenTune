/**
 * Tests/MaterializationStoreTimeGridTests.cpp — Phase C integration tests for
 * MaterializationStore TimeGrid + TimeStretchCache + SoundTouchStretcher
 * lifecycle (vocal-time-stretch §3.6, §5.5, §6.2, §6.5).
 *
 * Suite aggregator: runMaterializationStoreTimeGridSuite() — registered in TestMain.cpp.
 */
#include "TestSupport.h"
#include "Inference/SoundTouchStretcher.h"
#include "Inference/TimeStretchCache.h"
#include "Utils/TimeGrid.h"
#include "Utils/TimeCoordinate.h"

#include <vector>

namespace {

constexpr double kSampleRate = TimeCoordinate::kRenderSampleRate;

MaterializationStore::CreateMaterializationRequest makeMatRequestWithDuration(double durationSec)
{
    MaterializationStore::CreateMaterializationRequest req;
    req.sourceId = 1;
    const int n = static_cast<int>(std::round(durationSec * kSampleRate));
    auto buf = std::make_shared<juce::AudioBuffer<float>>(1, n);
    buf->clear();
    req.audioBuffer = std::move(buf);
    req.pitchCurve = std::make_shared<PitchCurve>();
    req.renderCache = std::make_shared<RenderCache>();
    return req;
}

} // namespace

void runMatStore_TimeGrid_AutoSeededOnImportTest()
{
    constexpr const char* testName = "MatStore_TimeGrid_AutoSeededOnImport";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeMatRequestWithDuration(2.0));
    if (matId == 0) { logFail(testName, "createMaterialization returned 0"); return; }

    std::shared_ptr<const TimeGridSnapshot> tg;
    if (!store.getTimeGrid(matId, tg) || tg == nullptr) {
        logFail(testName, "expected auto-seeded TimeGrid after createMaterialization");
        return;
    }

    if (tg->handles().size() != 2) {
        logFail(testName, ("auto-seeded grid should have 2 handles (Start+End), got "
                           + std::to_string(tg->handles().size())).c_str());
        return;
    }
    if (tg->handles().front().kind != HandleKind::ClipStart
        || tg->handles().back().kind != HandleKind::ClipEnd) {
        logFail(testName, "endpoints should be ClipStart + ClipEnd");
        return;
    }
    if (std::abs(tg->handles().back().source_seconds - 2.0) > 1e-6
        || std::abs(tg->handles().back().output_seconds - 2.0) > 1e-6) {
        logFail(testName, "auto-seeded ClipEnd should be at duration=2.0 with output==source");
        return;
    }

    if (store.getTimeGridRevision(matId) != 1) {
        logFail(testName, "auto-seeded TimeGrid revision should be 1");
        return;
    }

    logPass(testName);
}

void runMatStore_TimeGrid_ExplicitInRequestTest()
{
    constexpr const char* testName = "MatStore_TimeGrid_ExplicitInRequest";

    auto explicitGrid = TimeGridSnapshot::makeIdentity(5.0);
    if (!explicitGrid) { logFail(testName, "makeIdentity(5.0) failed"); return; }

    MaterializationStore store;
    auto req = makeMatRequestWithDuration(2.0);
    req.timeGrid = explicitGrid;   // explicit grid, distinct from audio duration
    const uint64_t matId = store.createMaterialization(std::move(req));

    std::shared_ptr<const TimeGridSnapshot> tg;
    store.getTimeGrid(matId, tg);
    if (tg.get() != explicitGrid.get()) {
        logFail(testName, "request.timeGrid should be stored verbatim, not auto-seeded");
        return;
    }

    logPass(testName);
}

void runMatStore_TimeGrid_SetBumpsRevisionTest()
{
    constexpr const char* testName = "MatStore_TimeGrid_SetBumpsRevision";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeMatRequestWithDuration(2.0));
    const uint64_t initialRev = store.getTimeGridRevision(matId);

    auto newGrid = TimeGridSnapshot::makeIdentity(3.0);
    if (!store.setTimeGrid(matId, newGrid)) {
        logFail(testName, "setTimeGrid returned false on valid input");
        return;
    }

    const uint64_t afterRev = store.getTimeGridRevision(matId);
    if (afterRev <= initialRev) {
        logFail(testName, ("revision should increase: initial=" + std::to_string(initialRev)
                           + " after=" + std::to_string(afterRev)).c_str());
        return;
    }

    logPass(testName);
}

void runMatStore_TimeStretchCache_InvalidatedOnTimeGridSetTest()
{
    constexpr const char* testName = "MatStore_TimeStretchCache_InvalidatedOnTimeGridSet";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeMatRequestWithDuration(2.0));

    // Pre-seed the time-stretch cache for this materialization
    auto& tsCache = store.getTimeStretchCache();
    std::vector<float> dummy(100, 0.5f);
    tsCache.store(matId, std::move(dummy), /*pitchRev=*/1, /*timeGridRev=*/1, kSampleRate);
    if (!tsCache.hit(matId, 1, 1)) {
        logFail(testName, "precondition: TimeStretchCache should be hit after store");
        return;
    }

    // Now mutate TimeGrid
    auto newGrid = TimeGridSnapshot::makeIdentity(3.0);
    store.setTimeGrid(matId, newGrid);

    // TimeStretchCache should be invalidated
    if (tsCache.hit(matId, 1, 1)) {
        logFail(testName, "TimeStretchCache should be invalidated after setTimeGrid");
        return;
    }

    logPass(testName);
}

void runMatStore_TimeStretchCache_InvalidatedOnPitchCurveSetTest()
{
    constexpr const char* testName = "MatStore_TimeStretchCache_InvalidatedOnPitchCurveSet";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeMatRequestWithDuration(2.0));

    auto& tsCache = store.getTimeStretchCache();
    std::vector<float> dummy(100, 0.5f);
    tsCache.store(matId, std::move(dummy), 1, 1, kSampleRate);
    if (!tsCache.hit(matId, 1, 1)) {
        logFail(testName, "precondition: TimeStretchCache hit before mutation");
        return;
    }

    // Pitch edit (downstream of Stage 1) must invalidate Stage 2
    auto newCurve = std::make_shared<PitchCurve>();
    store.setPitchCurve(matId, newCurve);

    if (tsCache.hit(matId, 1, 1)) {
        logFail(testName, "TimeStretchCache should be invalidated after setPitchCurve "
                          "(spec §6.5: pitch edit invalidates BOTH stages)");
        return;
    }

    logPass(testName);
}

void runMatStore_TimeStretchCache_InvalidatedOnDeleteTest()
{
    constexpr const char* testName = "MatStore_TimeStretchCache_InvalidatedOnDelete";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeMatRequestWithDuration(2.0));

    auto& tsCache = store.getTimeStretchCache();
    std::vector<float> dummy(100, 0.5f);
    tsCache.store(matId, std::move(dummy), 1, 1, kSampleRate);

    store.deleteMaterialization(matId);

    if (tsCache.hit(matId, 1, 1)) {
        logFail(testName, "TimeStretchCache should be invalidated after deleteMaterialization");
        return;
    }

    logPass(testName);
}

void runMatStore_SoundTouchStretcher_LazyConstructTest()
{
    constexpr const char* testName = "MatStore_SoundTouchStretcher_LazyConstruct";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeMatRequestWithDuration(2.0));

    SoundTouchStretcher* rb1 = store.getOpenTuneStretcher(matId, kSampleRate, 1);
    if (rb1 == nullptr) {
        logFail(testName, "first call should lazily construct SoundTouchStretcher");
        return;
    }

    SoundTouchStretcher* rb2 = store.getOpenTuneStretcher(matId, kSampleRate, 1);
    if (rb2 != rb1) {
        logFail(testName, "subsequent calls should return the same instance");
        return;
    }

    if (rb1->phase() != SoundTouchStretcher::Phase::Idle) {
        logFail(testName, "freshly-constructed stretcher should be in Idle phase");
        return;
    }

    logPass(testName);
}

void runMatStore_SoundTouchStretcher_NullForUnknownIdTest()
{
    constexpr const char* testName = "MatStore_SoundTouchStretcher_NullForUnknownId";

    MaterializationStore store;
    SoundTouchStretcher* rb = store.getOpenTuneStretcher(/*matId=*/9999, kSampleRate, 1);
    if (rb != nullptr) {
        logFail(testName, "expected nullptr for unknown materializationId");
        return;
    }
    logPass(testName);
}

void runMatStore_TimeGridInSnapshotTest()
{
    constexpr const char* testName = "MatStore_TimeGrid_IncludedInSnapshot";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeMatRequestWithDuration(2.0));

    MaterializationStore::MaterializationSnapshot snap;
    if (!store.getSnapshot(matId, snap)) {
        logFail(testName, "getSnapshot failed");
        return;
    }
    if (snap.timeGrid == nullptr) {
        logFail(testName, "snapshot.timeGrid should be the auto-seeded grid");
        return;
    }
    if (snap.timeGridRevision != 1) {
        logFail(testName, "snapshot.timeGridRevision should be 1 after import");
        return;
    }
    logPass(testName);
}

void runMaterializationStoreTimeGridSuite()
{
    logSection("MatStore-TimeGrid");
    runMatStore_TimeGrid_AutoSeededOnImportTest();
    runMatStore_TimeGrid_ExplicitInRequestTest();
    runMatStore_TimeGrid_SetBumpsRevisionTest();
    runMatStore_TimeGridInSnapshotTest();
    runMatStore_TimeStretchCache_InvalidatedOnTimeGridSetTest();
    runMatStore_TimeStretchCache_InvalidatedOnPitchCurveSetTest();
    runMatStore_TimeStretchCache_InvalidatedOnDeleteTest();
    runMatStore_SoundTouchStretcher_LazyConstructTest();
    runMatStore_SoundTouchStretcher_NullForUnknownIdTest();
}
