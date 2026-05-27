/**
 * Tests/TestAutoRefFailure.cpp - failure and edge-case tests for ReferenceAutoAlign.
 */

#include "TestSupport.h"
#include "DSP/ReferenceAutoAlign.h"
#include "Utils/TimeGrid.h"

#include <utility>
#include <vector>

namespace {

Note makeNote(double startSeconds, double endSeconds, float pitchHz)
{
    Note note;
    note.startTime = startSeconds;
    note.endTime = endSeconds;
    note.pitch = pitchHz;
    note.originalPitch = pitchHz;
    note.isVoiced = true;
    return note;
}

ReferenceTimingAnchor makeAnchor(uint64_t id, double sourceSeconds, float confidence = 0.8f)
{
    ReferenceTimingAnchor anchor;
    anchor.anchorId = id;
    anchor.sourceSeconds = sourceSeconds;
    anchor.strength = confidence;
    anchor.kind = ReferenceTimingAnchorKind::Onset;
    anchor.confidence = confidence;
    return anchor;
}

ReferenceFeatureSet makeFeatures(std::vector<Note> notes,
                                 std::vector<ReferenceTimingAnchor> anchors,
                                 ReferenceFeatureStatus status = ReferenceFeatureStatus::Ready)
{
    ReferenceFeatureSet features;
    features.status = status;
    features.producer = ReferenceFeatureProducer::Game;
    features.analysisRevision = 1;
    features.inputFingerprint = 1;
    features.sourceDurationSeconds = 2.0;
    features.pitch.notes = std::move(notes);
    features.timing.anchors = std::move(anchors);
    return features;
}

ReferenceAlignmentRequest makeRequest(ReferenceFeatureSet target,
                                      ReferenceFeatureSet reference,
                                      double overlapStartTimelineSeconds,
                                      double overlapEndTimelineSeconds,
                                      double targetTimelineStartSeconds,
                                      double referenceTimelineStartSeconds,
                                      double targetDurationSeconds,
                                      double referenceDurationSeconds = 0.0)
{
    const double referenceDuration = referenceDurationSeconds > 0.0
        ? referenceDurationSeconds
        : targetDurationSeconds;

    ReferenceAlignmentRequest request;
    request.target.materializationId = 1001;
    request.reference.materializationId = 2001;
    request.target.timelineStartSeconds = targetTimelineStartSeconds;
    request.target.timelineEndSeconds = targetTimelineStartSeconds + targetDurationSeconds;
    request.reference.timelineStartSeconds = referenceTimelineStartSeconds;
    request.reference.timelineEndSeconds = referenceTimelineStartSeconds + referenceDuration;
    request.targetTimeMap = EffectiveTimeMap::identity(targetDurationSeconds);
    request.referenceTimeMap = EffectiveTimeMap::identity(referenceDuration);
    request.targetFeatures = std::move(target);
    request.referenceFeatures = std::move(reference);
    request.targetNotesBefore = request.targetFeatures.pitch.notes;
    request.targetTimeGridBefore = TimeGridSnapshot::makeIdentity(targetDurationSeconds);
    request.overlapStartTimelineSeconds = overlapStartTimelineSeconds;
    request.overlapEndTimelineSeconds = overlapEndTimelineSeconds;
    return request;
}

} // namespace

