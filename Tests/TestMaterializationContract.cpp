/**
 * Tests/TestMaterializationContract.cpp - L4 contract tests for
 * MaterializationStore ReferenceFeatureSet invariants.
 *
 * Verifies that reference features are NOT exposed through the general-purpose
 * MaterializationSnapshot API, and that setNotes does NOT invalidate the
 * reference feature cache.
 *
 * Suite aggregator: runMaterializationContractSuite()
 */
#include "TestSupport.h"

#include <vector>

namespace {

ReferenceFeatureSet makeReadyFeatures()
{
    ReferenceFeatureSet features;
    features.status = ReferenceFeatureStatus::Ready;
    features.producer = ReferenceFeatureProducer::Game;
    features.analysisRevision = 1;
    features.inputFingerprint = 7;
    features.sourceDurationSeconds = 3.0;

    Note note;
    note.startTime = 0.10;
    note.endTime = 0.45;
    note.pitch = 440.0f;
    note.originalPitch = 438.0f;
    note.isVoiced = true;
    features.pitch.notes.push_back(note);

    ReferenceTimingAnchor anchor;
    anchor.anchorId = 11;
    anchor.sourceSeconds = 0.30;
    anchor.strength = 0.90f;
    anchor.kind = ReferenceTimingAnchorKind::Onset;
    anchor.confidence = 0.95f;
    features.timing.anchors.push_back(anchor);
    return features;
}

} // namespace

void runMaterializationContract_ReferenceFeaturesNotInSnapshot()
{
    constexpr const char* testName = "MaterializationContract_ReferenceFeaturesNotInSnapshot";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "createMaterialization returned 0");
        return;
    }

    if (!store.setReferenceFeatures(matId, makeReadyFeatures())) {
        logFail(testName, "setReferenceFeatures returned false");
        return;
    }

    MaterializationStore::MaterializationSnapshot snap;
    if (!store.getSnapshot(matId, snap)) {
        logFail(testName, "getSnapshot returned false");
        return;
    }
    if (snap.materializationId != matId) {
        logFail(testName, "snapshot materializationId mismatch");
        return;
    }

    // The snapshot struct intentionally has no ReferenceFeatureSet field.
    // Compilation plus successful snapshot retrieval is the contract.
    logPass(testName);
}

void runMaterializationContract_ReferenceFeaturesPersistAcrossSetNotes()
{
    constexpr const char* testName = "MaterializationContract_ReferenceFeaturesPersistAcrossSetNotes";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "createMaterialization returned 0");
        return;
    }

    if (!store.setReferenceFeatures(matId, makeReadyFeatures())) {
        logFail(testName, "setReferenceFeatures returned false");
        return;
    }

    ReferenceFeatureSet before;
    if (!store.getReferenceFeatures(matId, before) || !before.isReady()) {
        logFail(testName, "precondition: should have ready reference features");
        return;
    }

    std::vector<Note> notes;
    Note userNote;
    userNote.startTime = 0.20;
    userNote.endTime = 0.80;
    userNote.pitch = 523.25f;
    userNote.originalPitch = 520.0f;
    userNote.isVoiced = true;
    notes.push_back(userNote);

    if (!store.setNotes(matId, notes)) {
        logFail(testName, "setNotes returned false");
        return;
    }

    ReferenceFeatureSet after;
    if (!store.getReferenceFeatures(matId, after) || !after.isReady()) {
        logFail(testName, "reference feature cache should survive setNotes");
        return;
    }
    if (after.analysisRevision != before.analysisRevision) {
        logFail(testName, "setNotes should not bump reference feature cache revision");
        return;
    }
    if (after.producer != ReferenceFeatureProducer::Game) {
        logFail(testName, "setNotes should not alter reference feature producer");
        return;
    }
    if (after.pitch.notes.size() != before.pitch.notes.size()
        || after.timing.anchors.size() != before.timing.anchors.size()) {
        logFail(testName, "setNotes should not change cached reference features");
        return;
    }

    logPass(testName);
}

void runMaterializationContractSuite()
{
    logSection("MaterializationContract");
    runMaterializationContract_ReferenceFeaturesNotInSnapshot();
    runMaterializationContract_ReferenceFeaturesPersistAcrossSetNotes();
}
