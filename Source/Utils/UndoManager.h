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

#include <juce_core/juce_core.h>
#include <functional>
#include <memory>
#include <vector>

namespace OpenTune {

// 撤销操作基类：子类实现 undo()/redo() 对称操作
class UndoAction {
public:
    virtual ~UndoAction() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual juce::String getDescription() const = 0;
};

class UndoManager {
public:
    using ChangeCallback = std::function<void()>;

    void addAction(std::unique_ptr<UndoAction> action);
    UndoAction* undo();   // Returns the undone action, or nullptr
    UndoAction* redo();   // Returns the redone action, or nullptr
    bool canUndo() const;
    bool canRedo() const;
    void clear();

    juce::String getUndoDescription() const;
    juce::String getRedoDescription() const;

    void setOnChange(ChangeCallback cb) { onChange_ = std::move(cb); }

private:
    std::vector<std::unique_ptr<UndoAction>> actions_;
    int cursor_{0};  // 指向下一个可 redo 的位置（即当前栈顶 + 1）
    static constexpr int maxSize_{500};  // 最大历史层数
    bool isPerformingUndoRedo_{false};  // 防止 undo/redo 执行期间递归 addAction
    ChangeCallback onChange_;

    void notifyChange();
};

} // namespace OpenTune
