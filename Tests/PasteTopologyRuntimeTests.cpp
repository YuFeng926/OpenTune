/**
 * Paste topology runtime tests — direct use of the production paste planning
 * function planPasteTopology and normalizeStoredNotes from Source/Utils/Note.h.
 * No mocks and no copied production algorithms for the planning step.
 *
 * 场景一（生产 bug 回归）：原音符 A=[0,10]，粘贴 B=[5,8] 落在 A 内部。
 * normalizeStoredNotes 把 A 截短为 A'=[0,5]；affected range 必须扩展覆盖
 * A 的完整 [0,10]，否则 afterPatch 丢失 A'，提交后 A 被整体删除，
 * Undo/Redo 不对称。
 *
 * 场景二（对称）：粘贴 B=[2,8] 切入右侧原音符 A=[3,10]，粘贴音符被截短为
 * B'=[2,3]，原音符完整保留；range 扩展为 [2,10]，提交/Undo/Redo 对称。
 *
 * 场景三（无相交）：粘贴落在原音符区间之外，原音符由 keptBefore 完整保留。
 */
#include <cstddef>
#include <iostream>
#include <vector>

#include "Utils/Note.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition)
        return;

    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

OpenTune::Note makeNote(double startTime, double endTime)
{
    OpenTune::Note note;
    note.startTime = startTime;
    note.endTime = endTime;
    return note;
}

bool sameNotes(const std::vector<OpenTune::Note>& left,
               const std::vector<OpenTune::Note>& right)
{
    if (left.size() != right.size())
        return false;

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].startTime != right[index].startTime
            || left[index].endTime != right[index].endTime)
            return false;
    }
    return true;
}

// 提交/Undo/Redo 直接调用生产 mergeNotesRange（Source/Utils/Note.h）：
// keptBefore(endTime <= start) + afterNotesInRange + keptAfter(startTime >= end)，
// 最后 normalizeStoredNotes。运行测试直接覆盖生产实现，无复制模型。

void testPasteTruncatesLeftOriginalNote()
{
    using namespace OpenTune;

    const auto original = std::vector<Note>{makeNote(0.0, 10.0)};
    const auto pasted = std::vector<Note>{makeNote(5.0, 8.0)};

    const auto plan = planPasteTopology(original, pasted);

    expect(plan.affectedRange.startSeconds == 0.0 && plan.affectedRange.endSeconds == 10.0,
           "affected range extends over the truncated original note's full boundaries");
    expect(sameNotes(plan.beforeNotesInRange, {makeNote(0.0, 10.0)}),
           "before patch keeps the full original note");
    expect(sameNotes(plan.afterNotesInRange, {makeNote(0.0, 5.0), makeNote(5.0, 8.0)}),
           "after patch keeps the truncated original note next to the pasted note");

    // 提交：A' + B
    const auto committed = mergeNotesRange(original, plan.affectedRange, plan.afterNotesInRange);
    expect(sameNotes(committed, {makeNote(0.0, 5.0), makeNote(5.0, 8.0)}),
           "commit yields the truncated original note plus the pasted note");

    // Undo：恢复 A
    const auto undone = mergeNotesRange(committed, plan.affectedRange, plan.beforeNotesInRange);
    expect(sameNotes(undone, {makeNote(0.0, 10.0)}),
           "undo restores the original note");

    // Redo：A' + B
    const auto redone = mergeNotesRange(undone, plan.affectedRange, plan.afterNotesInRange);
    expect(sameNotes(redone, {makeNote(0.0, 5.0), makeNote(5.0, 8.0)}),
           "redo restores the truncated original note plus the pasted note");
}

void testPasteTruncatesIntoRightSideOriginal()
{
    using namespace OpenTune;

    const auto original = std::vector<Note>{makeNote(3.0, 10.0)};
    const auto pasted = std::vector<Note>{makeNote(2.0, 8.0)};

    const auto plan = planPasteTopology(original, pasted);

    expect(plan.affectedRange.startSeconds == 2.0 && plan.affectedRange.endSeconds == 10.0,
           "affected range spans from the pasted note start to the original note end");
    expect(sameNotes(plan.beforeNotesInRange, {makeNote(3.0, 10.0)}),
           "before patch keeps the intact right-side original note");
    expect(sameNotes(plan.afterNotesInRange, {makeNote(2.0, 3.0), makeNote(3.0, 10.0)}),
           "after patch keeps the truncated pasted note and the intact original note");

    const auto committed = mergeNotesRange(original, plan.affectedRange, plan.afterNotesInRange);
    expect(sameNotes(committed, {makeNote(2.0, 3.0), makeNote(3.0, 10.0)}),
           "commit keeps the intact original note next to the truncated pasted note");

    const auto undone = mergeNotesRange(committed, plan.affectedRange, plan.beforeNotesInRange);
    expect(sameNotes(undone, {makeNote(3.0, 10.0)}),
           "undo restores only the original note");

    const auto redone = mergeNotesRange(undone, plan.affectedRange, plan.afterNotesInRange);
    expect(sameNotes(redone, {makeNote(2.0, 3.0), makeNote(3.0, 10.0)}),
           "redo restores the truncated pasted note plus the original note");
}

void testPasteWithoutIntersectionKeepsOriginalNotes()
{
    using namespace OpenTune;

    const auto original = std::vector<Note>{makeNote(0.0, 2.0)};
    const auto pasted = std::vector<Note>{makeNote(5.0, 8.0)};

    const auto plan = planPasteTopology(original, pasted);

    expect(plan.affectedRange.startSeconds == 5.0 && plan.affectedRange.endSeconds == 8.0,
           "non-intersecting paste keeps the paste envelope as the affected range");
    expect(plan.beforeNotesInRange.empty(), "before patch is empty without intersection");
    expect(sameNotes(plan.afterNotesInRange, {makeNote(5.0, 8.0)}),
           "after patch contains only the pasted note");

    const auto committed = mergeNotesRange(original, plan.affectedRange, plan.afterNotesInRange);
    expect(sameNotes(committed, {makeNote(0.0, 2.0), makeNote(5.0, 8.0)}),
           "commit keeps the untouched original note before the pasted note");

    const auto undone = mergeNotesRange(committed, plan.affectedRange, plan.beforeNotesInRange);
    expect(sameNotes(undone, {makeNote(0.0, 2.0)}),
           "undo restores the original state");
}

} // namespace

int main()
{
    testPasteTruncatesLeftOriginalNote();
    testPasteTruncatesIntoRightSideOriginal();
    testPasteWithoutIntersectionKeepsOriginalNotes();

    if (failures != 0) {
        std::cerr << failures << " Paste topology runtime test(s) failed\n";
        return 1;
    }

    std::cout << "Paste topology runtime tests passed\n";
    return 0;
}
