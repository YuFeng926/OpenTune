/**
 * Tests/TestReferenceAutoAlign.cpp - pure ReferenceAutoAlign request/patch contract tests.
 */

#include "TestSupport.h"
#include "DSP/ReferenceAutoAlign.h"
#include "Utils/TimeGrid.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace {

constexpr double kDurationSeconds = 3.0;

Note makeAlignNote(double startSeconds, double endSeconds, float pitchHz)
{
    Note note;
    note.startTime = startSeconds;
    note.endTime = endSeconds;
    note.pitch = pitchHz;
    note.originalPitch = pitchHz;
    note.retuneSpeed = 0.5f;
    note.vibratoDepth = 0.25f;
    note.vibratoRate = 5.5f;
    note.isVoiced = true;
    return note;
}

ReferenceTimingAnchor makeAnchor(uint64_t id,
                                 double sourceSeconds,
                                 ReferenceTimingAnchorKind kind = ReferenceTimingAnchorKind::Onset,
                                 float confidence = 0.8f)
{
    ReferenceTimingAnchor anchor;
    anchor.anchorId = id;
    anchor.sourceSeconds = sourceSeconds;
    anchor.strength = confidence;
    anchor.kind = kind;
    anchor.confidence = confidence;
    return anchor;
}

ReferenceFeatureSet makeReadyFeatures(std::vector<Note> notes,
                                      std::vector<ReferenceTimingAnchor> anchors)
{
    ReferenceFeatureSet features;
    features.status = ReferenceFeatureStatus::Ready;
    features.producer = ReferenceFeatureProducer::Game;
    features.analysisRevision = 1;
    features.inputFingerprint = 1;
    features.sourceDurationSeconds = kDurationSeconds;
    features.pitch.notes = std::move(notes);
    features.timing.anchors = std::move(anchors);
    return features;
}

std::shared_ptr<const TimeGridSnapshot> makeIdentityGrid()
{
    return TimeGridSnapshot::makeIdentity(kDurationSeconds);
}

std::shared_ptr<const TimeGridSnapshot> makeReferenceWarpGrid()
{
    std::vector<TimeHandle> handles;
    handles.push_back({ 1, 0.0, 0.0, HandleKind::ClipStart, true, Confidence::Default });
    handles.push_back({ 2, 1.0, 1.2, HandleKind::UserAdded, false, Confidence::Default });
    handles.push_back({ 3, kDurationSeconds, kDurationSeconds, HandleKind::ClipEnd, true, Confidence::Default });
    return TimeGridSnapshot::makeFromHandles(std::move(handles), 1);
}

std::shared_ptr<const TimeGridSnapshot> makeTargetWarpGrid()
{
    std::vector<TimeHandle> handles;
    handles.push_back({ 1, 0.0, 0.0, HandleKind::ClipStart, true, Confidence::Default });
    handles.push_back({ 2, 1.0, 0.8, HandleKind::UserAdded, false, Confidence::Default });
    handles.push_back({ 3, kDurationSeconds, kDurationSeconds, HandleKind::ClipEnd, true, Confidence::Default });
    return TimeGridSnapshot::makeFromHandles(std::move(handles), 1);
}

ReferenceAlignmentRequest makeRequest(ReferenceFeatureSet targetFeatures,
                                      ReferenceFeatureSet referenceFeatures,
                                      std::vector<Note> targetNotesBefore,
                                      std::vector<CorrectedSegment> targetSegmentsBefore = {},
                                      std::shared_ptr<const TimeGridSnapshot> targetGrid = makeIdentityGrid(),
                                      std::shared_ptr<const TimeGridSnapshot> referenceGrid = makeIdentityGrid())
{
    ReferenceAlignmentRequest request;
    request.target.placementId = 10;
    request.target.materializationId = 100;
    request.target.timelineStartSeconds = 0.0;
    request.target.timelineEndSeconds = kDurationSeconds;
    request.reference.placementId = 20;
    request.reference.materializationId = 200;
    request.reference.timelineStartSeconds = 0.0;
    request.reference.timelineEndSeconds = kDurationSeconds;
    request.targetTimeMap = EffectiveTimeMap::fromTimeGrid(targetGrid, kDurationSeconds);
    request.referenceTimeMap = EffectiveTimeMap::fromTimeGrid(referenceGrid, kDurationSeconds);
    request.targetFeatures = std::move(targetFeatures);
    request.referenceFeatures = std::move(referenceFeatures);
    request.targetNotesBefore = std::move(targetNotesBefore);
    request.targetSegmentsBefore = std::move(targetSegmentsBefore);
    request.targetTimeGridBefore = std::move(targetGrid);
    request.overlapStartTimelineSeconds = 0.0;
    request.overlapEndTimelineSeconds = kDurationSeconds;
    return request;
}

