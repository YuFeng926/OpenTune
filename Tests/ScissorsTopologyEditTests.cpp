#include "ContentEditCommandsStub.h"
#include "Content/ContentKey.h"
#include "Utils/ScissorsTopologyEdit.h"
#include "Utils/ScissorsUndoAction.h"
#include "Utils/UndoManager.h"
#include <algorithm>
#include <cmath>
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
    note.pitchOffset = 0.5f;
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

F0Timeline makeTimeline()
{
    // hop=10ms, 100 帧 → [0, 1.0s)
    return F0Timeline(10, 1000.0, 100);
}

// F0 数据：帧 0..99，帧 0..49 = 200Hz，帧 50..99 = 400Hz。
std::vector<float> makeF0()
{
    std::vector<float> f0(100, 400.0f);
    std::fill(f0.begin(), f0.begin() + 50, 200.0f);
    return f0;
}

bool checkSplitStrictBoundaries()
{
    const auto f0tl = makeTimeline();
    const auto f0 = makeF0();
    const std::vector<Note> before = { makeNote(0.0, 1.0, 220.0f) };

    // splitTime 恰在边界（0.0 / 1.0）→ 不切割
    const auto atStart = planScissorsSplit(before, 0.0, f0, f0tl);
    const auto atEnd = planScissorsSplit(before, 1.0, f0, f0tl);
    if (atStart.cutCount != 0 || atStart.lastRightIndex != -1
        || atStart.afterNotes.size() != 1 || !sameNote(atStart.afterNotes[0], before[0]))
        return false;
    if (atEnd.cutCount != 0 || atEnd.afterNotes.size() != 1)
        return false;

    // splitTime 在音符内部 → 切成两段
    const auto plan = planScissorsSplit(before, 0.5, f0, f0tl);
    if (plan.cutCount != 1 || plan.lastRightIndex != 1 || plan.afterNotes.size() != 2)
        return false;
    const auto& left = plan.afterNotes[0];
    const auto& right = plan.afterNotes[1];
    // 严格边界：left.endTime == splitTime，right.startTime == splitTime
    if (left.startTime != 0.0 || left.endTime != 0.5
        || right.startTime != 0.5 || right.endTime != 1.0)
        return false;
    // 切割片 dirty=true
    if (!left.dirty || !right.dirty)
        return false;
    // left 其余字段保留（retuneSpeed 等），right 其余参数丢弃（同源拷贝）
    if (left.retuneSpeed != 0.42f || right.retuneSpeed != 0.42f
        || left.outputGainDb != -3.0f || right.outputGainDb != -3.0f)
        return false;
    // effective F0 平均：left [0,0.5) 全 200Hz，right [0.5,1.0) 全 400Hz；
    // avg > 0 → pitch 更新、pitchOffset=0
    if (left.pitch != 200.0f || left.pitchOffset != 0.0f
        || right.pitch != 400.0f || right.pitchOffset != 0.0f)
        return false;
    return true;
}

bool checkSplitF0ZeroRegionKeepsPitch()
{
    const auto f0tl = makeTimeline();
    // F0 全 0（unvoiced 区间）→ avg <= 0 → 保持原 pitch
    const std::vector<float> zeros(100, 0.0f);
    const std::vector<Note> before = { makeNote(0.0, 1.0, 220.0f) };
    const auto plan = planScissorsSplit(before, 0.5, zeros, f0tl);
    if (plan.cutCount != 1)
        return false;
    return plan.afterNotes[0].pitch == 220.0f
        && plan.afterNotes[0].pitchOffset == 0.5f
        && plan.afterNotes[1].pitch == 220.0f
        && plan.afterNotes[1].pitchOffset == 0.5f;
}

bool checkSplitFloorCeilBoundaries()
{
    const auto f0tl = makeTimeline();
    const auto f0 = makeF0();
    const std::vector<Note> before = { makeNote(0.0, 1.0, 220.0f) };
    // splitTime = 0.505 → left [0, 0.505) = 帧 0..50（floor(0)=0, ceil(50.5)=51 → 帧 0..50 含帧 50=400Hz）
    const auto plan = planScissorsSplit(before, 0.505, f0, f0tl);
    if (plan.cutCount != 1)
        return false;
    // left 含帧 50（400Hz）：帧 0..49 = 200Hz + 帧 50 = 400Hz → avg = (200*50+400)/51
    const float expectedLeft = (200.0f * 50.0f + 400.0f) / 51.0f;
    return std::abs(plan.afterNotes[0].pitch - expectedLeft) < 1e-3f
        && plan.afterNotes[1].pitch == 400.0f;
}

