/**
 * Tests/TestBasicDerivedAnalysis.cpp — Smoke tests for MaterializationStore DerivedAnalysis slot
 *
 * Covers:
 *   - DerivedAnalysis slot exists and defaults to NotRequested
 *   - BasicReferenceFeatureBuilder generates source-derived notes and temporal events from original F0
 */

#include "TestSupport.h"
#include "DSP/BasicReferenceFeatureBuilder.h"
#include "MaterializationStore.h"
#include "Utils/PitchCurve.h"
#include "Utils/TimeGrid.h"

// ============================================================================
// Test: Derived analysis slot exists and defaults correctly
// ============================================================================

void runBasicDerivedAnalysisSlotSetGetSmokeTest()
{
    constexpr const char* testName = "DerivedAnalysisSlot_SetGetSmoke";

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

void runBasicReferenceFeatureBuilderGeneratesNotesAndTemporalEventsFromF0Test()
{
    constexpr const char* testName = "AlignmentFeatures_BuilderProducesTemporalEventsNotHandles";

    MaterializationStore store;
    auto request = makeTestClipRequest();
    auto pitchCurve = std::make_shared<PitchCurve>();
    std::vector<float> f0(140, 0.0f);
    std::vector<float> energy(f0.size(), 1.0f);
    std::fill(f0.begin() + 10, f0.begin() + 55, 220.0f);
    std::fill(f0.begin() + 80, f0.begin() + 125, 330.0f);
    pitchCurve->setHopSize(160);
    pitchCurve->setSampleRate(16000.0);
    pitchCurve->setOriginalF0(f0);
    pitchCurve->setOriginalEnergy(energy);
    request.pitchCurve = std::move(pitchCurve);
    request.notes.clear();
    request.timeGrid.reset();

    const uint64_t matId = store.createMaterialization(std::move(request));
    if (matId == 0) {
        logFail(testName, "failed to create materialization");
        return;
    }

    MaterializationStore::MaterializationSnapshot snapshot;
    if (!store.getSnapshot(matId, snapshot)) {
        logFail(testName, "failed to read materialization snapshot");
        return;
    }

    const auto analysis = BasicReferenceFeatureBuilder::build(snapshot);
    if (analysis.state != F0ExtractionState::Ready) {
        logFail(testName, "builder should produce Ready source-derived features");
        return;
    }
    if (analysis.basicDerivedNotes.empty()) {
        logFail(testName, "builder did not generate notes from original F0");
        return;
    }
    if (analysis.temporalEvents.size() < 2) {
        logFail(testName, "builder did not generate at least two temporal events from original F0");
        return;
    }
    for (size_t i = 1; i < analysis.temporalEvents.size(); ++i) {
        if (!TimeGridSnapshot::hasMinimumSourceSpacing(analysis.temporalEvents[i - 1].sourceSeconds,
                                                       analysis.temporalEvents[i].sourceSeconds)) {
            logFail(testName, "builder emitted temporal events closer than TimeGrid spacing invariant");
            return;
        }
    }

    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runBasicDerivedAnalysisSuite()
{
    logSection("BasicDerivedAnalysis");
    runBasicDerivedAnalysisSlotSetGetSmokeTest();
    runBasicReferenceFeatureBuilderGeneratesNotesAndTemporalEventsFromF0Test();
}
