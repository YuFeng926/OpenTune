#include "UndoManager.h"

namespace OpenTune {

void UndoManager::addAction(std::unique_ptr<UndoAction> action)
{
    if (isPerformingUndoRedo_ || action == nullptr)
        return;

    // Erase anything after cursor (discard redo history)
    actions_.erase(actions_.begin() + cursor_, actions_.end());

    actions_.push_back(std::move(action));

    // Enforce capacity limit
    if (static_cast<int>(actions_.size()) > maxSize_) {
        actions_.erase(actions_.begin());
    } else {
        ++cursor_;
    }

    // cursor_ always points past the last action
    cursor_ = static_cast<int>(actions_.size());
    notifyChange();
}

UndoAction* UndoManager::undo()
{
    if (!canUndo())
        return nullptr;

    isPerformingUndoRedo_ = true;
    auto* action = actions_[--cursor_].get();
    action->undo();
    isPerformingUndoRedo_ = false;
    notifyChange();
    return action;
}

UndoAction* UndoManager::redo()
{
    if (!canRedo())
        return nullptr;

    isPerformingUndoRedo_ = true;
    auto* action = actions_[cursor_++].get();
    action->redo();
    isPerformingUndoRedo_ = false;
    notifyChange();
    return action;
}

bool UndoManager::canUndo() const
{
    return cursor_ > 0;
}

bool UndoManager::canRedo() const
{
    return cursor_ < static_cast<int>(actions_.size());
}

void UndoManager::clear()
{
    actions_.clear();
    cursor_ = 0;
    notifyChange();
}

juce::String UndoManager::getUndoDescription() const
{
    if (!canUndo())
        return {};
    return actions_[cursor_ - 1]->getDescription();
}

juce::String UndoManager::getRedoDescription() const
{
    if (!canRedo())
        return {};
    return actions_[cursor_]->getDescription();
}

void UndoManager::notifyChange()
{
    if (onChange_)
        onChange_();
}

} // namespace OpenTune
