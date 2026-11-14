#include "ContentEditCommandsStub.h"
#include "Content/EditableContentSnapshot.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/PianoRollNotePatchAction.h"
#include "Utils/SelectedNoteDeleteEdit.h"
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace OpenTune;
using namespace OpenTune::TestSupport;

Note makeNote(const F0Timeline& timeline, int startFrame, int endFrameExclusive, float pitch)
{
    Note note;
    note.startTime = timeline.timeAtFrame(startFrame);
    note.endTime = timeline.timeAtFrame(endFrameExclusive);
    note.pitch = pitch;
    note.originalPitch = pitch;
    return note;
}

bool sameNote(const Note& left, const Note& right)
{
    return left.startTime == right.startTime
        && left.endTime == right.endTime
        && left.pitch == right.pitch
        && left.originalPitch == right.originalPitch
        && left.dirty == right.dirty
        && left.eq == right.eq;
}

bool sameNotes(const std::vector<Note>& left, const std::vector<Note>& right)
{
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (!sameNote(left[i], right[i]))
            return false;
    }
    return true;
}

bool sameSegment(const PitchCorrectionSegment& left, const PitchCorrectionSegment& right)
{
    return left.startFrame == right.startFrame
        && left.endFrame == right.endFrame
        && left.f0Data == right.f0Data
        && left.baseF0Data == right.baseF0Data
        && left.source == right.source;
}

bool sameSegments(const std::vector<PitchCorrectionSegment>& left,
                  const std::vector<PitchCorrectionSegment>& right)
{
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (!sameSegment(left[i], right[i]))
            return false;
    }
    return true;
}

bool samePatch(const ContentNoteRangePatch& left, const ContentNoteRangePatch& right)
{
    return left.affectedRange.startSeconds == right.affectedRange.startSeconds
        && left.affectedRange.endSeconds == right.affectedRange.endSeconds
        && sameNotes(left.afterNotesInRange, right.afterNotesInRange);
}

EditableContentSnapshot makeSnapshot(bool withCurve)
{
    EditableContentSnapshot snapshot;
    snapshot.notesRevision = 12;
    const F0Timeline timeline(10, 1000.0, 100);
    auto first = makeNote(timeline, 10, 20, 220.0f);
    first.dirty = true;
    auto middle = makeNote(timeline, 22, 30, 246.94f);
    middle.dirty = true;
    auto third = makeNote(timeline, 40, 50, 261.63f);
    third.dirty = true;
    auto last = makeNote(timeline, 52, 60, 293.66f);
    snapshot.notes = { first, middle, third, last };

    if (withCurve) {
        std::vector<float> originalF0(100, 220.0f);
        std::vector<float> originalEnergy(100, 1.0f);
        std::vector<float> correctedF0(70);
        std::vector<float> baseF0(70, 210.0f);
        for (size_t i = 0; i < correctedF0.size(); ++i)
            correctedF0[i] = 300.0f + static_cast<float>(i);
        PitchCorrectionSegment segment(
            0, 70, std::move(correctedF0), PitchCorrectionSegment::Source::HandDraw);
        segment.baseF0Data = std::move(baseF0);
        snapshot.pitchCurve = std::make_shared<const PitchCurveSnapshot>(
            std::move(originalF0), std::move(originalEnergy),
            std::vector<PitchCorrectionSegment>{ std::move(segment) }, 10, 1000.0);
    }
    return snapshot;
}

