/**
 * Tests/TestBasicDerivedAnalysis.cpp - smoke tests for the ReferenceFeatureSet
 * cache and the BasicReferenceFeatureBuilder fallback.
 */

#include "TestSupport.h"
#include "DSP/BasicReferenceFeatureBuilder.h"
#include "MaterializationStore.h"
#include "Utils/PitchCurve.h"
#include "Utils/TimeGrid.h"

namespace {

ReferenceFeatureSet makeReadyFeatures()
{
    ReferenceFeatureSet features;
    features.status = ReferenceFeatureStatus::Ready;
    features.producer = ReferenceFeatureProducer::Basic;
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

void runBasicDerivedAnalysisSlotSetGetSmokeTest()
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
    if (!out.isReady() || out.producer != ReferenceFeatureProducer::Basic) {
        logFail(testName, "reference feature cache did not round-trip Ready Basic state");
        return;
    }
    if (out.pitch.notes.size() != 1 || out.timing.anchors.size() != 1) {
        logFail(testName, "reference feature cache lost notes or timing anchors");
        return;
    }

    logPass(testName);
}

void runBasicReferenceFeatureBuilderGeneratesNotesAndTemporalEventsFromF0Test()
{
    constexpr const char* testName = "BasicReferenceFeatureBuilder_ProducesNotesAndTimingAnchors";

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

    const auto features = BasicReferenceFeatureBuilder::build(snapshot);
    if (!features.isReady()) {
        logFail(testName, "builder should produce Ready reference features");
        return;
    }
    if (features.producer != ReferenceFeatureProducer::Basic) {
        logFail(testName, "builder should tag features as Basic producer");
        return;
    }
    if (features.pitch.notes.empty()) {
        logFail(testName, "builder did not generate pitch notes from original F0");
        return;
    }
    if (features.timing.anchors.size() < 2) {
        logFail(testName, "builder did not generate at least two timing anchors from original F0");
        return;
    }
    for (size_t i = 1; i < features.timing.anchors.size(); ++i) {
        if (!TimeGridSnapshot::hasMinimumSourceSpacing(features.timing.anchors[i - 1].sourceSeconds,
                                                       features.timing.anchors[i].sourceSeconds)) {
            logFail(testName, "builder emitted timing anchors closer than TimeGrid spacing invariant");
            return;
        }
    }

    logPass(testName);
}

void runBasicDerivedAnalysisSuite()
{
    logSection("BasicDerivedAnalysis");
    runBasicDerivedAnalysisSlotSetGetSmokeTest();
    runBasicReferenceFeatureBuilderGeneratesNotesAndTemporalEventsFromF0Test();
}
