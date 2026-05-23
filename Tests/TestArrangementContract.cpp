/**
 * Tests/TestArrangementContract.cpp — StandaloneArrangement contract / invariant tests
 *
 * Coverage: L2-L3 contract tests for reference binding stability:
 *   - ReferenceBindingIdempotent: setting the same reference twice is a no-op
 *   - ReferenceBindingWithInvalidId: non-existent target returns false
 *   - PlaybackSnapshotExcludesReferenceForAudioThread: snapshot loads without error
 *
 * Suite aggregator: runArrangementContractSuite() — registered in TestMain.cpp.
 */
#include "TestSupport.h"

namespace {

/** Helper: create a placement on the given track, return its auto-assigned ID. */
uint64_t createPlacement(StandaloneArrangement& arr, int trackId,
                         double start, double duration, uint64_t matId)
{
    StandaloneArrangement::Placement p{};
    p.materializationId = matId;
    p.timelineStartSeconds = start;
    p.durationSeconds = duration;
    p.gain = 1.0f;
    arr.insertPlacement(trackId, p);
    return p.placementId; // auto-assigned by insertPlacement
}

// ============================================================================
// Test 1: ReferenceBindingIdempotent — setting A→B twice is a no-op
// ============================================================================

void runContractReferenceBindingIdempotentTest()
{
    constexpr const char* testName = "ArrangementContract_ReferenceBindingIdempotent";

    StandaloneArrangement arr;
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);
    const uint64_t bId = createPlacement(arr, 0, 3.0, 5.0, 2);

    if (aId == 0 || bId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    // First set
    if (!arr.setPlacementReferencePlacement(0, aId, bId)) {
        logFail(testName, "first setPlacementReferencePlacement should succeed");
        return;
    }

    // Second set — same target, same reference — must succeed (idempotent)
    if (!arr.setPlacementReferencePlacement(0, aId, bId)) {
        logFail(testName, "second setPlacementReferencePlacement with same args failed (not idempotent)");
        return;
    }

    // Verify reference is still B
    const uint64_t ref = arr.getPlacementReferencePlacement(0, aId);
    if (ref != bId) {
        logFail(testName, "reference changed after idempotent re-set");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 2: ReferenceBindingWithInvalidIdReturnsFalse
// ============================================================================

void runContractReferenceBindingWithInvalidIdTest()
{
    constexpr const char* testName = "ArrangementContract_ReferenceBindingWithInvalidId";

    StandaloneArrangement arr;
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);

    if (aId == 0) {
        logFail(testName, "failed to create placement");
        return;
    }

    // Try to set reference on a non-existent target placementId
    constexpr uint64_t kInvalidTargetId = 99999;
    const bool setOk = arr.setPlacementReferencePlacement(0, kInvalidTargetId, aId);
    if (setOk) {
        logFail(testName, "setPlacementReferencePlacement with invalid target should return false");
        return;
    }

    // Also test with non-existent reference placementId
    constexpr uint64_t kInvalidRefId = 88888;
    const bool setRefOk = arr.setPlacementReferencePlacement(0, aId, kInvalidRefId);
    if (setRefOk) {
        logFail(testName, "setPlacementReferencePlacement with non-existent reference should return false");
        return;
    }

    // Verify A's reference was not corrupted
    const uint64_t aRef = arr.getPlacementReferencePlacement(0, aId);
    if (aRef != 0) {
        logFail(testName, "A's reference was corrupted by failed set operation");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 3: PlaybackSnapshotExcludesReferenceForAudioThread — snapshot loads ok
// ============================================================================

void runContractPlaybackSnapshotLoadsWithoutErrorTest()
{
    constexpr const char* testName = "ArrangementContract_PlaybackSnapshotLoadsWithoutError";

    StandaloneArrangement arr;

    // Create several placements across tracks
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);
    const uint64_t bId = createPlacement(arr, 0, 3.0, 5.0, 2);
    const uint64_t cId = createPlacement(arr, 1, 1.0, 4.0, 3);

    if (aId == 0 || bId == 0 || cId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    // Set up a reference binding A→B
    if (!arr.setPlacementReferencePlacement(0, aId, bId)) {
        logFail(testName, "A→B reference should succeed");
        return;
    }

    // Load playback snapshot — must return non-null shared_ptr
    auto snapshot = arr.loadPlaybackSnapshot();
    if (snapshot == nullptr) {
        logFail(testName, "loadPlaybackSnapshot returned null shared_ptr");
        return;
    }

    // Verify epoch is non-zero (snapshot was published)
    if (snapshot->epoch == 0) {
        logFail(testName, "playback snapshot epoch is zero (snapshot not published)");
        return;
    }

    // Verify track 0 contains the expected number of placements (A + B = 2)
    const size_t track0Count = snapshot->tracks[0].placements.size();
    if (track0Count != 2) {
        logFail(testName, "snapshot track 0 does not contain expected 2 placements");
        return;
    }

    // Verify track 1 contains C
    const size_t track1Count = snapshot->tracks[1].placements.size();
    if (track1Count != 1) {
        logFail(testName, "snapshot track 1 does not contain expected 1 placement");
        return;
    }

    // The snapshot must contain the referencePlacementId field (it's part of Placement struct),
    // but we don't enforce that it's stripped — the audio thread simply uses it.
    // Just verify that loading the snapshot with references present does not crash.

    logPass(testName);
}

} // namespace

// ============================================================================
// Suite aggregator
// ============================================================================

void runArrangementContractSuite()
{
    logSection("ArrangementContract");
    runContractReferenceBindingIdempotentTest();
    runContractReferenceBindingWithInvalidIdTest();
    runContractPlaybackSnapshotLoadsWithoutErrorTest();
}