bool checkTopologyDelete()
{
    const auto snapshot = makeSnapshot(false);
    const auto plan = planSelectedNoteDelete(snapshot, { 0, 2 }, snapshot.notesRevision);
    const auto* edit = std::get_if<SelectedNoteTopologyDelete>(&plan.edit);
    if (plan.status != SelectedNoteDeleteStatus::Ready || edit == nullptr
        || edit->beforePatch.affectedRange.startSeconds != snapshot.notes[0].startTime
        || edit->beforePatch.affectedRange.endSeconds != snapshot.notes[2].endTime
        || edit->beforePatch.afterNotesInRange.size() != 3
        || !sameNotes(edit->beforePatch.afterNotesInRange,
                      { snapshot.notes[0], snapshot.notes[1], snapshot.notes[2] })
        || edit->afterPatch.afterNotesInRange.size() != 1
        || !sameNote(edit->afterPatch.afterNotesInRange[0], snapshot.notes[1]))
        return false;

    const auto finalNotes = mergeNotesRange(
        snapshot.notes, edit->afterPatch.affectedRange, edit->afterPatch.afterNotesInRange);
    return sameNotes(finalNotes, { snapshot.notes[1], snapshot.notes[3] });
}

bool checkPitchCurveDelete()
{
    const auto snapshot = makeSnapshot(true);
    const auto plan = planSelectedNoteDelete(snapshot, { 0, 2 }, snapshot.notesRevision);
    const auto* edit = std::get_if<SelectedNotePitchCurveDelete>(&plan.edit);
    if (plan.status != SelectedNoteDeleteStatus::Ready || edit == nullptr
        || edit->affectedRange.startFrame != 10
        || edit->affectedRange.endFrameExclusive != 50
        || !sameNotes(edit->beforeNotes, { snapshot.notes[0], snapshot.notes[1], snapshot.notes[2] })
        || !sameNotes(edit->afterNotes, { snapshot.notes[1] })
        || edit->beforeSegments.size() != 1
        || edit->beforeSegments[0].startFrame != 10
        || edit->beforeSegments[0].endFrame != 50
        || edit->beforeSegments[0].f0Data.size() != 40
        || edit->afterSegments.size() != 1
        || edit->afterSegments[0].startFrame != 20
        || edit->afterSegments[0].endFrame != 40
        || edit->afterSegments[0].f0Data.size() != 20
        || edit->afterSegments[0].f0Data.front() != 320.0f
        || edit->afterSegments[0].f0Data.back() != 339.0f
        || edit->afterSegments[0].baseF0Data.size() != 70)
        return false;

    const F0Timeline timeline(10, 1000.0, 100);
    const auto adjacent = makeNote(timeline, 20, 22, 300.0f);
    auto adjacentSnapshot = snapshot;
    adjacentSnapshot.notes = { snapshot.notes[0], adjacent };
    const auto adjacentPlan = planSelectedNoteDelete(
        adjacentSnapshot, { 0, 1 }, adjacentSnapshot.notesRevision);
    const auto* adjacentEdit = std::get_if<SelectedNotePitchCurveDelete>(&adjacentPlan.edit);
    return adjacentPlan.status == SelectedNoteDeleteStatus::Ready
        && adjacentEdit != nullptr
        && adjacentEdit->affectedRange.startFrame == 10
        && adjacentEdit->affectedRange.endFrameExclusive == 22;
}

bool checkNoTargetAndInvalidInput()
{
    const auto snapshot = makeSnapshot(false);
    return planSelectedNoteDelete(snapshot, {}, snapshot.notesRevision).status
               == SelectedNoteDeleteStatus::NoTarget
        && planSelectedNoteDelete(snapshot, { 0 }, snapshot.notesRevision + 1).status
               == SelectedNoteDeleteStatus::StaleRevision
        && planSelectedNoteDelete(snapshot, { 0, 8 }, snapshot.notesRevision).status
               == SelectedNoteDeleteStatus::InvalidSelection
        && planSelectedNoteDelete(snapshot, { -1 }, snapshot.notesRevision).status
               == SelectedNoteDeleteStatus::InvalidSelection;
}

class RecordingCommands final : public ContentEditCommandsStub
{
public:
    struct SegmentCommit
    {
        ContentKey key;
        std::vector<Note> notes;
        std::vector<PitchCorrectionSegment> segments;
        ContentEditRangeFrames range;
    };

    ContentKey lastKey;
    std::vector<ContentNoteRangePatch> notePatches;
    std::vector<SegmentCommit> segmentCommits;

