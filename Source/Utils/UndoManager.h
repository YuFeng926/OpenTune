/**
 * 撤销/重做管理器（UndoManager）
 *
 * 基于 Cursor 的线性 Undo 栈，上限 500 层。
 * 每个 UndoAction 负责自身的 undo()/redo() 语义。
 * addAction() 会丢弃 Cursor 之后的 redo 历史。
 *
 * 使用方式：Processor 持有唯一实例，双 Editor 共享。
 * 线程安全：仅在 message thread 调用。
 */
#pragma once

#include "../Content/ContentKey.h"
#include <juce_core/juce_core.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace OpenTune {

// 撤销操作基类：子类实现 undo()/redo() 对称操作
class UndoAction {
public:
    virtual ~UndoAction() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual juce::String getDescription() const = 0;
    virtual bool retainsRetiredPlacement(uint64_t trackIdentity, uint64_t placementId) const
    {
        juce::ignoreUnused(trackIdentity, placementId);
        return false;
    }
    virtual bool retainsRetiredContent(ContentKey key) const
    {
        juce::ignoreUnused(key);
        return false;
    }
    virtual bool dependsOnTrackIdentity(uint64_t identity) const
    {
        juce::ignoreUnused(identity);
        return false;
    }
    virtual bool dependsOnPlacement(uint64_t trackIdentity, uint64_t placementId) const
    {
        juce::ignoreUnused(trackIdentity, placementId);
        return false;
    }
    virtual bool dependsOnContent(ContentKey key) const
    {
        juce::ignoreUnused(key);
        return false;
    }
    virtual bool dependenciesExist() const { return true; }
};

class UndoManager {
public:
    struct PruneResult { int undo = 0; int redo = 0; };

    void addAction(std::unique_ptr<UndoAction> action);
    UndoAction* undo();   // Returns the undone action, or nullptr
    UndoAction* redo();   // Returns the redone action, or nullptr
    bool canUndo() const;
    bool canRedo() const;
    void clear();
    void setHistoryPrunedCallback(std::function<void()> callback)
    {
        historyPrunedCallback_ = std::move(callback);
    }
    bool retainsRetiredPlacement(uint64_t trackIdentity, uint64_t placementId) const;
    bool retainsRetiredContent(ContentKey key) const;
    PruneResult pruneForTrackDeletion(uint64_t trackIdentity,
                                      const std::vector<std::pair<uint64_t, uint64_t>>& placements,
                                      const std::vector<ContentKey>& removedContents,
                                      const std::function<bool(ContentKey)>& hasActiveReference);
    int pruneForContentRelease(ContentKey content);
    int pruneInvalidActions(bool notify = true);
    // Product commands inspect the settled history before applying an undo or redo.
    UndoAction* peekUndo() const { return canUndo() ? actions_[static_cast<std::size_t>(cursor_ - 1)].get() : nullptr; }
    UndoAction* peekRedo() const { return canRedo() ? actions_[static_cast<std::size_t>(cursor_)].get() : nullptr; }
    UndoAction* actionAt(int index) const
    {
        return index >= 0 && index < static_cast<int>(actions_.size())
            ? actions_[static_cast<std::size_t>(index)].get() : nullptr;
    }
    int indexOf(const UndoAction* action) const;
    int size() const { return static_cast<int>(actions_.size()); }
    int cursor() const { return cursor_; }
    void clearRedo()
    {
        if (!isPerformingUndoRedo_ && cursor_ < static_cast<int>(actions_.size())) {
            actions_.erase(actions_.begin() + cursor_, actions_.end());
            if (historyPrunedCallback_)
                historyPrunedCallback_();
        }
    }

private:
    int pruneActions(const std::function<bool(const UndoAction&)>& shouldPrune, bool notify);
    std::vector<std::unique_ptr<UndoAction>> actions_;
    int cursor_{0};  // 指向下一个可 redo 的位置（即当前栈顶 + 1）
    static constexpr int maxSize_{500};  // 最大历史层数
    bool isPerformingUndoRedo_{false};  // 防止 undo/redo 执行期间递归 addAction
    std::function<void()> historyPrunedCallback_;
};

} // namespace OpenTune