bool hasSegmentRange(const std::vector<CorrectedSegment>& segments, int startFrame, int endFrame)
{
    return std::any_of(segments.begin(), segments.end(), [startFrame, endFrame](const CorrectedSegment& segment) {
        return segment.startFrame == startFrame
            && segment.endFrame == endFrame
            && segment.source == CorrectedSegment::Source::NoteBased;
    });
}

} // namespace

void runReferenceAutoAlignUsesReferenceTauForwardTest()
{
    constexpr const char* testName = "ReferenceAutoAlign_UsesReferenceTauForward";

    auto targetFeatures = makeReadyFeatures(
        {},
        { makeAnchor(1, 0.4), makeAnchor(2, 1.0), makeAnchor(3, 2.0) });
    auto referenceFeatures = makeReadyFeatures(
        {},
        { makeAnchor(11, 0.4), makeAnchor(12, 1.0), makeAnchor(13, 2.0) });

    const auto request = makeRequest(std::move(targetFeatures),
                                     std::move(referenceFeatures),
                                     {},
                                     {},
                                     makeIdentityGrid(),
                                     makeReferenceWarpGrid());
    const auto patch = ReferenceAutoAlign::align(request);

    if (!patch.success) {
        logFail(testName, patch.diagnostics.toRawUTF8());
        return;
    }
    if (patch.timingIntents.empty()) {
        logFail(testName, "expected timing intent from warped reference grid");
        return;
    }

    const auto intent = std::find_if(patch.timingIntents.begin(),
                                     patch.timingIntents.end(),
                                     [](const TimeGridIntent& candidate) {
                                         return std::abs(candidate.targetSourceSeconds - 1.0) < 1.0e-9;
                                     });
    if (intent == patch.timingIntents.end()) {
        logFail(testName, "expected target anchor at 1.0s to produce an intent");
        return;
    }
    if (!approxEqual(intent->desiredOutputSeconds, 1.2, 1.0e-9)) {
        logFail(testName, "desired output should come from reference tauForward");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlignUsesTargetTauInverseTest()
{
    constexpr const char* testName = "ReferenceAutoAlign_UsesTargetTauInverse";

    const std::vector<Note> targetNotes = { makeAlignNote(1.0, 1.2, 220.0f) };
    auto targetFeatures = makeReadyFeatures(targetNotes, {});
    auto referenceFeatures = makeReadyFeatures({ makeAlignNote(0.75, 0.85, 440.0f) }, {});

    const auto request = makeRequest(std::move(targetFeatures),
                                     std::move(referenceFeatures),
                                     targetNotes,
                                     {},
                                     makeTargetWarpGrid(),
                                     makeIdentityGrid());
    const auto patch = ReferenceAutoAlign::align(request);

    if (!patch.success) {
        logFail(testName, patch.diagnostics.toRawUTF8());
        return;
    }
    if (patch.notesAfter.empty() || !approxEqual(patch.notesAfter.front().pitch, 440.0f, 1.0e-5f)) {
        logFail(testName, "target note should be matched through target tauInverse");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlignReturnsTimingIntentsNotTimeHandlesTest()
{
    constexpr const char* testName = "ReferenceAutoAlign_ReturnsTimingIntentsNotTimeHandles";

    auto targetFeatures = makeReadyFeatures(
        {},
        { makeAnchor(1, 0.4), makeAnchor(2, 1.0), makeAnchor(3, 2.0) });
    auto referenceFeatures = makeReadyFeatures(
        {},
        { makeAnchor(11, 0.4), makeAnchor(12, 1.2), makeAnchor(13, 2.0) });

    auto patch = ReferenceAutoAlign::align(
        makeRequest(std::move(targetFeatures), std::move(referenceFeatures), {}));

    if (!patch.success) {
        logFail(testName, patch.diagnostics.toRawUTF8());
        return;
    }
    if (!patch.timingChanged || patch.timingIntents.empty()) {
        logFail(testName, "align should return timing intents");
        return;
    }
    if (patch.timingIntents.front().targetSourceSeconds <= 0.0
        || patch.timingIntents.front().desiredOutputSeconds <= 0.0) {
        logFail(testName, "timing intent should contain source and desired output coordinates");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlignPreservesNonOverlapAndUnmatchedNotesTest()
{
    constexpr const char* testName = "ReferenceAutoAlign_PreservesNonOverlapAndUnmatchedNotes";

    const std::vector<Note> targetNotes = {
        makeAlignNote(0.20, 0.40, 220.0f),
        makeAlignNote(1.00, 1.20, 220.0f),
        makeAlignNote(2.50, 2.70, 220.0f)
    };
    auto targetFeatures = makeReadyFeatures(targetNotes, {});
    auto referenceFeatures = makeReadyFeatures({ makeAlignNote(1.00, 1.20, 440.0f) }, {});

    auto request = makeRequest(std::move(targetFeatures), std::move(referenceFeatures), targetNotes);
    request.overlapStartTimelineSeconds = 0.80;
    request.overlapEndTimelineSeconds = 1.40;
    const auto patch = ReferenceAutoAlign::align(request);

    if (!patch.success) {
        logFail(testName, patch.diagnostics.toRawUTF8());
        return;
    }
    if (patch.notesAfter.size() != targetNotes.size()) {
        logFail(testName, "patch should preserve full target note list");
        return;
    }
    if (!approxEqual(patch.notesAfter[0].pitch, 220.0f)
        || !approxEqual(patch.notesAfter[1].pitch, 440.0f)
        || !approxEqual(patch.notesAfter[2].pitch, 220.0f)) {
        logFail(testName, "non-overlap or unmatched notes were not preserved");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlignCorrectedSegmentsAreTargetLocalTest()
{
    constexpr const char* testName = "ReferenceAutoAlign_CorrectedSegmentsAreTargetLocal";

    const std::vector<Note> targetNotes = { makeAlignNote(1.00, 1.20, 220.0f) };
    auto targetFeatures = makeReadyFeatures(targetNotes, {});
    auto referenceFeatures = makeReadyFeatures({ makeAlignNote(1.00, 1.20, 440.0f) }, {});

    auto request = makeRequest(std::move(targetFeatures), std::move(referenceFeatures), targetNotes);
    request.target.timelineStartSeconds = 5.0;
    request.target.timelineEndSeconds = 8.0;
    request.reference.timelineStartSeconds = 5.0;
    request.reference.timelineEndSeconds = 8.0;
    request.overlapStartTimelineSeconds = 5.0;
    request.overlapEndTimelineSeconds = 8.0;

    const auto patch = ReferenceAutoAlign::align(request);

    if (!patch.success) {
        logFail(testName, patch.diagnostics.toRawUTF8());
        return;
    }
    if (!hasSegmentRange(patch.correctedSegmentsAfter, 100, 120)) {
        logFail(testName, "corrected segment frames should be target materialization local");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlignBoundedOneToOneNoteMatchingTest()
{
    constexpr const char* testName = "ReferenceAutoAlign_BoundedOneToOneNoteMatching";

    const std::vector<Note> targetNotes = { makeAlignNote(1.00, 1.20, 220.0f) };
    auto targetFeatures = makeReadyFeatures(targetNotes, {});
    auto referenceFeatures = makeReadyFeatures(
        { makeAlignNote(0.95, 1.05, 330.0f), makeAlignNote(1.05, 1.15, 440.0f) },
        {});

    const auto patch = ReferenceAutoAlign::align(
        makeRequest(std::move(targetFeatures), std::move(referenceFeatures), targetNotes));

    if (!patch.success) {
        logFail(testName, patch.diagnostics.toRawUTF8());
        return;
    }
    const auto noteBasedSegments = std::count_if(patch.correctedSegmentsAfter.begin(),
                                                 patch.correctedSegmentsAfter.end(),
                                                 [](const CorrectedSegment& segment) {
                                                     return segment.source == CorrectedSegment::Source::NoteBased;
                                                 });
    if (noteBasedSegments != 1) {
        logFail(testName, "one target note must not be matched by multiple reference notes");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlignTimeOnlyWhenReferenceNotesMissingTest()
{
    constexpr const char* testName = "ReferenceAutoAlign_TimeOnlyWhenReferenceNotesMissing";

    auto targetFeatures = makeReadyFeatures(
        { makeAlignNote(1.0, 1.2, 220.0f) },
        { makeAnchor(1, 0.4), makeAnchor(2, 1.0), makeAnchor(3, 2.0) });
    auto referenceFeatures = makeReadyFeatures(
        {},
        { makeAnchor(11, 0.4), makeAnchor(12, 1.2), makeAnchor(13, 2.0) });

    const auto patch = ReferenceAutoAlign::align(
        makeRequest(std::move(targetFeatures),
                    std::move(referenceFeatures),
                    { makeAlignNote(1.0, 1.2, 220.0f) }));

    if (!patch.success || patch.pitchChanged || !patch.timingChanged) {
        logFail(testName, "missing reference notes should still allow a time-only patch");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlignPitchOnlyWhenReferenceAnchorsMissingTest()
{
    constexpr const char* testName = "ReferenceAutoAlign_PitchOnlyWhenReferenceAnchorsMissing";

    auto targetFeatures = makeReadyFeatures({ makeAlignNote(1.0, 1.2, 220.0f) }, {});
    auto referenceFeatures = makeReadyFeatures({ makeAlignNote(1.0, 1.2, 440.0f) }, {});

    const auto patch = ReferenceAutoAlign::align(
        makeRequest(std::move(targetFeatures),
                    std::move(referenceFeatures),
                    { makeAlignNote(1.0, 1.2, 220.0f) }));

    if (!patch.success || !patch.pitchChanged || patch.timingChanged) {
        logFail(testName, "missing reference anchors should still allow a pitch-only patch");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlignFailsWhenBothNotesAndAnchorsMissingTest()
{
    constexpr const char* testName = "ReferenceAutoAlign_FailsWhenBothNotesAndAnchorsMissing";

    auto targetFeatures = makeReadyFeatures({}, {});
    auto referenceFeatures = makeReadyFeatures({}, {});

    const auto patch = ReferenceAutoAlign::align(
        makeRequest(std::move(targetFeatures), std::move(referenceFeatures), {}));

    if (patch.success || patch.error != AlignmentPatch::ErrorCode::InsufficientFeatures) {
        logFail(testName, "align should fail when both pitch and timing features are absent");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlignSuite()
{
    logSection("ReferenceAutoAlign");
    runReferenceAutoAlignUsesReferenceTauForwardTest();
    runReferenceAutoAlignUsesTargetTauInverseTest();
    runReferenceAutoAlignReturnsTimingIntentsNotTimeHandlesTest();
    runReferenceAutoAlignPreservesNonOverlapAndUnmatchedNotesTest();
    runReferenceAutoAlignCorrectedSegmentsAreTargetLocalTest();
    runReferenceAutoAlignBoundedOneToOneNoteMatchingTest();
    runReferenceAutoAlignTimeOnlyWhenReferenceNotesMissingTest();
    runReferenceAutoAlignPitchOnlyWhenReferenceAnchorsMissingTest();
    runReferenceAutoAlignFailsWhenBothNotesAndAnchorsMissingTest();
}
