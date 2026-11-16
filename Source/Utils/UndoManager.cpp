#include "UndoManager.h"

namespace OpenTune {

void UndoManager::addAction(std::unique_ptr<UndoAction> action)
{
    if (isPerformingUndoRedo_ || action == nullptr)
        return;

    const bool discardedRedo = cursor_ < static_cast<int>(actions_.size());
    const bool discardedOldest = static_cast<int>(actions_.size()) == maxSize_;
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
    if ((discardedRedo || discardedOldest) && historyPrunedCallback_)
        historyPrunedCallback_();
}

UndoAction* UndoManager::undo()
{
    if (!canUndo())
        return nullptr;

    isPerformingUndoRedo_ = true;
    auto* action = actions_[--cursor_].get();
    action->undo();
    isPerformingUndoRedo_ = false;
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
    const bool hadActions = !actions_.empty();
    actions_.clear();
    cursor_ = 0;
    if (hadActions && historyPrunedCallback_)
        historyPrunedCallback_();
}

bool UndoManager::retainsRetiredPlacement(int trackId, uint64_t placementId) const
{
    for (const auto& action : actions_)
        if (action->retainsRetiredPlacement(trackId, placementId))
            return true;
    return false;
}

bool UndoManager::retainsRetiredContent(ContentKey key) const
{
    for (const auto& action : actions_)
        if (action->retainsRetiredContent(key))
            return true;
    return false;
}

} // namespace OpenTune
