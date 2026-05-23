/**
 * Tests/TestPlacementReferenceCascade.cpp — StandaloneArrangement reference binding cascade tests
 *
 * Coverage: L2 unit tests for cascading reference binding behavior:
 *   - DeletePlacementCascadesToBindings: deleting a referenced placement clears all inbound bindings
 *   - MovePlacementCrossTrackPreservesOverlap: cross-track move keeps reference when overlap persists
 *   - SplitPlacementInheritsReference: timeline adjustment preserves reference under overlap
 *
 * Suite aggregator: runPlacementReferenceCascadeSuite() — registered in TestMain.cpp.
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
// Test 1: DeletePlacementCascadesToBindings — multiple targets reference B,
//         deleting B clears all of them
// ============================================================================

void runCascadeDeletePlacementCascadesToBindingsTest()
{
    constexpr const char* testName = "PlacementReferenceCascade_DeleteCascadesToBindings";

    StandaloneArrangement arr;

    // Create A, B, C all on track 0
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);
    const uint64_t bId = createPlacement(arr, 0, 3.0, 5.0, 2);
    const uint64_t cId = createPlacement(arr, 0, 1.0, 5.0, 3);

    if (aId == 0 || bId == 0 || cId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    // A → B  and  C → B
    if (!arr.setPlacementReferencePlacement(0, aId, bId)) {
        logFail(testName, "A→B reference should succeed");
        return;
    }
    if (!arr.setPlacementReferencePlacement(0, cId, bId)) {
        logFail(testName, "C→B reference should succeed");
        return;
    }

    // Verify bindings before delete
    if (arr.getPlacementReferencePlacement(0, aId) != bId) {
        logFail(testName, "A→B binding not established before delete");
        return;
    }
    if (arr.getPlacementReferencePlacement(0, cId) != bId) {
        logFail(testName, "C→B binding not established before delete");
        return;
    }

    // Delete B
    if (!arr.deletePlacementById(0, bId)) {
        logFail(testName, "failed to delete placement B");
        return;
    }

    // Verify both A and C have reference cleared
    if (arr.getPlacementReferencePlacement(0, aId) != 0) {
        logFail(testName, "A's reference to B was not cleared after B was deleted");
        return;
    }
    if (arr.getPlacementReferencePlacement(0, cId) != 0) {
        logFail(testName, "C's reference to B was not cleared after B was deleted");
        return;
    }

    // Verify A and C still exist
    StandaloneArrangement::Placement aOut{}, cOut{};
    if (!arr.getPlacementById(0, aId, aOut) || !arr.getPlacementById(0, cId, cOut)) {
        logFail(testName, "A or C was unexpectedly deleted when B was deleted");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 2: MovePlacementCrossTrackPreservesOverlap — cross-track move keeps ref
// ============================================================================

void runCascadeMovePlacementCrossTrackPreservesOverlapTest()
{
    constexpr const char* testName = "PlacementReferenceCascade_MoveCrossTrackPreservesOverlap";

    StandaloneArrangement arr;

    // A on track 0 at [0, 5], B on track 1 at [3, 8]
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);
    const uint64_t bId = createPlacement(arr, 1, 3.0, 5.0, 2);

    if (aId == 0 || bId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    // A → B
    if (!arr.setPlacementReferencePlacement(0, aId, bId)) {
        logFail(testName, "initial A→B reference should succeed");
        return;
    }

    // Move A from track 0 to track 1 at new start [1, 6] — still overlaps B [3, 8]
    if (!arr.movePlacementToTrack(0, 1, aId, 1.0)) {
        logFail(testName, "movePlacementToTrack failed");
        return;
    }

    // Verify A is now on track 1
    StandaloneArrangement::Placement aOut{};
    if (!arr.getPlacementById(1, aId, aOut)) {
        logFail(testName, "A not found on track 1 after move");
        return;
    }

    if (!approxEqual(aOut.timelineStartSeconds, 1.0)) {
        logFail(testName, "A's timeline start was not updated after move");
        return;
    }

    // Verify reference preserved (overlap: A[1,6] ∩ B[3,8] = 3s > 0)
    const uint64_t ref = arr.getPlacementReferencePlacement(1, aId);
    if (ref != bId) {
        logFail(testName, "reference was cleared even though A and B still overlap after cross-track move");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 3: TimelineAdjustPreservesReference — setPlacementTimelineStartSeconds
//         preserves reference when overlap is maintained
// ============================================================================

void runCascadeTimelineAdjustPreservesReferenceTest()
{
    constexpr const char* testName = "PlacementReferenceCascade_TimelineAdjustPreservesReference";

    StandaloneArrangement arr;

    // A on track 0 at [0, 10], B on track 0 at [3, 8]
    const uint64_t aId = createPlacement(arr, 0, 0.0, 10.0, 1);
    const uint64_t bId = createPlacement(arr, 0, 3.0, 5.0, 2);

    if (aId == 0 || bId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    // A → B
    if (!arr.setPlacementReferencePlacement(0, aId, bId)) {
        logFail(testName, "initial A→B reference should succeed");
        return;
    }

    // Adjust A's timeline start to 2.0 → A becomes [2, 12]
    // Still overlaps B [3, 8] by 5 seconds
    if (!arr.setPlacementTimelineStartSeconds(0, aId, 2.0)) {
        logFail(testName, "setPlacementTimelineStartSeconds failed");
        return;
    }

    // Verify position updated
    StandaloneArrangement::Placement aOut{};
    if (!arr.getPlacementById(0, aId, aOut)) {
        logFail(testName, "A not found after timeline adjustment");
        return;
    }

    if (!approxEqual(aOut.timelineStartSeconds, 2.0)) {
        logFail(testName, "A's timeline start was not updated");
        return;
    }

    // Reference must survive — overlap still exists
    const uint64_t ref = arr.getPlacementReferencePlacement(0, aId);
    if (ref != bId) {
        logFail(testName, "reference was cleared by setPlacementTimelineStartSeconds despite persistent overlap");
        return;
    }

    logPass(testName);
}

} // namespace

// ============================================================================
// Suite aggregator
// ============================================================================

void runPlacementReferenceCascadeSuite()
{
    logSection("PlacementReferenceCascade");
    runCascadeDeletePlacementCascadesToBindingsTest();
    runCascadeMovePlacementCrossTrackPreservesOverlapTest();
    runCascadeTimelineAdjustPreservesReferenceTest();
}
