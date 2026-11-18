#include "UndoManager.h"

#include <juce_events/juce_events.h>
#include <algorithm>

namespace OpenTune {

void UndoManager::addAction(std::unique_ptr<UndoAction> action)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
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
    jassert(juce::MessageManager::existsAndIsCurrentThread());
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
    jassert(juce::MessageManager::existsAndIsCurrentThread());
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

void UndoManager::clearRedo()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (!isPerformingUndoRedo_ && cursor_ < static_cast<int>(actions_.size())) {
        actions_.erase(actions_.begin() + cursor_, actions_.end());
        if (historyPrunedCallback_)
            historyPrunedCallback_();
    }
}

void UndoManager::clear()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
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

int UndoManager::settleStructure(const UndoReplayState& currentState)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    std::vector<bool> keep(actions_.size(), false);
    UndoReplayState undoState = currentState;
    int undoCount = 0;
    for (int index = cursor_ - 1; index >= 0; --index) {
        if (actions_[static_cast<std::size_t>(index)]->replayStructure(undoState, true)) {
            keep[static_cast<std::size_t>(index)] = true;
            ++undoCount;
        }
    }

    UndoReplayState redoState = currentState;
    for (int index = cursor_; index < static_cast<int>(actions_.size()); ++index) {
        if (actions_[static_cast<std::size_t>(index)]->replayStructure(redoState, false))
            keep[static_cast<std::size_t>(index)] = true;
    }

    const int removed = static_cast<int>(std::count(keep.begin(), keep.end(), false));
    if (removed == 0)
        return 0;

    std::vector<std::unique_ptr<UndoAction>> compacted;
    compacted.reserve(actions_.size());
    for (std::size_t index = 0; index < actions_.size(); ++index) {
        if (keep[index])
            compacted.push_back(std::move(actions_[index]));
    }
    actions_ = std::move(compacted);
    cursor_ = undoCount;
    return removed;
}

int UndoManager::pruneActionsDependingOnContents(const std::vector<ContentKey>& contents)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (contents.empty())
        return 0;
    int removed = 0;
    for (int index = 0; index < static_cast<int>(actions_.size());) {
        const auto& action = *actions_[static_cast<std::size_t>(index)];
        const bool dependsOnReleasedContent = std::any_of(contents.begin(), contents.end(), [&action](ContentKey key) {
            return action.dependsOnContent(key);
        });
        if (!dependsOnReleasedContent) {
            ++index;
            continue;
        }
        if (index < cursor_)
            --cursor_;
        actions_.erase(actions_.begin() + index);
        ++removed;
    }
    return removed;
}

} // namespace OpenTune