    bool replaceContentNotesForFullMutation(ContentKey, std::vector<Note>) override { return false; }
    ContentCommitSnapshot commitNoteTopologyPatch(ContentKey key, ContentNoteRangePatch patch) override
    {
        lastKey = key;
        notePatches.push_back(std::move(patch));
        return {};
    }
    ContentCommitSnapshot commitNotesAndSegments(
        ContentKey key, std::vector<Note> notes,
        std::vector<PitchCorrectionSegment> segments, ContentEditRangeFrames range) override
    {
        lastKey = key;
        segmentCommits.push_back({ key, std::move(notes), std::move(segments), range });
        return {};
    }
    ContentCommitSnapshot commitNotesAndSegmentsWithOriginalF0(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>, ContentEditRangeFrames,
        std::vector<float>, std::optional<std::vector<F0FrameRange>>) override { return {}; }
    bool setTimeGrid(ContentKey, std::shared_ptr<const TimeGridSnapshot>) override { return false; }
    bool applyPitchShiftState(ContentKey, const PitchShiftEditState&) override { return false; }
};

bool checkUndoActionsUseBoundKey()
{
    const ContentKey key{ DomainKind::StandaloneClip, 91, 4 };
    auto commands = std::make_shared<RecordingCommands>();
    const auto topologySnapshot = makeSnapshot(false);
    const auto topologyPlan = planSelectedNoteDelete(
        topologySnapshot, { 0, 2 }, topologySnapshot.notesRevision);
    const auto& topology = std::get<SelectedNoteTopologyDelete>(topologyPlan.edit);
    PianoRollNotePatchAction noteAction(
        commands, key, "Delete note", topology.beforePatch, topology.afterPatch);

    noteAction.undo();
    if (commands->lastKey != key || commands->notePatches.size() != 1
        || !samePatch(commands->notePatches.back(), topology.beforePatch))
        return false;
    noteAction.redo();
    if (commands->lastKey != key || commands->notePatches.size() != 2
        || !samePatch(commands->notePatches.back(), topology.afterPatch))
        return false;

    const auto curveSnapshot = makeSnapshot(true);
    const auto curvePlan = planSelectedNoteDelete(curveSnapshot, { 0, 2 }, curveSnapshot.notesRevision);
    const auto& curve = std::get<SelectedNotePitchCurveDelete>(curvePlan.edit);
    const ContentEditRangeFrames range{
        curve.affectedRange.startFrame, curve.affectedRange.endFrameExclusive
    };
    PianoRollEditAction curveAction(
        commands, key, "Delete note", curve.beforeNotes, curve.afterNotes,
        curve.beforeSegments, curve.afterSegments, range);

    curveAction.undo();
    const auto& before = commands->segmentCommits.back();
    if (before.key != key || before.range.startFrame != range.startFrame
        || before.range.endFrameExclusive != range.endFrameExclusive
        || !sameNotes(before.notes, curve.beforeNotes)
        || !sameSegments(before.segments, curve.beforeSegments))
        return false;
    curveAction.redo();
    const auto& after = commands->segmentCommits.back();
    return after.key == key
        && after.range.startFrame == range.startFrame
        && after.range.endFrameExclusive == range.endFrameExclusive
        && sameNotes(after.notes, curve.afterNotes)
        && sameSegments(after.segments, curve.afterSegments);
}

} // namespace

int main()
{
    if (!checkTopologyDelete()) {
        std::cerr << "Selected-note topology delete failed\n";
        return 1;
    }
    if (!checkPitchCurveDelete()) {
        std::cerr << "Selected-note pitch-curve delete failed\n";
        return 1;
    }
    if (!checkNoTargetAndInvalidInput()) {
        std::cerr << "Selected-note delete target validation failed\n";
        return 1;
    }
    if (!checkUndoActionsUseBoundKey()) {
        std::cerr << "Selected-note delete undo/redo failed\n";
        return 1;
    }
    return 0;
}
