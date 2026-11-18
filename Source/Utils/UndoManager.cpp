#include "UndoManager.h"

#include <algorithm>

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
    pruneInvalidActions();
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
    pruneInvalidActions();
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

bool UndoManager::retainsRetiredPlacement(uint64_t trackIdentity, uint64_t placementId) const
{
    for (const auto& action : actions_)
        if (action->retainsRetiredPlacement(trackIdentity, placementId))
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

int UndoManager::indexOf(const UndoAction* action) const
{
    for (int index = 0; index < static_cast<int>(actions_.size()); ++index)
        if (actions_[static_cast<std::size_t>(index)].get() == action)
            return index;
    return -1;
}

int UndoManager::pruneActions(const std::function<bool(const UndoAction&)>& shouldPrune, bool notify)
{
    int removed = 0;
    for (int index = 0; index < static_cast<int>(actions_.size());) {
        if (!shouldPrune(*actions_[static_cast<std::size_t>(index)])) {
            ++index;
            continue;
        }
        if (index < cursor_)
            --cursor_;
        actions_.erase(actions_.begin() + index);
        ++removed;
    }
    if (removed != 0) {
        if (notify && historyPrunedCallback_)
            historyPrunedCallback_();
    }
    return removed;
}

UndoManager::PruneResult UndoManager::pruneForTrackDeletion(
    uint64_t trackIdentity, const std::vector<std::pair<uint64_t, uint64_t>>& placements,
    const std::vector<ContentKey>& removedContents,
    const std::function<bool(ContentKey)>& hasActiveReference)
{
    PruneResult result;
    auto markAndPrune = [this, &result](const std::function<bool(const UndoAction&)>& predicate) {
        const int cursorBefore = cursor_;
        const int removed = pruneActions(predicate, false);
        result.undo += cursorBefore - cursor_;
        result.redo += removed - (cursorBefore - cursor_);
    };

    markAndPrune([trackIdentity, &placements](const UndoAction& action) {
        if (!action.dependenciesExist() || action.dependsOnTrackIdentity(trackIdentity))
            return true;
        return std::any_of(placements.begin(), placements.end(), [&action](const auto& placement) {
            return action.dependsOnPlacement(placement.first, placement.second);
        });
    });

    for (const auto key : removedContents) {
        if (hasActiveReference(key))
            continue;
        const bool heldByHistory = std::any_of(actions_.begin(), actions_.end(), [key](const auto& action) {
            return action->retainsRetiredContent(key);
        });
        if (heldByHistory)
            continue;
        markAndPrune([key](const UndoAction& action) { return action.dependsOnContent(key); });
    }
    return result;
}

int UndoManager::pruneForContentRelease(ContentKey content)
{
    return pruneActions([content](const UndoAction& action) { return action.dependsOnContent(content); }, false);
}

int UndoManager::pruneInvalidActions(bool notify)
{
    return pruneActions([](const UndoAction& action) { return !action.dependenciesExist(); }, notify);
}

} // namespace OpenTune
