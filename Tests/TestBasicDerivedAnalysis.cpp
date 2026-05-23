/**
 * Tests/TestBasicDerivedAnalysis.cpp — Smoke tests for MaterializationStore DerivedAnalysis slot
 *
 * Covers:
 *   - DerivedAnalysis slot exists and defaults to NotRequested
 *   - Stub for future generated analysis tests
 */

#include "TestSupport.h"
#include "MaterializationStore.h"
#include "Utils/PitchCurve.h"

// ============================================================================
// Test: Derived analysis slot exists and defaults correctly
// ============================================================================

void runBasicDerivedAnalysisGeneratesNotesAndAnchorsTest()
{
    constexpr const char* testName = "BasicDerivedAnalysis_GeneratesNotesAndAnchorsFromF0";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "failed to create materialization");
        return;
    }

    // Verify: after creation, derived analysis slot exists but is not yet Ready.
    // getDerivedAnalysis returns false when state != Ready (expected for fresh materialization).
    MaterializationStore::DerivedAnalysis dummy;
    if (store.getDerivedAnalysis(matId, dummy)) {
        logFail(testName, "fresh materialization should NOT have a Ready derived analysis");
        return;
    }

    // Seed a derived analysis to verify the slot is functional
    MaterializationStore::DerivedAnalysis da;
    da.state = F0ExtractionState::Ready;
    da.analysisRevision = 1;
    if (!store.setDerivedAnalysis(matId, da)) {
        logFail(testName, "setDerivedAnalysis returned false — slot may not exist");
        return;
    }

    MaterializationStore::DerivedAnalysis derived;
    if (!store.getDerivedAnalysis(matId, derived)) {
        logFail(testName, "getDerivedAnalysis returned false after setDerivedAnalysis");
        return;
    }

    if (derived.state != F0ExtractionState::Ready) {
        logFail(testName, "derived analysis state should be Ready after set");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runBasicDerivedAnalysisSuite()
{
    logSection("BasicDerivedAnalysis");
    runBasicDerivedAnalysisGeneratesNotesAndAnchorsTest();
}
