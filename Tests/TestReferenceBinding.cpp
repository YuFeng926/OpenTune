/**
 * Tests/TestReferenceBinding.cpp — StandaloneArrangement reference binding lifecycle tests
 *
 * Coverage: L2 unit tests for the reference binding CRUD API and lifecycle:
 *   - SetAndGet, ClearBinding
 *   - RejectsSelfReference, RejectsCyclicReference
 *   - DeleteRefClearsBinding
 *   - MovePreservesBindingIfOverlap, MoveClearsBindingIfNoOverlap
 *
 * Suite aggregator: runReferenceBindingSuite() — registered in TestMain.cpp.
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
// Test 1: SetAndGet — create two overlapping placements, A→B, verify get
// ============================================================================

void runRefBindSetAndGetTest()
{
    constexpr const char* testName = "ReferenceBinding_SetAndGet";

    StandaloneArrangement arr;
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);
    const uint64_t bId = createPlacement(arr, 0, 3.0, 5.0, 2);

    if (aId == 0 || bId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    const bool setOk = arr.setPlacementReferencePlacement(0, aId, bId);
    if (!setOk) {
        logFail(testName, "setPlacementReferencePlacement returned false");
        return;
    }

    const uint64_t got = arr.getPlacementReferencePlacement(0, aId);
    if (got != bId) {
        logFail(testName, "getPlacementReferencePlacement did not return the expected reference ID");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 2: ClearBinding — set reference then clear, verify get returns 0
// ============================================================================

void runRefBindClearBindingTest()
{
    constexpr const char* testName = "ReferenceBinding_ClearBinding";

    StandaloneArrangement arr;
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);
    const uint64_t bId = createPlacement(arr, 0, 2.0, 5.0, 2);

    if (aId == 0 || bId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    arr.setPlacementReferencePlacement(0, aId, bId);

    const bool cleared = arr.clearPlacementReferencePlacement(0, aId);
    if (!cleared) {
        logFail(testName, "clearPlacementReferencePlacement returned false");
        return;
    }

    const uint64_t got = arr.getPlacementReferencePlacement(0, aId);
    if (got != 0) {
        logFail(testName, "reference was not cleared to 0");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 3: RejectsSelfReference — set A→A, verify returns false AND get returns 0
// ============================================================================

void runRefBindRejectsSelfReferenceTest()
{
    constexpr const char* testName = "ReferenceBinding_RejectsSelfReference";

    StandaloneArrangement arr;
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);

    if (aId == 0) {
        logFail(testName, "failed to create placement A");
        return;
    }

    const bool setOk = arr.setPlacementReferencePlacement(0, aId, aId);
    if (setOk) {
        logFail(testName, "self-reference was not rejected");
        return;
    }

    const uint64_t got = arr.getPlacementReferencePlacement(0, aId);
    if (got != 0) {
        logFail(testName, "self-reference leaked into referencePlacementId");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 4: RejectsCyclicReference — A→B already set, then try B→A, verify fails
// ============================================================================

void runRefBindRejectsCyclicReferenceTest()
{
    constexpr const char* testName = "ReferenceBinding_RejectsCyclicReference";

    StandaloneArrangement arr;
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);
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

    // B → A should be rejected as cyclic
    const bool setOk = arr.setPlacementReferencePlacement(0, bId, aId);
    if (setOk) {
        logFail(testName, "cyclic reference B→A was not rejected");
        return;
    }

    // Verify B's reference is still 0
    const uint64_t bRef = arr.getPlacementReferencePlacement(0, bId);
    if (bRef != 0) {
        logFail(testName, "B's reference was set despite cyclic rejection");
        return;
    }

    // Verify A's reference is still B
    const uint64_t aRef = arr.getPlacementReferencePlacement(0, aId);
    if (aRef != bId) {
        logFail(testName, "A's reference was corrupted by failed B→A attempt");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 5: DeleteRefClearsBinding — A→B, delete B, verify A's reference cleared
// ============================================================================

void runRefBindDeleteRefClearsBindingTest()
{
    constexpr const char* testName = "ReferenceBinding_DeleteRefClearsBinding";

    StandaloneArrangement arr;
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);
    const uint64_t bId = createPlacement(arr, 0, 3.0, 5.0, 2);

    if (aId == 0 || bId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    arr.setPlacementReferencePlacement(0, aId, bId);

    // Verify binding before delete
    if (arr.getPlacementReferencePlacement(0, aId) != bId) {
        logFail(testName, "A→B reference not established before delete");
        return;
    }

    // Delete B
    if (!arr.deletePlacementById(0, bId)) {
        logFail(testName, "failed to delete placement B");
        return;
    }

    // Verify A's reference cleared
    const uint64_t got = arr.getPlacementReferencePlacement(0, aId);
    if (got != 0) {
        logFail(testName, "A's reference was not cleared after B was deleted");
        return;
    }

    // Verify A still exists
    StandaloneArrangement::Placement aOut{};
    if (!arr.getPlacementById(0, aId, aOut)) {
        logFail(testName, "A was unexpectedly deleted along with B");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 6: MovePreservesBindingIfOverlap — A→B still overlapping after move
// ============================================================================

void runRefBindMovePreservesBindingIfOverlapTest()
{
    constexpr const char* testName = "ReferenceBinding_MovePreservesBindingIfOverlap";

    StandaloneArrangement arr;
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);   // A: [0, 5]
    const uint64_t bId = createPlacement(arr, 1, 3.0, 5.0, 2);   // B: [3, 8]

    if (aId == 0 || bId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    // A → B (cross-track reference)
    if (!arr.setPlacementReferencePlacement(0, aId, bId)) {
        logFail(testName, "initial A→B reference should succeed");
        return;
    }

    // Move A to [1, 6] — still overlaps B [3, 8] by 3 seconds
    if (!arr.setPlacementTimelineStartSeconds(0, aId, 1.0)) {
        logFail(testName, "setPlacementTimelineStartSeconds failed");
        return;
    }

    const uint64_t got = arr.getPlacementReferencePlacement(0, aId);
    if (got != bId) {
        logFail(testName, "reference was cleared even though A and B still overlap");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 7: MoveClearsBindingIfNoOverlap — A→B no longer overlapping after move
// ============================================================================

void runRefBindMoveClearsBindingIfNoOverlapTest()
{
    constexpr const char* testName = "ReferenceBinding_MoveClearsBindingIfNoOverlap";

    StandaloneArrangement arr;
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);   // A: [0, 5]
    const uint64_t bId = createPlacement(arr, 1, 3.0, 5.0, 2);   // B: [3, 8]

    if (aId == 0 || bId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    // A → B
    if (!arr.setPlacementReferencePlacement(0, aId, bId)) {
        logFail(testName, "initial A→B reference should succeed");
        return;
    }

    // Move A to [10, 15] — no overlap with B [3, 8]
    if (!arr.setPlacementTimelineStartSeconds(0, aId, 10.0)) {
        logFail(testName, "setPlacementTimelineStartSeconds failed");
        return;
    }

    const uint64_t got = arr.getPlacementReferencePlacement(0, aId);
    if (got != 0) {
        logFail(testName, "reference was not cleared even though A and B no longer overlap");
        return;
    }

    logPass(testName);
}

} // namespace

// ============================================================================
// Suite aggregator
// ============================================================================

void runReferenceBindingSuite()
{
    logSection("ReferenceBindingLifecycle");
    runRefBindSetAndGetTest();
    runRefBindClearBindingTest();
    runRefBindRejectsSelfReferenceTest();
    runRefBindRejectsCyclicReferenceTest();
    runRefBindDeleteRefClearsBindingTest();
    runRefBindMovePreservesBindingIfOverlapTest();
    runRefBindMoveClearsBindingIfNoOverlapTest();
}