void runAutoRefFailureNoOverlapTest()
{
    constexpr const char* testName = "AutoRefFailure_NoOverlapReturnsError";

    auto target = makeFeatures({ makeNote(0.1, 0.3, 440.0f) },
                               { makeAnchor(1, 0.2), makeAnchor(2, 0.8), makeAnchor(3, 1.4) });
    auto reference = makeFeatures({ makeNote(0.1, 0.3, 440.0f) },
                                  { makeAnchor(11, 0.2), makeAnchor(12, 0.8), makeAnchor(13, 1.4) });

    const auto request = makeRequest(
        std::move(target), std::move(reference),
        /*overlapStartTimelineSeconds=*/1.0,
        /*overlapEndTimelineSeconds=*/0.5,
        /*targetTimelineStartSeconds=*/0.0,
        /*referenceTimelineStartSeconds=*/2.0,
        /*targetDurationSeconds=*/2.0);
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

void runAutoRefFailureTargetNotReadyTest()
{
    constexpr const char* testName = "AutoRefFailure_TargetNotReadyReturnsError";

    auto target = makeFeatures({ makeNote(0.1, 0.3, 440.0f) },
                               { makeAnchor(1, 0.2), makeAnchor(2, 0.8) },
                               ReferenceFeatureStatus::NotRequested);
    auto reference = makeFeatures({ makeNote(0.1, 0.3, 440.0f) },
                                  { makeAnchor(11, 0.2), makeAnchor(12, 0.8) });

    const auto result = ReferenceAutoAlign::align(
        makeRequest(std::move(target), std::move(reference), 0.0, 1.0, 0.0, 0.0, 1.0));

    if (result.success) {
        logFail(testName, "align should fail when target features are not ready");
        return;
    }
    if (result.error != AlignmentPatch::ErrorCode::TargetAnalysisNotReady) {
        logFail(testName, "error code should be TargetAnalysisNotReady");
        return;
    }

    logPass(testName);
}

void runAutoRefFailureReferenceNotReadyTest()
{
    constexpr const char* testName = "AutoRefFailure_ReferenceNotReadyReturnsError";

    auto target = makeFeatures({ makeNote(0.1, 0.3, 440.0f) },
                               { makeAnchor(1, 0.2), makeAnchor(2, 0.8) });
    auto reference = makeFeatures({ makeNote(0.1, 0.3, 440.0f) },
                                  { makeAnchor(11, 0.2), makeAnchor(12, 0.8) },
                                  ReferenceFeatureStatus::NotRequested);

    const auto result = ReferenceAutoAlign::align(
        makeRequest(std::move(target), std::move(reference), 0.0, 1.0, 0.0, 0.0, 1.0));

    if (result.success) {
        logFail(testName, "align should fail when reference features are not ready");
        return;
    }
    if (result.error != AlignmentPatch::ErrorCode::ReferenceAnalysisNotReady) {
        logFail(testName, "error code should be ReferenceAnalysisNotReady");
        return;
    }

    logPass(testName);
}

void runAutoRefFailureInsufficientFeaturesTest()
{
    constexpr const char* testName = "AutoRefFailure_InsufficientFeatures";

    auto target = makeFeatures({}, {});
    auto reference = makeFeatures({}, {});

    const auto result = ReferenceAutoAlign::align(
        makeRequest(std::move(target), std::move(reference), 0.0, 2.0, 0.0, 0.0, 2.0));

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

void runAutoRefFailureMissingTargetTimeGridTest()
{
    constexpr const char* testName = "AutoRefFailure_MissingTargetTimeGridReturnsInvalidTimeGrid";

    auto target = makeFeatures({}, { makeAnchor(1, 0.4), makeAnchor(2, 1.0), makeAnchor(3, 1.6) });
    auto reference = makeFeatures({}, { makeAnchor(11, 0.4), makeAnchor(12, 1.2), makeAnchor(13, 1.6) });

    auto request = makeRequest(std::move(target), std::move(reference), 0.0, 2.0, 0.0, 0.0, 2.0);
    request.targetTimeGridBefore.reset();

    const auto result = ReferenceAutoAlign::align(request);

    if (result.success) {
        logFail(testName, "align should fail when timing features exist but target TimeGrid is missing");
        return;
    }
    if (result.error != AlignmentPatch::ErrorCode::TimeGridInvalid) {
        logFail(testName, "error code should be TimeGridInvalid");
        return;
    }

    logPass(testName);
}

void runAutoRefFailureReferenceIdentityTimeMapIsAllowedTest()
{
    constexpr const char* testName = "AutoRefFailure_ReferenceIdentityTimeMapIsAllowed";

    auto target = makeFeatures({}, { makeAnchor(1, 0.4), makeAnchor(2, 1.0), makeAnchor(3, 1.6) });
    auto reference = makeFeatures({}, { makeAnchor(11, 0.4), makeAnchor(12, 1.2), makeAnchor(13, 1.6) });

    auto request = makeRequest(std::move(target), std::move(reference), 0.0, 2.0, 0.0, 0.0, 2.0);
    request.referenceTimeMap = EffectiveTimeMap::identity(2.0);

    const auto result = ReferenceAutoAlign::align(request);

    if (!result.success || !result.timingChanged) {
        logFail(testName, "reference clip without explicit TimeGrid should still allow timing alignment");
        return;
    }

    logPass(testName);
}

void runAutoRefFailureNoMutationWhenAlreadyAlignedTest()
{
    constexpr const char* testName = "AutoRefFailure_NoMutationWhenAlreadyAligned";

    const std::vector<Note> alignedNotes = { makeNote(0.4, 0.8, 440.0f) };
    auto target = makeFeatures(alignedNotes, {});
    auto reference = makeFeatures(alignedNotes, {});
    auto request = makeRequest(std::move(target), std::move(reference), 0.0, 1.5, 0.0, 0.0, 2.0);

    CorrectedSegment existingSegment;
    existingSegment.startFrame = 40;
    existingSegment.endFrame = 80;
    existingSegment.source = CorrectedSegment::Source::NoteBased;
    request.targetSegmentsBefore.push_back(existingSegment);

    const auto result = ReferenceAutoAlign::align(request);

    if (result.success) {
        logFail(testName, "align should report NoMutation when notes and note-based segments already match");
        return;
    }
    if (result.error != AlignmentPatch::ErrorCode::NoMutation) {
        logFail(testName, "error code should be NoMutation");
        return;
    }

    logPass(testName);
}

void runAutoRefFailureSuite()
{
    logSection("AutoRefFailure");
    runAutoRefFailureNoOverlapTest();
    runAutoRefFailureTargetNotReadyTest();
    runAutoRefFailureReferenceNotReadyTest();
    runAutoRefFailureInsufficientFeaturesTest();
    runAutoRefFailureMissingTargetTimeGridTest();
    runAutoRefFailureReferenceIdentityTimeMapIsAllowedTest();
    runAutoRefFailureNoMutationWhenAlreadyAlignedTest();
}
