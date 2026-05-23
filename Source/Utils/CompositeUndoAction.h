#pragma once

#include "UndoManager.h"
#include <memory>
#include <vector>

namespace OpenTune {

/**
 * @brief 复合 Undo Action，将多个子 action 打包为一个原子操作。
 *
 * undo() 逆序执行子 action 的 undo，redo() 正序执行子 action 的 redo。
 * 对 UndoManager 而言，CompositeUndoAction 只计数为 1 层（不压缩内部 action 数）。
 *
 * 应用场景：AUTO (Ref) 时需要同时修改 pitch (notes/segments) 和 timeGrid，
 *           这两项修改必须作为一次 undo/redo。
 */
class CompositeUndoAction : public UndoAction {
public:
    CompositeUndoAction() = default;
    explicit CompositeUndoAction(const juce::String& description);
    ~CompositeUndoAction() override = default;

    /** 添加子 action（转移所有权） */
    void addAction(std::unique_ptr<UndoAction> action);

    /** 逆序 undo 所有子 action */
    void undo() override;

    /** 正序 redo 所有子 action */
    void redo() override;

    /** 返回描述文本 */
    juce::String getDescription() const override;

    /** 子 action 数量 */
    int getNumActions() const;

    /** 设置描述 */
    void setDescription(const juce::String& description);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CompositeUndoAction)

private:
    std::vector<std::unique_ptr<UndoAction>> actions_;
    juce::String description_;
};

// ============================================================================
// Inline implementations (header-only)
// ============================================================================

inline CompositeUndoAction::CompositeUndoAction(const juce::String& description)
    : description_(description)
{
}

inline void CompositeUndoAction::addAction(std::unique_ptr<UndoAction> action)
{
    actions_.push_back(std::move(action));
}

inline void CompositeUndoAction::undo()
{
    // 逆序执行：最后添加的 action 最先撤销
    for (auto it = actions_.rbegin(); it != actions_.rend(); ++it) {
        (*it)->undo();
    }
}

inline void CompositeUndoAction::redo()
{
    // 正序执行：先添加的 action 先重做
    for (auto& action : actions_) {
        action->redo();
    }
}

inline juce::String CompositeUndoAction::getDescription() const
{
    return description_;
}

inline int CompositeUndoAction::getNumActions() const
{
    return static_cast<int>(actions_.size());
}

inline void CompositeUndoAction::setDescription(const juce::String& description)
{
    description_ = description;
}

} // namespace OpenTune
