#include "ContentEditCommandsStub.h"
#include "Content/EditableContentSnapshot.h"
#include "Utils/PianoRollNotePatchAction.h"
#include "Utils/SelectedNoteEqEdit.h"
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace OpenTune;
using namespace OpenTune::TestSupport;

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

bool samePatch(const ContentNoteRangePatch& left, const ContentNoteRangePatch& right)
{
    return left.affectedRange.startSeconds == right.affectedRange.startSeconds
        && left.affectedRange.endSeconds == right.affectedRange.endSeconds
        && sameNotes(left.afterNotesInRange, right.afterNotesInRange);
}

EditableContentSnapshot makeSnapshot()
{
    EditableContentSnapshot snapshot;
    snapshot.notesRevision = 12;

    Note first;
    first.startTime = 0.1;
    first.endTime = 0.3;
    first.pitch = 220.0f;
    first.originalPitch = 220.0f;
    first.eq = EqSettings{};

    Note neighbor;
    neighbor.startTime = 0.4;
    neighbor.endTime = 0.6;
    neighbor.pitch = 246.94f;
    neighbor.originalPitch = 246.94f;
    neighbor.eq = EqSettings{};
    neighbor.eq->active = false;

    Note third;
    third.startTime = 0.7;
    third.endTime = 0.9;
    third.pitch = 261.63f;
    third.originalPitch = 261.63f;
    third.eq = EqSettings{};

    snapshot.notes = { first, neighbor, third };
    return snapshot;
}

EqSettings makeSettings()
{
    EqSettings settings;
    settings.active = false;
    settings.filters[1].frequencyHz = 4400.0f;
    settings.filters[1].gainDb = 3.5f;
    settings.filters[1].q = 2.75f;
    return settings;
}

bool checkSetAndRange()
{
    const auto snapshot = makeSnapshot();
    const ContentKey key{ DomainKind::StandaloneClip, 77, 0 };
    const auto settings = makeSettings();
    const auto plan = planSelectedNoteEqEdit(key, snapshot, { 0, 2 }, 12, settings);
    if (plan.status != SelectedNoteEqEditStatus::Ready
        || !settings.isValid()
        || plan.afterPatch.afterNotesInRange.size() != 3
        || plan.afterPatch.afterNotesInRange[0].eq != settings
        || plan.afterPatch.afterNotesInRange[1].eq != snapshot.notes[1].eq
        || plan.afterPatch.afterNotesInRange[2].eq != settings
        || plan.afterPatch.afterNotesInRange[0].dirty
        || plan.afterPatch.afterNotesInRange[2].dirty
        || plan.beforePatch.affectedRange.startSeconds != 0.1
        || plan.beforePatch.affectedRange.endSeconds != 0.9
        || plan.afterPatch.affectedRange.startSeconds != 0.1
        || plan.afterPatch.affectedRange.endSeconds != 0.9
        || !sameNotes(plan.beforePatch.afterNotesInRange, snapshot.notes))
        return false;

    auto editedSnapshot = snapshot;
    for (const int index : { 0, 2 })
        editedSnapshot.notes[static_cast<size_t>(index)].eq = settings;
    const auto repeat = planSelectedNoteEqEdit(key, editedSnapshot, { 0, 2 }, 12, settings);
    return repeat.status == SelectedNoteEqEditStatus::NoChange;
}

bool checkRemoveAndTargets()
{
    const auto snapshot = makeSnapshot();
    const ContentKey key{ DomainKind::StandaloneClip, 77, 0 };
    const auto plan = planSelectedNoteEqEdit(key, snapshot, { 0, 2 }, 12, std::nullopt);
    if (plan.status != SelectedNoteEqEditStatus::Ready
        || plan.afterPatch.afterNotesInRange.size() != 3
        || plan.afterPatch.afterNotesInRange[0].eq.has_value()
        || plan.afterPatch.afterNotesInRange[1].eq != snapshot.notes[1].eq
        || plan.afterPatch.afterNotesInRange[2].eq.has_value())
        return false;

    auto removedSnapshot = snapshot;
    removedSnapshot.notes[0].eq.reset();
    const auto removeAgain = planSelectedNoteEqEdit(key, removedSnapshot, { 0 }, 12, std::nullopt);
    if (removeAgain.status != SelectedNoteEqEditStatus::NoChange)
        return false;

    const auto empty = planSelectedNoteEqEdit(key, snapshot, {}, 12, std::nullopt).status;
    const auto invalid = planSelectedNoteEqEdit(key, snapshot, { -1, 3 }, 12, std::nullopt).status;
    const auto partial = planSelectedNoteEqEdit(key, snapshot, { 0, 3 }, 12, std::nullopt).status;
    if (empty != SelectedNoteEqEditStatus::NoTarget
        || invalid != SelectedNoteEqEditStatus::NoTarget
        || partial != SelectedNoteEqEditStatus::NoTarget) {
        return false;
    }
    return true;
}

