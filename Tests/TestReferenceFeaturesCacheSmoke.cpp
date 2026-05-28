/**
 * Tests/TestReferenceFeaturesCacheSmoke.cpp - smoke tests for the runtime
 * ReferenceFeatureSet cache slot.
 */

#include "TestSupport.h"
#include "MaterializationStore.h"

namespace {

ReferenceFeatureSet makeReadyFeatures()
{
    ReferenceFeatureSet features;
    features.status = ReferenceFeatureStatus::Ready;
    features.producer = ReferenceFeatureProducer::Game;
    features.analysisRevision = 1;
    features.inputFingerprint = 1;
    features.sourceDurationSeconds = 2.0;

    Note note;
    note.startTime = 0.10;
    note.endTime = 0.40;
    note.pitch = 440.0f;
    note.originalPitch = 440.0f;
    note.isVoiced = true;
    features.pitch.notes.push_back(note);

    ReferenceTimingAnchor anchor;
    anchor.anchorId = 1;
    anchor.sourceSeconds = 0.20;
    anchor.strength = 1.0f;
    anchor.kind = ReferenceTimingAnchorKind::Onset;
    anchor.confidence = 1.0f;
    features.timing.anchors.push_back(anchor);
    return features;
}

} // namespace

void runReferenceFeaturesCacheSlotSetGetSmokeTest()
{
    constexpr const char* testName = "ReferenceFeaturesSlot_SetGetSmoke";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "failed to create materialization");
        return;
    }

    ReferenceFeatureSet empty;
    if (store.getReferenceFeatures(matId, empty)) {
        logFail(testName, "fresh materialization should not report ready reference features");
        return;
    }
    if (empty.status != ReferenceFeatureStatus::NotRequested || empty.analysisRevision != 0) {
        logFail(testName, "fresh reference feature cache should reset to NotRequested");
        return;
    }

    const auto seeded = makeReadyFeatures();
    if (!store.setReferenceFeatures(matId, seeded)) {
        logFail(testName, "setReferenceFeatures returned false");
        return;
    }

    ReferenceFeatureSet out;
    if (!store.getReferenceFeatures(matId, out)) {
        logFail(testName, "getReferenceFeatures returned false after set");
        return;
    }
    if (!out.isReady() || out.producer != ReferenceFeatureProducer::Game) {
        logFail(testName, "reference feature cache did not round-trip Ready Game state");
        return;
    }
    if (out.pitch.notes.size() != 1 || out.timing.anchors.size() != 1) {
        logFail(testName, "reference feature cache lost notes or timing anchors");
        return;
    }

    logPass(testName);
}

void runReferenceFeaturesCacheSmokeSuite()
{
    logSection("ReferenceFeaturesCacheSmoke");
    runReferenceFeaturesCacheSlotSetGetSmokeTest();
}
