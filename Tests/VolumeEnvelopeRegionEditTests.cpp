#include "ContentEditCommandsStub.h"
#include "Content/EditableContentSnapshot.h"
#include "Utils/VolumeEnvelopeRegionEdit.h"
#include "Utils/VolumeEnvelopeEditAction.h"
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace OpenTune;
using namespace OpenTune::TestSupport;

EditableContentSnapshot makeSnapshot()
{
    EditableContentSnapshot snapshot;
    snapshot.notesRevision = 7;

    Note first;
    first.startTime = 0.2;
    first.endTime = 0.6;

    Note second;
    second.startTime = 0.5;
    second.endTime = 1.0;

    Note third;
    third.startTime = 1.2;
    third.endTime = 1.5;

    snapshot.notes = { first, second, third };

    // 基线包络：0..2s 从 -3 dB 线性到 3 dB（归一化点 {0,-3} {2,3}）。
    snapshot.volumeEnvelope = AutomationLane::fromSnapshot({ { 0.0, -3.0f }, { 2.0, 3.0f } });
    return snapshot;
}

// 旧 ToolHandler 拖拽循环语义参照：baseline 每次取 evalAt(note.start) + delta。
AutomationLane legacyDragPreview(const AutomationLane& baseline,
                                 const std::vector<Note>& notes,
                                 const std::vector<int>& indices,
                                 float deltaGainDb)
{
    AutomationLane envelope = baseline;
    for (const int index : indices) {
        const auto& note = notes[static_cast<size_t>(index)];
        envelope.setRegionGain(note.startTime, note.endTime,
                               baseline.evalAt(note.startTime) + deltaGainDb);
    }
    return envelope;
}

bool regionGainNear(const AutomationLane& lane, double time, float expectedDb)
{
    return std::abs(lane.evalAt(time) - expectedDb) <= 1.0e-3f;
}

bool checkDeltaMatchesLegacySemantics()
{
    const auto snapshot = makeSnapshot();
    const auto plan = planVolumeEnvelopeRegionDeltaEdit(snapshot, { 0, 1, 2 }, 7, 2.5f);
    if (plan.status != VolumeEnvelopeRegionEditStatus::Ready)
        return false;
    if (!(plan.beforeEnvelope == snapshot.volumeEnvelope))
        return false;
    if (!(plan.afterEnvelope == legacyDragPreview(snapshot.volumeEnvelope, snapshot.notes, { 0, 1, 2 }, 2.5f)))
        return false;

    // 重叠音符 0/1：每 region 的 gain 仍取 baseline evalAt(start)，不以前一变更为基准。
    // note0: start 0.2 → baseline -2.4 + 2.5 = 0.1；note1: start 0.5 → baseline -1.5 + 2.5 = 1.0；
    // note2: start 1.2 → baseline 0.6 + 2.5 = 3.1。
    const auto& after = plan.afterEnvelope;
    if (!regionGainNear(after, 0.2, 0.1f)
        || !regionGainNear(after, 0.35, 0.1f)
        || !regionGainNear(after, 0.5, 1.0f)
        || !regionGainNear(after, 0.9, 1.0f)
        || !regionGainNear(after, 1.2, 3.1f)
        || !regionGainNear(after, 1.4, 3.1f))
        return false;

    // 区域外回到 baseline。
    if (!regionGainNear(after, 0.1, snapshot.volumeEnvelope.evalAt(0.1))
        || !regionGainNear(after, 1.9, snapshot.volumeEnvelope.evalAt(1.9)))
        return false;
    return true;
}

bool checkOverlapIterationOrder()
{
    const auto snapshot = makeSnapshot();
    // 重叠区域由后处理的 note 覆盖；每次目标 gain 仍取同一 baseline。
    const auto forward = planVolumeEnvelopeRegionDeltaEdit(snapshot, { 0, 1 }, 7, -1.0f);
    const auto reverse = planVolumeEnvelopeRegionDeltaEdit(snapshot, { 1, 0 }, 7, -1.0f);
    const auto forwardLegacy = legacyDragPreview(snapshot.volumeEnvelope, snapshot.notes, { 0, 1 }, -1.0f);
    const auto reverseLegacy = legacyDragPreview(snapshot.volumeEnvelope, snapshot.notes, { 1, 0 }, -1.0f);
    if (forward.status != VolumeEnvelopeRegionEditStatus::Ready
        || reverse.status != VolumeEnvelopeRegionEditStatus::Ready)
        return false;
    return forward.afterEnvelope == forwardLegacy
        && reverse.afterEnvelope == reverseLegacy
        && !(forward.afterEnvelope == reverse.afterEnvelope);
}

bool checkResetZero()
{
    const auto snapshot = makeSnapshot();
    const auto plan = planVolumeEnvelopeRegionResetEdit(snapshot, 1, 7);
    if (plan.status != VolumeEnvelopeRegionEditStatus::Ready)
        return false;

    AutomationLane expected = snapshot.volumeEnvelope;
    expected.setRegionGain(snapshot.notes[1].startTime, snapshot.notes[1].endTime, 0.0f);
    if (!(plan.afterEnvelope == expected))
        return false;
    // reset 区域内为 0 dB。
    if (!regionGainNear(plan.afterEnvelope, 0.6, 0.0f)
        || !regionGainNear(plan.afterEnvelope, 0.9, 0.0f))
        return false;

    // 空包络上 reset（全零归一化 → lane 清空）→ NoChange。
    auto emptyEnvSnapshot = snapshot;
    emptyEnvSnapshot.volumeEnvelope.clear();
    const auto emptyPlan = planVolumeEnvelopeRegionResetEdit(emptyEnvSnapshot, 0, 7);
    return emptyPlan.status == VolumeEnvelopeRegionEditStatus::NoChange;
}

