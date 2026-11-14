#include "ContentEditCommandsStub.h"
#include "Content/EditableContentSnapshot.h"
#include "Utils/PasteTopologyEdit.h"
#include "Utils/PianoRollNotePatchAction.h"
#include <algorithm>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace OpenTune;
using namespace OpenTune::TestSupport;

Note makeNote(double start, double end, float pitch)
{
    Note note;
    note.startTime = start;
    note.endTime = end;
    note.pitch = pitch;
    note.originalPitch = pitch;
    note.retuneSpeed = 0.42f;
    note.vibratoDepth = 0.11f;
    note.vibratoRate = 5.3f;
    note.outputGainDb = -3.0f;
    note.pitchDriftScale = 0.8f;
    note.isVoiced = true;
    return note;
}

bool sameNote(const Note& left, const Note& right)
{
    return left.startTime == right.startTime
        && left.endTime == right.endTime
        && left.pitch == right.pitch
        && left.originalPitch == right.originalPitch
        && left.retuneSpeed == right.retuneSpeed
        && left.vibratoDepth == right.vibratoDepth
        && left.vibratoRate == right.vibratoRate
        && left.outputGainDb == right.outputGainDb
        && left.pitchDriftScale == right.pitchDriftScale
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

bool samePatch(const ContentNoteRangePatch& left, const ContentNoteRangePatch& right)
{
    return left.affectedRange.startSeconds == right.affectedRange.startSeconds
        && left.affectedRange.endSeconds == right.affectedRange.endSeconds
        && sameNotes(left.afterNotesInRange, right.afterNotesInRange);
}

EditableContentSnapshot makeSnapshot()
{
    EditableContentSnapshot snapshot;
    snapshot.notesRevision = 7;
    // A=[0,10] 与粘贴 B=[5,8] 重叠 → 归一化后截短为 A'=[0,5]
    snapshot.notes = { makeNote(0.0, 10.0, 220.0f) };
    return snapshot;
}

bool checkPlanTranslatesClipboard()
{
    const auto snapshot = makeSnapshot();
    // 剪贴板两音符：间距 2s；第二音符时长 3s
    std::vector<Note> clipboard = {
        makeNote(10.0, 12.0, 220.0f),
        makeNote(14.0, 17.0, 246.94f),
    };

    const auto plan = planPasteNotesEdit(snapshot, clipboard, 100.0, snapshot.notesRevision);
    if (plan.status != PasteNotesStatus::Ready
        || plan.pastedNotes.size() != 2
        || plan.pastedNotes[0].startTime != 100.0
        || plan.pastedNotes[0].endTime != 102.0
        || plan.pastedNotes[1].startTime != 104.0
        || plan.pastedNotes[1].endTime != 107.0)
        return false;

    // 全字段保持：pitch/参数原样 + duration/relative offsets 保持 + dirty=true
    const auto& p0 = plan.pastedNotes[0];
    return p0.pitch == 220.0f
        && p0.originalPitch == 220.0f
        && p0.retuneSpeed == 0.42f
        && p0.vibratoDepth == 0.11f
        && p0.vibratoRate == 5.3f
        && p0.outputGainDb == -3.0f
        && p0.pitchDriftScale == 0.8f
        && p0.isVoiced
        && !p0.eq.has_value()
        && p0.dirty
        && plan.pastedNotes[1].dirty
        && (plan.pastedNotes[1].startTime - plan.pastedNotes[0].startTime) == 4.0
        && plan.pastedNotes[0].getDuration() == 2.0
        && plan.pastedNotes[1].getDuration() == 3.0;
}

bool checkStaleRevisionAndEmptyClipboard()
{
    const auto snapshot = makeSnapshot();
    std::vector<Note> clipboard = { makeNote(0.0, 1.0, 220.0f) };
    return planPasteNotesEdit(snapshot, clipboard, 0.0, snapshot.notesRevision + 1).status
               == PasteNotesStatus::StaleRevision
        && planPasteNotesEdit(snapshot, {}, 0.0, snapshot.notesRevision).status
               == PasteNotesStatus::EmptyClipboard;
}

bool checkTopologyOverlapTrim()
{
    const auto snapshot = makeSnapshot();
    std::vector<Note> clipboard = { makeNote(5.0, 8.0, 261.63f) };
    const auto plan = planPasteNotesEdit(snapshot, clipboard, 5.0, snapshot.notesRevision);
    if (plan.status != PasteNotesStatus::Ready)
        return false;

    // before patch：原音符完整 [0,10]；after patch：原音符截短为 [0,5] + 粘贴 [5,8]
    // （粘贴音符保持 dirty=true，原音符截短部分由归一化保持原 dirty）
    const std::vector<Note> beforeExpected = { makeNote(0.0, 10.0, 220.0f) };
    std::vector<Note> afterExpected = {
        makeNote(0.0, 5.0, 220.0f),
        makeNote(5.0, 8.0, 261.63f),
    };
    afterExpected[1].dirty = true;
    if (plan.beforePatch.affectedRange.startSeconds != 0.0
        || plan.beforePatch.affectedRange.endSeconds != 10.0
        || !sameNotes(plan.beforePatch.afterNotesInRange, beforeExpected)
        || plan.afterPatch.affectedRange.startSeconds != 0.0
        || plan.afterPatch.affectedRange.endSeconds != 10.0
        || !sameNotes(plan.afterPatch.afterNotesInRange, afterExpected))
        return false;

    // mergeNotesRange 应用 after patch 的结果
    const auto merged = mergeNotesRange(
        snapshot.notes, plan.afterPatch.affectedRange, plan.afterPatch.afterNotesInRange);
    return merged.size() == 2
        && merged[0].startTime == 0.0 && merged[0].endTime == 5.0
        && merged[1].startTime == 5.0 && merged[1].endTime == 8.0;
}

class RecordingCommands final : public ContentEditCommandsStub
{
public:
    ContentKey lastKey;
    std::vector<ContentNoteRangePatch> notePatches;

    bool replaceContentNotesForFullMutation(ContentKey, std::vector<Note>) override { return false; }
    ContentCommitSnapshot commitNoteTopologyPatch(ContentKey key, ContentNoteRangePatch patch) override
    {
        lastKey = key;
        notePatches.push_back(std::move(patch));
        return {};
    }
    ContentCommitSnapshot commitNotesAndSegments(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>,
        ContentEditRangeFrames) override { return {}; }
    ContentCommitSnapshot commitNotesAndSegmentsWithOriginalF0(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>, ContentEditRangeFrames,
        std::vector<float>, std::optional<std::vector<F0FrameRange>>) override { return {}; }
    bool setTimeGrid(ContentKey, std::shared_ptr<const TimeGridSnapshot>) override { return false; }
    bool applyPitchShiftState(ContentKey, const PitchShiftEditState&) override { return false; }
};

bool checkUndoRedoUsesBoundKey()
{
    const ContentKey key{ DomainKind::StandaloneClip, 37, 1 };
    const auto snapshot = makeSnapshot();
    std::vector<Note> clipboard = { makeNote(5.0, 8.0, 261.63f) };
    const auto plan = planPasteNotesEdit(snapshot, clipboard, 5.0, snapshot.notesRevision);
    if (plan.status != PasteNotesStatus::Ready)
        return false;

    auto commands = std::make_shared<RecordingCommands>();
    PianoRollNotePatchAction action(
        commands, key, "Paste", plan.beforePatch, plan.afterPatch);

    action.undo();
    if (commands->lastKey != key || commands->notePatches.size() != 1
        || !samePatch(commands->notePatches.back(), plan.beforePatch))
        return false;
    action.redo();
    return commands->lastKey == key && commands->notePatches.size() == 2
        && samePatch(commands->notePatches.back(), plan.afterPatch);
}

} // namespace

int main()
{
    if (!checkPlanTranslatesClipboard()) {
        std::cerr << "Paste plan translation/field preservation failed\n";
        return 1;
    }
    if (!checkStaleRevisionAndEmptyClipboard()) {
        std::cerr << "Paste plan revision/clipboard validation failed\n";
        return 1;
    }
    if (!checkTopologyOverlapTrim()) {
        std::cerr << "Paste topology overlap trim failed\n";
        return 1;
    }
    if (!checkUndoRedoUsesBoundKey()) {
        std::cerr << "Paste undo/redo bound key failed\n";
        return 1;
    }
    return 0;
}
