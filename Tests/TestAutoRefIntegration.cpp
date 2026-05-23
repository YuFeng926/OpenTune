/**
 * Tests/TestAutoRefIntegration.cpp — Integration stub tests for ReferenceAutoAlign + StandaloneArrangement
 *
 * Covers:
 *   - Reference binding wire-up through StandaloneArrangement API
 *   - setPlacementReferencePlacement / getPlacementReferencePlacement / clearPlacementReferencePlacement
 */

#include "TestSupport.h"
#include "StandaloneArrangement.h"

// ============================================================================
// Test: End-to-end reference binding through StandaloneArrangement
// ============================================================================

void runAutoRefIntegrationEndToEndStubTest()
{
    constexpr const char* testName = "AutoRefIntegration_EndToEndStub";

    StandaloneArrangement arrangement;

    // Insert placement 1 on track 0
    StandaloneArrangement::Placement p1;
    p1.placementId = 1;
    p1.materializationId = 100;
    p1.timelineStartSeconds = 0.0;
    p1.durationSeconds = 2.0;
    p1.gain = 1.0f;
    p1.name = "Target";

    if (!arrangement.insertPlacement(0, p1)) {
        logFail(testName, "failed to insert target placement");
        return;
    }

    // Insert placement 2 on track 0 (overlapping)
    StandaloneArrangement::Placement p2;
    p2.placementId = 2;
    p2.materializationId = 200;
    p2.timelineStartSeconds = 0.5;
    p2.durationSeconds = 1.5;
    p2.gain = 1.0f;
    p2.name = "Reference";

    if (!arrangement.insertPlacement(0, p2)) {
        logFail(testName, "failed to insert reference placement");
        return;
    }

    // Set reference binding: p1 references p2
    if (!arrangement.setPlacementReferencePlacement(0, p1.placementId, p2.placementId)) {
        logFail(testName, "failed to set reference placement binding");
        return;
    }

    // Verify binding was set
    const uint64_t refId = arrangement.getPlacementReferencePlacement(0, p1.placementId);
    if (refId != p2.placementId) {
        logFail(testName, "getPlacementReferencePlacement returned wrong reference ID");
        return;
    }

    // Clear the binding
    if (!arrangement.clearPlacementReferencePlacement(0, p1.placementId)) {
        logFail(testName, "failed to clear reference placement binding");
        return;
    }

    // Verify binding was cleared
    const uint64_t clearedRefId = arrangement.getPlacementReferencePlacement(0, p1.placementId);
    if (clearedRefId != 0) {
        logFail(testName, "getPlacementReferencePlacement should return 0 after clear");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runAutoRefIntegrationSuite()
{
    logSection("AutoRefIntegration");
    runAutoRefIntegrationEndToEndStubTest();
}
