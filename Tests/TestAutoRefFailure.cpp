/**
 * Tests/TestAutoRefFailure.cpp — Failure-mode tests for Source/DSP/ReferenceAutoAlign.h
 *
 * Covers:
 *   - NoOverlap error when placements don't overlap in time
 *   - Reference/target analysis not ready errors
 *   - InsufficientAnchors error
 *   - Target analysis is not modified on failure
 */

#include "TestSupport.h"
#include "DSP/ReferenceAutoAlign.h"

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

MaterializationStore::DerivedAnalysis makeReadyAnalysisWithAnchors(int numAnchors)
{
    MaterializationStore::DerivedAnalysis da = makeReadyAnalysis();
    for (int i = 0; i < numAnchors; ++i) {
        MaterializationStore::DerivedAnalysis::TimeAnchor anchor;
        anchor.id = static_cast<uint64_t>(i + 1);
        anchor.sourceSeconds = static_cast<double>(i) * 0.1;
        anchor.strength = 0.8f;
        da.basicDerivedAnchors.push_back(anchor);
    }
    return da;
}

} // namespace

// ============================================================================
// Test 1: No overlap → NoOverlap error
// ============================================================================

void runAutoRefFailureNoOverlapTest()
{
    constexpr const char* testName = "AutoRefFailure_NoOverlapReturnsError";

    auto target = makeReadyAnalysisWithAnchors(3);
    auto reference = makeReadyAnalysisWithAnchors(3);

    // overlapEndSeconds <= overlapStartSeconds → NoOverlap
    const auto result = ReferenceAutoAlign::align(
        target, reference,
        /*overlapStartSeconds=*/1.0,
        /*overlapEndSeconds=*/0.5,
        /*targetTimelineStartSeconds=*/0.0,
        /*referenceTimelineStartSeconds=*/2.0,
        /*targetTotalDurationSeconds=*/2.0);

    if (result.success) {
        logFail(testName, "align should fail with non-overlapping placements");
        return;
    }
    if (result.error != AlignResult::ErrorCode::NoOverlap) {
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

    auto target = makeReadyAnalysisWithAnchors(3);
    // target has notes + 3 anchors, state=Ready

    // reference: has notes + 3 anchors but state=NotRequested
    auto referenceNotReady = makeReadyAnalysisWithAnchors(3);
    referenceNotReady.state = F0ExtractionState::NotRequested;

    const auto result = ReferenceAutoAlign::align(
        target, referenceNotReady,
        0.0, 1.0,
        0.0, 0.0,
        1.0);

    if (result.success) {
        logFail(testName, "align should fail when reference analysis is not ready");
        return;
    }
    // align() checks reference state first → precise assertion
    if (result.error != AlignResult::ErrorCode::ReferenceAnalysisNotReady) {
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

    // target has notes + 3 anchors but state=NotRequested
    auto targetNotReady = makeReadyAnalysisWithAnchors(3);
    targetNotReady.state = F0ExtractionState::NotRequested;
    
    auto reference = makeReadyAnalysisWithAnchors(3);

    const auto result = ReferenceAutoAlign::align(
        targetNotReady, reference,
        0.0, 1.0,
        0.0, 0.0,
        1.0);

    if (result.success) {
        logFail(testName, "align should fail when target analysis is not ready");
        return;
    }
    if (result.error != AlignResult::ErrorCode::TargetAnalysisNotReady) {
        logFail(testName, "error code should be TargetAnalysisNotReady");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 3: Insufficient anchors → InsufficientAnchors error
// ============================================================================

void runAutoRefFailureInsufficientAnchorsTest()
{
    constexpr const char* testName = "AutoRefFailure_InsufficientAnchors";

    auto target = makeReadyAnalysisWithAnchors(3);
    auto reference = makeReadyAnalysisWithAnchors(1);  // only 1 anchor

    const auto result = ReferenceAutoAlign::align(
        target, reference,
        0.0, 2.0,
        0.0, 0.0,
        2.0);

    if (result.success) {
        logFail(testName, "align should fail with insufficient anchors");
        return;
    }
    if (result.error != AlignResult::ErrorCode::InsufficientAnchors) {
        logFail(testName, "error code should be InsufficientAnchors");
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

    auto target = makeReadyAnalysisWithAnchors(2);
    auto reference = makeReadyAnalysisWithAnchors(2);

    // Save copies of key fields before calling align
    const size_t notesBefore = target.basicDerivedNotes.size();
    const size_t anchorsBefore = target.basicDerivedAnchors.size();
    const auto revBefore = target.analysisRevision;

    // Call with non-overlapping params to trigger failure
    const auto result = ReferenceAutoAlign::align(
        target, reference,
        0.0, 1.0,
        0.0, 2.0,
        1.0);

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
    if (target.basicDerivedAnchors.size() != anchorsBefore) {
        logFail(testName, "target basicDerivedAnchors was modified on failure");
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
    runAutoRefFailureInsufficientAnchorsTest();
    runAutoRefFailureDoesNotModifyOutputTest();
}
