#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "Utils/UndoAction.h"
#include <vector>
#include <functional>
#include <cstdint>

namespace OpenTune {

/**
 * 钢琴卷帘撤销支持类
 * 管理音符编辑和F0曲线编辑的撤销/重做操作
 */
class PianoRollUndoSupport
{
public:
    struct Context
    {
        std::function<std::vector<Note>&()> getNotes;
        std::function<std::vector<Note>()> getNotesCopy;
        std::function<std::shared_ptr<PitchCurve>()> getPitchCurve;

        std::function<UndoManager*()> getUndoManager;

        std::function<void(int, int)> notifyPitchCurveEdited;
        std::function<void(const std::vector<Note>&)> notifyNotesChanged;
        std::function<void()> requestRepaint;

        std::function<void()> updateScrollBars;
    };

    explicit PianoRollUndoSupport(Context context);

    UndoManager* getCurrentUndoManager() noexcept;
    const UndoManager* getCurrentUndoManager() const noexcept;
    bool canUndo() const noexcept;
    bool canRedo() const noexcept;

    void beginNotesEditUndo();
    void commitNotesEditUndo(const juce::String& description);
    void setNotesEditUndoState(std::vector<Note> notesBefore,
                                std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot> f0Before);
    bool isNotesEditUndoActive() const noexcept;

    void beginF0EditUndo(const juce::String& description);
    void commitF0EditUndo();

    static bool notesEquivalent(const std::vector<Note>& a, const std::vector<Note>& b);

    void undo();
    void redo();

private:
    static bool nearlyEqualFloat(float a, float b, float epsilon = 1e-6f);

    Context ctx_;

    bool notesEditUndoActive_ = false;
    std::vector<Note> notesEditUndoBefore_;
    std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot> notesEditF0Before_;

    bool f0EditUndoActive_ = false;
    std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot> f0EditUndoBefore_;
    juce::String f0EditUndoDescription_;
    int f0EditStartFrame_ = -1;
    int f0EditEndFrame_ = -1;
};

/**
 * 音高修正段快照比较工具结构体
 * 提供修正段和快照的等价性比较静态方法
 */
struct SegmentSnapshotCompare
{
    static bool equivalent(const CorrectedSegment& a, const CorrectedSegment& b);
    static bool listsEquivalent(const std::vector<CorrectedSegment>& a, const std::vector<CorrectedSegment>& b);
    static bool equivalent(const CorrectedSegmentsChangeAction::SegmentSnapshot& a,
                           const CorrectedSegmentsChangeAction::SegmentSnapshot& b);
    static bool listsEquivalent(const std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot>& a,
                                const std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot>& b);
};

} // namespace OpenTune