bool checkStatuses()
{
    const auto snapshot = makeSnapshot();

    // NoChange：baseline 在区域内已平坦，delta 0 不改变任何点。
    auto flatSnapshot = snapshot;
    flatSnapshot.volumeEnvelope = AutomationLane::fromSnapshot({ { 0.0, 1.0f }, { 2.0, 1.0f } });
    const auto noChange = planVolumeEnvelopeRegionDeltaEdit(flatSnapshot, { 0 }, 7, 0.0f);
    if (noChange.status != VolumeEnvelopeRegionEditStatus::NoChange
        || !(noChange.afterEnvelope == flatSnapshot.volumeEnvelope))
        return false;

    // StaleRevision。
    const auto stale = planVolumeEnvelopeRegionDeltaEdit(snapshot, { 0 }, 6, 1.0f);
    const auto staleReset = planVolumeEnvelopeRegionResetEdit(snapshot, 0, 99);
    if (stale.status != VolumeEnvelopeRegionEditStatus::StaleRevision
        || staleReset.status != VolumeEnvelopeRegionEditStatus::StaleRevision)
        return false;

    // NoTarget：空 indices / 含越界 index / 越界 reset index。
    const auto empty = planVolumeEnvelopeRegionDeltaEdit(snapshot, {}, 7, 1.0f);
    const auto outOfRange = planVolumeEnvelopeRegionDeltaEdit(snapshot, { -1, 5 }, 7, 1.0f);
    const auto mixed = planVolumeEnvelopeRegionDeltaEdit(snapshot, { 0, 9 }, 7, 1.0f);
    const auto badReset = planVolumeEnvelopeRegionResetEdit(snapshot, 3, 7);
    if (empty.status != VolumeEnvelopeRegionEditStatus::NoTarget
        || outOfRange.status != VolumeEnvelopeRegionEditStatus::NoTarget
        || mixed.status != VolumeEnvelopeRegionEditStatus::NoTarget
        || badReset.status != VolumeEnvelopeRegionEditStatus::NoTarget)
        return false;
    return true;
}

class RecordingCommands final : public ContentEditCommandsStub
{
public:
    ContentKey lastKey;
    std::vector<AutomationLane> committed;

    bool replaceContentNotesForFullMutation(ContentKey, std::vector<Note>) override { return false; }
    ContentCommitSnapshot commitNoteTopologyPatch(ContentKey, ContentNoteRangePatch) override { return {}; }
    ContentCommitSnapshot commitNotesAndSegments(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>, ContentEditRangeFrames) override { return {}; }
    ContentCommitSnapshot commitNotesAndSegmentsWithOriginalF0(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>, ContentEditRangeFrames,
        std::vector<float>, std::optional<std::vector<F0FrameRange>>) override { return {}; }
    bool setTimeGrid(ContentKey, std::shared_ptr<const TimeGridSnapshot>) override { return false; }
    bool applyPitchShiftState(ContentKey, const PitchShiftEditState&) override { return false; }
    ContentCommitSnapshot commitVolumeEnvelope(ContentKey key, AutomationLane envelope) override
    {
        lastKey = key;
        committed.push_back(envelope);
        return {};
    }
};

bool checkUndoRedoUsesBoundContentKey()
{
    const ContentKey key{ DomainKind::StandaloneClip, 91, 4 };
    auto commands = std::make_shared<RecordingCommands>();
    const auto snapshot = makeSnapshot();
    const auto plan = planVolumeEnvelopeRegionDeltaEdit(snapshot, { 0, 2 }, 7, 2.0f);
    VolumeEnvelopeEditAction action(commands, key, "Volume envelope",
                                    plan.beforeEnvelope, plan.afterEnvelope);

    action.undo();
    if (commands->lastKey != key || commands->committed.size() != 1
        || !(commands->committed.back() == plan.beforeEnvelope))
        return false;
    action.redo();
    return commands->lastKey == key && commands->committed.size() == 2
        && commands->committed.back() == plan.afterEnvelope;
}

} // namespace

int main()
{
    if (!checkDeltaMatchesLegacySemantics()) {
        std::cerr << "Volume region delta semantics check failed\n";
        return 1;
    }
    if (!checkOverlapIterationOrder()) {
        std::cerr << "Volume overlap iteration order check failed\n";
        return 1;
    }
    if (!checkResetZero()) {
        std::cerr << "Volume region reset check failed\n";
        return 1;
    }
    if (!checkStatuses()) {
        std::cerr << "Volume region edit status check failed\n";
        return 1;
    }
    if (!checkUndoRedoUsesBoundContentKey()) {
        std::cerr << "VolumeEnvelopeEditAction undo/redo regression failed\n";
        return 1;
    }
    return 0;
}
