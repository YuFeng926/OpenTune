/**
 * Tests/TestAutoRefFailure.cpp — Failure-mode tests for Source/DSP/ReferenceAutoAlign.h
 *
 * Covers:
 *   - NoOverlap error when placements don't overlap in time
 *   - Reference/target analysis not ready errors
 *   - InsufficientFeatures error
 *   - Target analysis is not modified on failure
 */

#include "TestSupport.h"
#include "DSP/ReferenceAutoAlign.h"
#include "Utils/TimeGrid.h"

#include <utility>

namespace {

MaterializationStore::DerivedAnalysis makeReadyAnalysis()
{
    MaterializationStore::DerivedAnalysis da;
    da.state = F0ExtractionState::Ready;
    da.analysisRevision = 1;
    Note note;
    note.startTime = 0.1; note.endTime = 0.3; note.pitch = 440.0f;
    note.originalPitch = 440.0f; note.isVoiced = true;
    da.basicDerivedNotes.push_back(note);
    return da;
}

MaterializationStore::DerivedAnalysis makeReadyAnalysisWithTemporalEvents(int numEvents)
{
    MaterializationStore::DerivedAnalysis da = makeReadyAnalysis();
    for (int i = 0; i < numEvents; ++i) {
        MaterializationStore::DerivedAnalysis::TemporalEvent event;
        event.eventId = static_cast<uint64_t>(i + 1);
        event.sourceSeconds = static_cast<double>(i + 1) * TimeGridSnapshot::kMinSourceSpacingSeconds;
        event.strength = 0.8f;
        event.kind = MaterializationStore::DerivedAnalysis::TemporalEventKind::Onset;
        event.confidence = 0.8f;
        da.temporalEvents.push_back(event);
    }
    return da;
}

ReferenceAlignmentRequest makeRequest(MaterializationStore::DerivedAnalysis target,
                                      MaterializationStore::DerivedAnalysis reference,
                                      double overlapStartTimelineSeconds,
                                      double overlapEndTimelineSeconds,
                                      double targetTimelineStartSeconds,
                                      double referenceTimelineStartSeconds,
                                      double targetTotalDurationSeconds)
{
    ReferenceAlignmentRequest request;
    request.target.materializationId = 1001;
    request.reference.materializationId = 2001;
    request.target.timelineStartSeconds = targetTimelineStartSeconds;
    request.target.timelineEndSeconds = targetTimelineStartSeconds + targetTotalDurationSeconds;
    request.reference.timelineStartSeconds = referenceTimelineStartSeconds;
    request.reference.timelineEndSeconds = referenceTimelineStartSeconds + targetTotalDurationSeconds;
    request.target.timeGrid = TimeGridSnapshot::makeIdentity(targetTotalDurationSeconds);
    request.reference.timeGrid = TimeGridSnapshot::makeIdentity(targetTotalDurationSeconds);
    request.targetFeatures = std::move(target);
    request.referenceFeatures = std::move(reference);
    request.targetNotesBefore = request.targetFeatures.basicDerivedNotes;
    request.overlapStartTimelineSeconds = overlapStartTimelineSeconds;
    request.overlapEndTimelineSeconds = overlapEndTimelineSeconds;
    return request;
}

} // namespace

// ============================================================================
// Test 1: No overlap → NoOverlap error
// ============================================================================

