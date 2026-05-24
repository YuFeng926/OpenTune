/**
 * Tests/TestMaterializationDerivedAnalysis.cpp — L3 tests for
 * MaterializationStore DerivedAnalysis slot lifecycle.
 *
 * Covers set/get, invalidation on pitch-curve change, and invalidation
 * on audio replacement per the Derived Analysis API contract.
 *
 * Suite aggregator: runMaterializationDerivedAnalysisSuite()
 */
#include "TestSupport.h"

#include <vector>

namespace {

// Helper: build a minimal DerivedAnalysis for a single "Ready" state
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

    MaterializationStore::DerivedAnalysis::TemporalEvent event;
    event.eventId = 1;
    event.sourceSeconds = 0.25;
    event.strength = 0.85f;
    event.kind = MaterializationStore::DerivedAnalysis::TemporalEventKind::PitchTransition;
    event.confidence = 0.85f;
    da.temporalEvents.push_back(event);

    da.inputFingerprint = 42;
    return da;
}

} // namespace

// ============================================================================
// Test 1: SetAndGetDerivedAnalysis
// ============================================================================
void runDerivedAnalysis_SetAndGet()
{
    constexpr const char* testName = "DerivedAnalysis_SetAndGet";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "createMaterialization returned 0");
        return;
    }

    const auto da = makeReadyAnalysis();

    // Precondition: no valid derived analysis initially
    if (store.hasValidDerivedAnalysis(matId)) {
        logFail(testName, "precondition: should NOT have valid derived analysis after import");
        return;
    }

    if (!store.setDerivedAnalysis(matId, da)) {
        logFail(testName, "setDerivedAnalysis returned false");
        return;
    }

    if (!store.hasValidDerivedAnalysis(matId)) {
        logFail(testName, "hasValidDerivedAnalysis should return true after set");
        return;
    }

    MaterializationStore::DerivedAnalysis out;
    if (!store.getDerivedAnalysis(matId, out)) {
        logFail(testName, "getDerivedAnalysis returned false after set");
        return;
    }

    if (out.state != F0ExtractionState::Ready) {
        logFail(testName, "round-trip: state mismatch");
        return;
    }
    if (out.analysisRevision < 1 || out.analysisRevision > 2) {
        logFail(testName, "round-trip: analysisRevision mismatch (store auto-bumps by 1)");
        return;
    }
    if (out.backendMode != 0) {
        logFail(testName, "round-trip: backendMode mismatch");
        return;
    }
    if (out.basicDerivedNotes.size() != 1) {
        logFail(testName, "round-trip: basicDerivedNotes count mismatch");
        return;
    }
    if (out.basicDerivedNotes.front().pitch != 440.0f) {
        logFail(testName, "round-trip: basicDerivedNotes pitch mismatch");
        return;
    }
    if (out.temporalEvents.size() != 1) {
        logFail(testName, "round-trip: temporalEvents count mismatch");
        return;
    }
    if (out.temporalEvents.front().eventId != 1
        || out.temporalEvents.front().strength != 0.85f
        || out.temporalEvents.front().kind
            != MaterializationStore::DerivedAnalysis::TemporalEventKind::PitchTransition) {
        logFail(testName, "round-trip: temporalEvents data mismatch");
        return;
    }
    if (out.inputFingerprint != 42) {
        logFail(testName, "round-trip: inputFingerprint mismatch");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 2: InvalidateOnPitchCurveSet
// ============================================================================
void runDerivedAnalysis_InvalidateOnPitchCurveSet()
{
    constexpr const char* testName = "DerivedAnalysis_InvalidateOnPitchCurveSet";

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

    // Now set a new PitchCurve — should invalidate derived analysis
    auto newCurve = std::make_shared<PitchCurve>();
    if (!store.setPitchCurve(matId, newCurve)) {
        logFail(testName, "setPitchCurve returned false");
        return;
    }

    if (store.hasValidDerivedAnalysis(matId)) {
        logFail(testName, "derived analysis should be invalid after setPitchCurve");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 3: InvalidateOnReplaceAudio
// ============================================================================
void runDerivedAnalysis_InvalidateOnReplaceAudio()
{
    constexpr const char* testName = "DerivedAnalysis_InvalidateOnReplaceAudio";

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

    // Replace audio — should invalidate derived analysis
    auto newBuffer = std::make_shared<juce::AudioBuffer<float>>(1, 256);
    newBuffer->clear();
    std::vector<SilentGap> gaps;

    if (!store.replaceAudio(matId, newBuffer, gaps)) {
        logFail(testName, "replaceAudio returned false");
        return;
    }

    if (store.hasValidDerivedAnalysis(matId)) {
        logFail(testName, "derived analysis should be invalid after replaceAudio");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite entry point
// ============================================================================
void runMaterializationDerivedAnalysisSuite()
{
    logSection("MaterializationDerivedAnalysis");
    runDerivedAnalysis_SetAndGet();
    runDerivedAnalysis_InvalidateOnPitchCurveSet();
    runDerivedAnalysis_InvalidateOnReplaceAudio();
}
