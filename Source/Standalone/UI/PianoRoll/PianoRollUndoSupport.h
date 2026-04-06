#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "Utils/UndoAction.h"
#include <vector>
#include <functional>
#include <cstdint>

namespace OpenTune {

class OpenTuneAudioProcessor;

/**
 * PianoRoll 撤销支持类
 * 管理 PianoRoll 编辑操作的撤销/重做：音符编辑、F0曲线编辑
 * 
 * 设计原则：
 * - 单一语义入口：所有 Undo Action 的创建都通过此类
 * - 事务机制：beginTransaction/commitTransaction 捕获编辑前后状态
 */
class PianoRollUndoSupport
{
public:
    struct Context
    {
        std::function<std::vector<Note>()> getNotesCopy;
        std::function<std::shared_ptr<PitchCurve>()> getPitchCurve;
        std::function<uint64_t()> getCurrentClipId;
        std::function<int()> getCurrentTrackId;
        std::function<OpenTuneAudioProcessor*()> getProcessor;
        std::function<UndoManager*()> getUndoManager;
    };

    explicit PianoRollUndoSupport(Context context);

    UndoManager* getCurrentUndoManager() noexcept;

    // === PianoRoll 编辑事务（唯一入口）===
    void beginTransaction(const juce::String& description);
    void commitTransaction();
    bool isTransactionActive() const noexcept;

    static bool notesEquivalent(const std::vector<Note>& a, const std::vector<Note>& b);

private:
    void clearTransaction();

    Context ctx_;

    bool transactionActive_ = false;
    juce::String transactionDescription_;
    std::vector<Note> transactionBeforeNotes_;
    std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot> transactionBeforeSegments_;
};

} // namespace OpenTune
