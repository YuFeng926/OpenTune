#pragma once

#include "UndoManager.h"
#include "Note.h"
#include "Content/ContentKey.h"
#include <functional>
#include <memory>
#include <vector>

namespace OpenTune {

// Scissors 拓扑 undo action：保存切分前后的完整 notes 向量。
// undo/redo 直接应用 before/after 完整向量（replaceContentNotesForFullMutation），
// 不重新计算切点、不触发 render、不失效 RenderCache。
class ScissorsUndoAction : public UndoAction {
public:
    ScissorsUndoAction(ContentKey key,
                       juce::String description,
                       std::vector<Note> beforeNotes,
                       std::vector<Note> afterNotes,
                       std::function<bool(const std::vector<Note>&)> applyNotes);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

private:
    ContentKey contentKey_;
    juce::String description_;
    std::vector<Note> beforeNotes_, afterNotes_;
    std::function<bool(const std::vector<Note>&)> applyNotes_;
};

} // namespace OpenTune
