/**
 * Tests/TestReferenceFeaturesCacheLifecycle.cpp - L3 tests for the
 * MaterializationStore ReferenceFeatureSet cache lifecycle.
 *
 * Covers set/get, invalidation on pitch-curve change, and invalidation
 * on audio replacement per the reference feature cache contract.
 *
 * Suite aggregator: runReferenceFeaturesCacheLifecycleSuite()
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
    features.inputFingerprint = 42;
    features.sourceDurationSeconds = 2.0;

    Note note;
    note.startTime = 0.10;
    note.endTime = 0.40;
    note.pitch = 440.0f;
    note.originalPitch = 439.0f;
    note.isVoiced = true;
    features.pitch.notes.push_back(note);

    ReferenceTimingAnchor anchor;
    anchor.anchorId = 1;
    anchor.sourceSeconds = 0.25;
    anchor.strength = 0.85f;
    anchor.kind = ReferenceTimingAnchorKind::PitchTransition;
    anchor.confidence = 0.85f;
    features.timing.anchors.push_back(anchor);
    return features;
}

bool isReferenceCacheCleared(const ReferenceFeatureSet& features)
{
    return features.status == ReferenceFeatureStatus::NotRequested
        && features.analysisRevision == 0
        && features.pitch.notes.empty()
        && features.timing.anchors.empty()
        && features.inputFingerprint == 0;
}

} // namespace

void runReferenceFeatures_SetAndGet()
{
    constexpr const char* testName = "ReferenceFeatures_SetAndGet";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "createMaterialization returned 0");
        return;
    }

    ReferenceFeatureSet initial;
    if (store.getReferenceFeatures(matId, initial)) {
        logFail(testName, "fresh materialization should not report ready reference features");
        return;
    }
    if (!isReferenceCacheCleared(initial)) {
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
    if (!out.isReady()) {
        logFail(testName, "reference feature cache should be Ready after set");
        return;
    }
    if (out.producer != ReferenceFeatureProducer::Game) {
        logFail(testName, "reference feature cache producer mismatch");
        return;
    }
    if (out.analysisRevision < 1 || out.analysisRevision > 2) {
        logFail(testName, "reference feature cache revision mismatch");
        return;
    }
    if (out.pitch.notes.size() != 1 || out.pitch.notes.front().pitch != 440.0f) {
        logFail(testName, "reference feature pitch notes failed to round-trip");
        return;
    }
    if (out.timing.anchors.size() != 1
        || out.timing.anchors.front().anchorId != 1
        || out.timing.anchors.front().kind != ReferenceTimingAnchorKind::PitchTransition) {
        logFail(testName, "reference feature timing anchors failed to round-trip");
        return;
    }
    if (out.inputFingerprint != 42) {
        logFail(testName, "reference feature input fingerprint mismatch");
        return;
    }

    logPass(testName);
}

void runReferenceFeatures_InvalidateOnPitchCurveSet()
{
    constexpr const char* testName = "ReferenceFeatures_InvalidateOnPitchCurveSet";

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

    auto newCurve = std::make_shared<PitchCurve>();
    if (!store.setPitchCurve(matId, newCurve)) {
        logFail(testName, "setPitchCurve returned false");
        return;
    }

    ReferenceFeatureSet out;
    if (store.getReferenceFeatures(matId, out)) {
        logFail(testName, "reference feature cache should be invalid after setPitchCurve");
        return;
    }
    if (!isReferenceCacheCleared(out)) {
        logFail(testName, "reference feature cache should reset after setPitchCurve");
        return;
    }

    logPass(testName);
}

void runReferenceFeatures_InvalidateOnReplaceAudio()
{
    constexpr const char* testName = "ReferenceFeatures_InvalidateOnReplaceAudio";

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

    auto newBuffer = std::make_shared<juce::AudioBuffer<float>>(1, 256);
    newBuffer->clear();
    std::vector<SilentGap> gaps;
    if (!store.replaceAudio(matId, newBuffer, gaps)) {
        logFail(testName, "replaceAudio returned false");
        return;
    }

    ReferenceFeatureSet out;
    if (store.getReferenceFeatures(matId, out)) {
        logFail(testName, "reference feature cache should be invalid after replaceAudio");
        return;
    }
    if (!isReferenceCacheCleared(out)) {
        logFail(testName, "reference feature cache should reset after replaceAudio");
        return;
    }

    logPass(testName);
}

void runReferenceFeaturesCacheLifecycleSuite()
{
    logSection("ReferenceFeaturesCacheLifecycle");
    runReferenceFeatures_SetAndGet();
    runReferenceFeatures_InvalidateOnPitchCurveSet();
    runReferenceFeatures_InvalidateOnReplaceAudio();
}