bool checkSplitNoHit()
{
    const auto f0tl = makeTimeline();
    const auto f0 = makeF0();
    const std::vector<Note> before = { makeNote(0.0, 0.4, 220.0f), makeNote(0.6, 1.0, 300.0f) };
    // splitTime 在音符间隙 → 不切割，afterNotes 原样
    const auto plan = planScissorsSplit(before, 0.5, f0, f0tl);
    return plan.cutCount == 0 && plan.lastRightIndex == -1
        && plan.afterNotes.size() == 2
        && sameNote(plan.afterNotes[0], before[0])
        && sameNote(plan.afterNotes[1], before[1]);
}

bool checkMergePairing()
{
    const auto f0tl = makeTimeline();
    const auto f0 = makeF0();
    // 两条不同 pitch 行的音符共享分离线 0.5
    const std::vector<Note> before = {
        makeNote(0.0, 0.5, 220.0f),
        makeNote(0.5, 1.0, 300.0f),
        makeNote(0.0, 0.5, 260.0f),
        makeNote(0.5, 1.0, 340.0f),
    };
    const auto plan = planScissorsMerge(before, 0.5, 0.01, f0, f0tl);
    if (plan.mergeCount != 2 || plan.afterNotes.size() != 2)
        return false;
    // 每对合并：left 字段保留、endTime 取 right.endTime、dirty=true
    const auto& m0 = plan.afterNotes[0];
    const auto& m1 = plan.afterNotes[1];
    if (m0.startTime != 0.0 || m0.endTime != 1.0 || !m0.dirty)
        return false;
    if (m1.startTime != 0.0 || m1.endTime != 1.0 || !m1.dirty)
        return false;
    // merged 保留 left 其余字段（pitch 之外的参数），right 其余参数丢弃：
    // 对 [0,1.0) 平均 = (200*50+400*50)/100 = 300Hz
    if (m0.pitch != 300.0f || m0.pitchOffset != 0.0f
        || m1.pitch != 300.0f || m1.pitchOffset != 0.0f)
        return false;
    if (m0.retuneSpeed != 0.42f || m1.retuneSpeed != 0.42f)
        return false;
    return true;
}

bool checkMergeNoRepeatConsume()
{
    const auto f0tl = makeTimeline();
    const auto f0 = makeF0();
    // 三个音符共享同一分离线：left 只有其后第一个 right 与之配对，right 不被两次消费
    const std::vector<Note> before = {
        makeNote(0.0, 0.5, 220.0f),
        makeNote(0.5, 1.0, 300.0f),
        makeNote(0.5, 0.8, 340.0f),
    };
    const auto plan = planScissorsMerge(before, 0.5, 0.01, f0, f0tl);
    if (plan.mergeCount != 1 || plan.afterNotes.size() != 2)
        return false;
    // 第一个 right 被消费后归入 merged；第二个 right 保留原样
    const auto& merged = plan.afterNotes[0];
    if (merged.startTime != 0.0 || merged.endTime != 1.0 || !merged.dirty)
        return false;
    const auto& kept = plan.afterNotes[1];
    return kept.startTime == 0.5 && kept.endTime == 0.8 && kept.pitch == 340.0f && !kept.dirty;
}

bool checkMergeNoHit()
{
    const auto f0tl = makeTimeline();
    const auto f0 = makeF0();
    const std::vector<Note> before = { makeNote(0.0, 0.4, 220.0f) };
    // splitTime 不在任何音符边界 → 不合并
    const auto plan = planScissorsMerge(before, 0.5, 0.01, f0, f0tl);
    return plan.mergeCount == 0 && plan.afterNotes.size() == 1
        && sameNote(plan.afterNotes[0], before[0]);
}

bool checkSplitThenMergeRestoresBoundary()
{
    const auto f0tl = makeTimeline();
    const auto f0 = makeF0();
    const std::vector<Note> original = { makeNote(0.0, 1.0, 220.0f) };
    const auto split = planScissorsSplit(original, 0.5, f0, f0tl);
    if (split.cutCount != 1)
        return false;
    // 对切割点（边界 0.5）做 merge → 恢复原时间边界 [0, 1.0)
    // 容差与旧 UI 一致为正（像素容差换算），此处取 0.01s。
    const auto merge = planScissorsMerge(split.afterNotes, 0.5, 0.01, f0, f0tl);
    if (merge.mergeCount != 1 || merge.afterNotes.size() != 1)
        return false;
    const auto& restored = merge.afterNotes[0];
    return restored.startTime == 0.0 && restored.endTime == 1.0 && restored.dirty;
}

// ── ScissorsUndoAction：undo/redo 固定 ContentKey ──