bool checkRevisionAndValidation()
{
    const auto snapshot = makeSnapshot();
    const ContentKey key{ DomainKind::StandaloneClip, 77, 0 };
    const auto settings = makeSettings();
    uint64_t refreshedNotesRevision = 0;
    const auto stale = planSelectedNoteEqEdit(key, snapshot, { 0 }, refreshedNotesRevision, settings);
    refreshedNotesRevision = snapshot.notesRevision;
    const auto refreshed = planSelectedNoteEqEdit(key, snapshot, { 0 }, refreshedNotesRevision, settings);
    auto invalid = makeSettings();
    invalid.filters[0].frequencyHz = -1.0f;
    return stale.status == SelectedNoteEqEditStatus::StaleRevision
        && refreshed.status == SelectedNoteEqEditStatus::Ready
        && refreshed.afterPatch.afterNotesInRange.size() == 1
        && refreshed.afterPatch.afterNotesInRange[0].eq == settings
        && planSelectedNoteEqEdit(key, snapshot, { 0 }, 12, invalid).status
               == SelectedNoteEqEditStatus::InvalidSettings;
}

class RecordingCommands final : public ContentEditCommandsStub
{
public:
    ContentKey lastKey;
    std::vector<ContentNoteRangePatch> patches;

    bool replaceContentNotesForFullMutation(ContentKey, std::vector<Note>) override { return false; }
    ContentCommitSnapshot commitNoteTopologyPatch(ContentKey key, ContentNoteRangePatch patch) override
    {
        lastKey = key;
        patches.push_back(std::move(patch));
        return {};
    }
    ContentCommitSnapshot commitNotesAndSegments(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>, ContentEditRangeFrames) override { return {}; }
    ContentCommitSnapshot commitNotesAndSegmentsWithOriginalF0(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>, ContentEditRangeFrames,
        std::vector<float>, std::optional<std::vector<F0FrameRange>>) override { return {}; }
    bool setTimeGrid(ContentKey, std::shared_ptr<const TimeGridSnapshot>) override { return false; }
    bool applyPitchShiftState(ContentKey, const PitchShiftEditState&) override { return false; }
};

bool checkUndoRedoUsesBoundContentKey()
{
    const ContentKey key{ DomainKind::StandaloneClip, 91, 4 };
    auto commands = std::make_shared<RecordingCommands>();
    const auto snapshot = makeSnapshot();
    const auto settings = makeSettings();
    const auto plan = planSelectedNoteEqEdit(key, snapshot, { 0, 2 }, 12, settings);
    PianoRollNotePatchAction action(commands, key, "Edit EQ", plan.beforePatch, plan.afterPatch);

    action.undo();
    if (commands->lastKey != key || commands->patches.size() != 1
        || !samePatch(commands->patches.back(), plan.beforePatch))
        return false;
    action.redo();
    return commands->lastKey == key && commands->patches.size() == 2
        && samePatch(commands->patches.back(), plan.afterPatch);
}

} // namespace

int main()
{
    if (!checkSetAndRange()) {
        std::cerr << "EQ set/range check failed\n";
        return 1;
    }
    if (!checkRemoveAndTargets()) {
        std::cerr << "EQ remove/target check failed\n";
        return 1;
    }
    if (!checkRevisionAndValidation()) {
        std::cerr << "EQ revision/validation check failed\n";
        return 1;
    }
    if (!checkUndoRedoUsesBoundContentKey()) {
        std::cerr << "Selected-note EQ edit regression failed\n";
        return 1;
    }
    return 0;
}
