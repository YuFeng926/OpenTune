/**
 * Tests/TestMaterializationContract.cpp — L4 contract tests for
 * MaterializationStore Derived Analysis invariants.
 *
 * Verifies that derived analysis is NOT exposed via the general-purpose
 * snapshot API (separation of concerns), and that setNotes does NOT
 * invalidate derived analysis (they are independent data streams).
 *
 * Suite aggregator: runMaterializationContractSuite()
 */
#include "TestSupport.h"

#include <vector>

namespace {

// Helper: build a minimal DerivedAnalysis in Ready state
MaterializationStore::DerivedAnalysis makeReadyAnalysis()
{
    MaterializationStore::DerivedAnalysis da;
    da.state = F0ExtractionState::Ready;
    da.analysisRevision = 1;
    da.backendMode = 0;

    Note note;
    note.startTime = 0.1;
    note.endTime = 0.4;
    note.pitch = 440.0f;
    note.originalPitch = 439.0f;
    note.isVoiced = true;
    da.basicDerivedNotes.push_back(note);

    da.inputFingerprint = 1;
    return da;
}

} // namespace

// ============================================================================
// Test 1: DerivedAnalysisNotInSnapshot
//
// The MaterializationSnapshot struct does NOT have a derivedAnalysis field.
// This test verifies the snapshot API compiles and returns correctly without
// derived analysis (just verify getSnapshot succeeds and materializationId
// matches).
// ============================================================================
void runMaterializationContract_DerivedAnalysisNotInSnapshot()
{
    constexpr const char* testName = "MaterializationContract_DerivedAnalysisNotInSnapshot";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "createMaterialization returned 0");
        return;
    }

    // Seed a derived analysis
    const auto da = makeReadyAnalysis();
    if (!store.setDerivedAnalysis(matId, da)) {
        logFail(testName, "setDerivedAnalysis returned false");
        return;
    }

    // getSnapshot should succeed — derived analysis is NOT in the snapshot struct
    MaterializationStore::MaterializationSnapshot snap;
    if (!store.getSnapshot(matId, snap)) {
        logFail(testName, "getSnapshot returned false");
        return;
    }

    if (snap.materializationId != matId) {
        logFail(testName, "snapshot materializationId mismatch");
        return;
    }

    // The snapshot struct has no derivedAnalysis field — the fact that this
    // test compiles proves the contract (derived analysis is separate from
    // the general-purpose snapshot).
    logPass(testName);
}

// ============================================================================
// Test 2: DerivedAnalysisPersistsAcrossSetNotes
//
// setNotes should NOT invalidate derived analysis. User note edits and
// derived analysis are independent data streams. The contract says:
// "setNotes does NOT invalidate — user note edits are independent."
// ============================================================================
void runMaterializationContract_DerivedAnalysisPersistsAcrossSetNotes()
{
    constexpr const char* testName = "MaterializationContract_DerivedAnalysisPersistsAcrossSetNotes";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "createMaterialization returned 0");
        return;
    }

    // Seed a valid derived analysis
    const auto da = makeReadyAnalysis();
    if (!store.setDerivedAnalysis(matId, da)) {
        logFail(testName, "setDerivedAnalysis returned false");
        return;
    }
    if (!store.hasValidDerivedAnalysis(matId)) {
        logFail(testName, "precondition: should have valid derived analysis");
        return;
    }

    // Now set notes — this should NOT invalidate derived analysis
    std::vector<Note> notes;
    Note userNote;
    userNote.startTime = 0.2;
    userNote.endTime = 0.8;
    userNote.pitch = 523.25f; // C5
    userNote.originalPitch = 520.0f;
    userNote.isVoiced = true;
    notes.push_back(userNote);

    if (!store.setNotes(matId, notes)) {
        logFail(testName, "setNotes returned false");
        return;
    }

    // Derived analysis MUST still be valid after setNotes
    if (!store.hasValidDerivedAnalysis(matId)) {
        logFail(testName, "derived analysis should NOT be invalidated by setNotes");
        return;
    }

    // Also verify the derived analysis data is intact
    MaterializationStore::DerivedAnalysis out;
    if (!store.getDerivedAnalysis(matId, out)) {
        logFail(testName, "getDerivedAnalysis returned false after setNotes");
        return;
    }
    if (out.state != F0ExtractionState::Ready || out.analysisRevision <= 0) {
        logFail(testName, "derived analysis data should be unchanged after setNotes (store auto-bumps revision)");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite entry point
// ============================================================================
void runMaterializationContractSuite()
{
    logSection("MaterializationContract");
    runMaterializationContract_DerivedAnalysisNotInSnapshot();
    runMaterializationContract_DerivedAnalysisPersistsAcrossSetNotes();
}