void runAutoRefFailureNoOverlapTest()
{
    constexpr const char* testName = "AutoRefFailure_NoOverlapReturnsError";

    auto target = makeReadyAnalysisWithTemporalEvents(3);
    auto reference = makeReadyAnalysisWithTemporalEvents(3);

    // overlapEndSeconds <= overlapStartSeconds → NoOverlap
    const auto request = makeRequest(
        std::move(target), std::move(reference),
        /*overlapStartSeconds=*/1.0,
        /*overlapEndSeconds=*/0.5,
        /*targetTimelineStartSeconds=*/0.0,
        /*referenceTimelineStartSeconds=*/2.0,
        /*targetTotalDurationSeconds=*/2.0);
    const auto result = ReferenceAutoAlign::align(request);

    if (result.success) {
        logFail(testName, "align should fail with non-overlapping placements");
        return;
    }
    if (result.error != AlignmentPatch::ErrorCode::NoOverlap) {
        logFail(testName, "error code should be NoOverlap");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 2: Reference not ready → ReferenceAnalysisNotReady
//
// align() checks reference state BEFORE target state (see
// ReferenceAutoAlign.cpp precondition order). When target is Ready
// but reference is not, the correct error is ReferenceAnalysisNotReady.
// ============================================================================

void runAutoRefFailureNoReferenceTest()
{
    constexpr const char* testName = "AutoRefFailure_NoReferenceReturnsError";

    auto target = makeReadyAnalysisWithTemporalEvents(3);
    // target has notes + 3 temporal events, state=Ready

    // reference: has notes + 3 temporal events but state=NotRequested
    auto referenceNotReady = makeReadyAnalysisWithTemporalEvents(3);
    referenceNotReady.state = F0ExtractionState::NotRequested;

    const auto request = makeRequest(
        std::move(target), std::move(referenceNotReady),
        0.0, 1.0,
        0.0, 0.0,
        1.0);
    const auto result = ReferenceAutoAlign::align(request);

    if (result.success) {
        logFail(testName, "align should fail when reference analysis is not ready");
        return;
    }
    // align() checks reference state first → precise assertion
    if (result.error != AlignmentPatch::ErrorCode::ReferenceAnalysisNotReady) {
        logFail(testName, "error code should be ReferenceAnalysisNotReady (checked before target)");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 2b: Target not ready → TargetAnalysisNotReady
// ============================================================================

void runAutoRefFailureTargetNotReadyTest()
{
    constexpr const char* testName = "AutoRefFailure_TargetNotReadyReturnsError";

    // target has notes + 3 temporal events but state=NotRequested
    auto targetNotReady = makeReadyAnalysisWithTemporalEvents(3);
    targetNotReady.state = F0ExtractionState::NotRequested;
    
    auto reference = makeReadyAnalysisWithTemporalEvents(3);

    const auto request = makeRequest(
        std::move(targetNotReady), std::move(reference),
        0.0, 1.0,
        0.0, 0.0,
        1.0);
    const auto result = ReferenceAutoAlign::align(request);

    if (result.success) {
        logFail(testName, "align should fail when target analysis is not ready");
        return;
    }
    if (result.error != AlignmentPatch::ErrorCode::TargetAnalysisNotReady) {
        logFail(testName, "error code should be TargetAnalysisNotReady");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 3: Insufficient features.
// ============================================================================

void runAutoRefFailureInsufficientFeaturesTest()
{
    constexpr const char* testName = "AutoRefFailure_InsufficientFeatures";

    auto target = makeReadyAnalysisWithTemporalEvents(3);
    target.basicDerivedNotes.clear();
    auto reference = makeReadyAnalysisWithTemporalEvents(1);
    reference.basicDerivedNotes.clear();

    const auto request = makeRequest(
        std::move(target), std::move(reference),
        0.0, 2.0,
        0.0, 0.0,
        2.0);
    const auto result = ReferenceAutoAlign::align(request);

    if (result.success) {
        logFail(testName, "align should fail with insufficient features");
        return;
    }
    if (result.error != AlignmentPatch::ErrorCode::InsufficientFeatures) {
        logFail(testName, "error code should be InsufficientFeatures");
        return;
    }

    logPass(testName);
}

void runAutoRefFailureMissingTimeGridTest()
{
    constexpr const char* testName = "AutoRefFailure_MissingTimeGridReturnsInvalidTimeGrid";

    auto target = makeReadyAnalysisWithTemporalEvents(3);
    auto reference = makeReadyAnalysisWithTemporalEvents(3);

    auto request = makeRequest(
        std::move(target), std::move(reference),
        0.0, 1.0,
        0.0, 0.0,
        1.0);
    request.target.timeGrid.reset();

    const auto result = ReferenceAutoAlign::align(request);

    if (result.success) {
        logFail(testName, "align should fail when target TimeGrid is missing");
        return;
    }
    if (result.error != AlignmentPatch::ErrorCode::TimeGridInvalid) {
        logFail(testName, "error code should be TimeGridInvalid");
        return;
    }

    request.target.timeGrid = TimeGridSnapshot::makeIdentity(1.0);
    request.reference.timeGrid.reset();
    const auto referenceResult = ReferenceAutoAlign::align(request);
    if (referenceResult.success) {
        logFail(testName, "align should fail when reference TimeGrid is missing");
        return;
    }
    if (referenceResult.error != AlignmentPatch::ErrorCode::TimeGridInvalid) {
        logFail(testName, "reference-missing error code should be TimeGridInvalid");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 4: Target analysis is not modified on failure
// ============================================================================

void runAutoRefFailureDoesNotModifyOutputTest()
{
    constexpr const char* testName = "AutoRefFailure_DoesNotModifyOutputOnFailure";

    auto target = makeReadyAnalysisWithTemporalEvents(2);
    auto reference = makeReadyAnalysisWithTemporalEvents(2);

    // Save copies of key fields before calling align
    const size_t notesBefore = target.basicDerivedNotes.size();
    const size_t eventsBefore = target.temporalEvents.size();
    const auto revBefore = target.analysisRevision;

    // Call with non-overlapping params to trigger failure
    const auto request = makeRequest(
        target, reference,
        0.0, 1.0,
        0.0, 2.0,
        1.0);
    const auto result = ReferenceAutoAlign::align(request);

    // Verify failure
    if (result.success) {
        logFail(testName, "align was expected to fail but succeeded");
        return;
    }

    // Verify targetAnalysis was not mutated
    if (target.basicDerivedNotes.size() != notesBefore) {
        logFail(testName, "target basicDerivedNotes was modified on failure");
        return;
    }
    if (target.temporalEvents.size() != eventsBefore) {
        logFail(testName, "target temporalEvents was modified on failure");
        return;
    }
    if (target.analysisRevision != revBefore) {
        logFail(testName, "target analysisRevision was modified on failure");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runAutoRefFailureSuite()
{
    logSection("AutoRefFailure");
    runAutoRefFailureNoOverlapTest();
    runAutoRefFailureNoReferenceTest();
    runAutoRefFailureTargetNotReadyTest();
    runAutoRefFailureInsufficientFeaturesTest();
    runAutoRefFailureMissingTimeGridTest();
    runAutoRefFailureDoesNotModifyOutputTest();
}
