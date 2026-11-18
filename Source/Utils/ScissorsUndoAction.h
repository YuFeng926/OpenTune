#pragma once

#include "UndoManager.h"
#include "Note.h"
#include "Content/ContentEditCommands.h"
#include "Content/ContentKey.h"
#include <memory>
#include <vector>

namespace OpenTune {

// Scissors 拓扑 undo action：保存切分/合并前后的完整 notes 向量与固定 ContentKey。
// undo/redo 经构造时绑定的 ContentEditCommands 对固定 key 全量替换 notes
// （replaceContentNotesForFullMutation + republishPlaybackSource），不重新计算切点、
// 不触发 render、不失效 RenderCache；写入失败时不 republish（内容未变，避免发布
// 错误内容）。不通过组件当前 editedContentKey_ 解析。
class ScissorsUndoAction : public UndoAction {
public:
    ScissorsUndoAction(std::shared_ptr<ContentEditCommands> commands,
                       ContentKey key,
                       juce::String description,
                       std::vector<Note> beforeNotes,
                       std::vector<Note> afterNotes);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }
    bool dependsOnContent(ContentKey key) const noexcept override { return contentKey_ == key; }

private:
    void applyNotes(const std::vector<Note>& notes);

    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String description_;
    std::vector<Note> beforeNotes_, afterNotes_;
};

} // namespace OpenTune