class RecordingCommands final : public ContentEditCommandsStub
{
public:
    ContentKey lastKey;
    std::vector<std::vector<Note>> appliedNotes;
    size_t replaceCalls = 0;
    size_t republishCalls = 0;
    bool nextReplaceResult = true;

    bool replaceContentNotesForFullMutation(ContentKey key, std::vector<Note> notes) override
    {
        ++replaceCalls;
        if (!nextReplaceResult)
            return false;
        lastKey = key;
        appliedNotes.push_back(std::move(notes));
        return true;
    }
    ContentCommitSnapshot commitNoteTopologyPatch(ContentKey, ContentNoteRangePatch) override { return {}; }
    ContentCommitSnapshot commitNotesAndSegments(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>,
        ContentEditRangeFrames) override { return {}; }
    ContentCommitSnapshot commitNotesAndSegmentsWithOriginalF0(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>, ContentEditRangeFrames,
        std::vector<float>, std::optional<std::vector<F0FrameRange>>) override { return {}; }
    bool setTimeGrid(ContentKey, std::shared_ptr<const TimeGridSnapshot>) override { return false; }
    bool applyPitchShiftState(ContentKey, const PitchShiftEditState&) override { return false; }
    void republishPlaybackSource(ContentKey key) override
    {
        ++republishCalls;
        lastKey = key;
    }
};

bool checkUndoRedoUsesBoundKey()
{
    const ContentKey key{ DomainKind::StandaloneClip, 37, 1 };
    const ContentKey otherKey{ DomainKind::StandaloneClip, 99, 2 };
    const std::vector<Note> before = { makeNote(0.0, 1.0, 220.0f) };
    const std::vector<Note> after = {
        makeNote(0.0, 0.5, 220.0f),
        makeNote(0.5, 1.0, 220.0f),
    };

    auto commands = std::make_shared<RecordingCommands>();
    ScissorsUndoAction action(commands, key, "Split note", before, after);

    action.undo();
    if (commands->lastKey != key || commands->appliedNotes.size() != 1
        || !sameNote(commands->appliedNotes[0].front(), before.front()))
        return false;

    action.redo();
    if (commands->lastKey != key || commands->appliedNotes.size() != 2
        || !sameNote(commands->appliedNotes[1].front(), after.front()))
        return false;

    // 模拟调用者当前 key 切换后 undo/redo 仍对原 key 操作（绑定 key，不解析当前 key）
    commands->lastKey = otherKey;
    action.undo();
    return commands->lastKey == key && commands->appliedNotes.size() == 3;
}

bool checkUndoWriteFailureDoesNotRepublish()
{
    const ContentKey key{ DomainKind::StandaloneClip, 37, 1 };
    const std::vector<Note> before = { makeNote(0.0, 1.0, 220.0f) };
    const std::vector<Note> after = { makeNote(0.0, 0.5, 220.0f) };

    auto commands = std::make_shared<RecordingCommands>();
    commands->nextReplaceResult = false;
    ScissorsUndoAction action(commands, key, "Split note", before, after);

    action.undo();
    action.redo();
    return commands->republishCalls == 0;
}

} // namespace

int main()
{
    if (!checkSplitStrictBoundaries()) {
        std::cerr << "Scissors split strict boundaries failed\n";
        return 1;
    }
    if (!checkSplitF0ZeroRegionKeepsPitch()) {
        std::cerr << "Scissors split F0 zero region keeps pitch failed\n";
        return 1;
    }
    if (!checkSplitFloorCeilBoundaries()) {
        std::cerr << "Scissors split floor/ceil boundaries failed\n";
        return 1;
    }
    if (!checkSplitNoHit()) {
        std::cerr << "Scissors split no-hit failed\n";
        return 1;
    }
    if (!checkMergePairing()) {
        std::cerr << "Scissors merge pairing failed\n";
        return 1;
    }
    if (!checkMergeNoRepeatConsume()) {
        std::cerr << "Scissors merge no-repeat-consume failed\n";
        return 1;
    }
    if (!checkMergeNoHit()) {
        std::cerr << "Scissors merge no-hit failed\n";
        return 1;
    }
    if (!checkSplitThenMergeRestoresBoundary()) {
        std::cerr << "Scissors split-then-merge restore failed\n";
        return 1;
    }
    if (!checkUndoRedoUsesBoundKey()) {
        std::cerr << "Scissors undo/redo bound key failed\n";
        return 1;
    }
    if (!checkUndoWriteFailureDoesNotRepublish()) {
        std::cerr << "Scissors undo write failure republish guard failed\n";
        return 1;
    }
    return 0;
}
