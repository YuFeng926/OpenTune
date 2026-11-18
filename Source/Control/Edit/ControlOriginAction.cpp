// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlOriginAction.h"

#include "Utils/PianoRollEditAction.h"
#include "Utils/PianoRollNotePatchAction.h"
#include "Utils/PitchShiftEditAction.h"
#include "Utils/PlacementActions.h"
#include "Utils/ScissorsUndoAction.h"
#include "Utils/TimeGridEditAction.h"
#include "Utils/VolumeEnvelopeEditAction.h"

#include <juce_events/juce_events.h>
#include <algorithm>
#include <utility>

namespace OpenTune {

// ---- ControlOriginAction ----------------------------------------------------------------------------

ControlOriginAction::ControlOriginAction(std::string undoId, juce::String description, std::vector<Inner> inner)
    : undoId_(std::move(undoId))
    , description_(std::move(description))
    , inner_(std::move(inner))
{
    jassert(std::all_of(inner_.begin(), inner_.end(), [](const Inner& inner) { return inner.action != nullptr; }));
}

void ControlOriginAction::undo()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    for (auto it = inner_.rbegin(); it != inner_.rend(); ++it)
        it->action->undo();
}

void ControlOriginAction::redo()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    for (auto& inner : inner_)
        inner.action->redo();
}

bool ControlOriginAction::replayStructure(UndoReplayState& state, bool undo) const
{
    auto candidate = state;
    if (undo) {
        for (auto it = inner_.rbegin(); it != inner_.rend(); ++it)
            if (!it->action->replayStructure(candidate, true))
                return false;
    } else {
        for (const auto& inner : inner_)
            if (!inner.action->replayStructure(candidate, false))
                return false;
    }
    state = std::move(candidate);
    return true;
}

bool ControlOriginAction::retainsRetiredPlacement(uint64_t trackIdentity, uint64_t placementId) const
{
    return std::any_of(inner_.begin(), inner_.end(), [trackIdentity, placementId](const Inner& inner) {
        return inner.action->retainsRetiredPlacement(trackIdentity, placementId);
    });
}

bool ControlOriginAction::retainsRetiredContent(ContentKey key) const
{
    return std::any_of(inner_.begin(), inner_.end(), [key](const Inner& inner) {
        return inner.action->retainsRetiredContent(key);
    });
}

bool ControlOriginAction::dependsOnContent(ContentKey key) const
{
    return std::any_of(inner_.begin(), inner_.end(), [key](const Inner& inner) {
        return inner.action->dependsOnContent(key);
    });
}

std::vector<std::string> ControlOriginAction::innerActionNames() const
{
    std::vector<std::string> names;
    for (const auto& inner : inner_)
        names.push_back(inner.name);
    return names;
}

// ---- ControlHistory ---------------------------------------------------------------------------------

std::string ControlHistory::nextUndoId(const std::string& instanceShort)
{
    return "u:" + instanceShort + ":" + std::to_string(nextSeq_++);
}

ControlHistory::Observation ControlHistory::observe(const UndoManager& manager)
{
    // UndoManager::size(), cursor(), peekUndo() and peekRedo(): the read hook on OpenTune's history.
    return {manager.size(), manager.cursor(), manager.peekUndo(), manager.peekRedo()};
}

void ControlHistory::prune(const UndoManager& manager, std::uint64_t epoch)
{
    // Opening a project clears OpenTune's history (ProjectSession::commitPreparedOpen) and starts a new
    // content epoch; an entry beyond the history's end left it.
    if (epoch_ != epoch) {
        uiEntries_.clear();
        lastObserved_.reset();
    }
    epoch_ = epoch;
    // A full history that changed since it was last observed may have dropped its oldest entry
    // (UndoManager::addAction() at its capacity), which moves every entry down by one position; that
    // position may now hold another entry, also one that took the memory of a dropped one. The positions of
    // UI entries can no longer be confirmed, so their undoIds are forgotten.
    const auto now = observe(manager);
    if (now.size >= kUndoHistoryCapacity && lastObserved_ != now) {
        uiEntries_.clear();
    }
    lastObserved_ = now;
    uiEntries_.erase(std::remove_if(uiEntries_.begin(), uiEntries_.end(), [&manager](UiEntry& entry) {
                         const int index = manager.indexOf(entry.action);
                         if (index < 0)
                             return true;
                         entry.index = index;
                         return false;
                     }),
                     uiEntries_.end());
}

void ControlHistory::noteOwnStep(const UndoManager& manager)
{
    lastObserved_ = observe(manager);
}

void ControlHistory::synchronize(const UndoManager& manager, std::uint64_t epoch)
{
    prune(manager, epoch);
}

const ControlHistory::UiEntry* ControlHistory::findUiEntry(const UndoAction& action, int index) const
{
    for (const auto& entry : uiEntries_)
        if (entry.action == &action && entry.index == index && entry.type == std::type_index(typeid(action))
            && entry.description == action.getDescription())
            return &entry;
    return nullptr;
}

ControlHistory::Entry ControlHistory::describe(const UndoManager& manager, const UndoAction& action, int index,
                                               std::uint64_t epoch)
{
    prune(manager, epoch);
    Entry entry;
    entry.description = action.getDescription();
    if (const auto* origin = dynamic_cast<const ControlOriginAction*>(&action)) {
        entry.undoId = origin->undoId();
        entry.control = true;
        return entry;
    }
    const auto* known = findUiEntry(action, index);
    if (known != nullptr)
        entry.undoId = known->undoId;
    return entry;
}

std::string ControlHistory::assignUndoId(const UndoManager& manager, const UndoAction& action, int index,
                                         std::uint64_t epoch, const std::string& instanceShort)
{
    const auto entry = describe(manager, action, index, epoch);
    if (entry.undoId)
        return *entry.undoId;
    UiEntry known;
    known.action = &action;
    known.index = index;
    known.type = std::type_index(typeid(action));
    known.description = action.getDescription();
    known.undoId = nextUndoId(instanceShort);
    uiEntries_.push_back(known);
    return known.undoId;
}

void ControlHistory::onChangeWithoutEntry(UndoManager& manager)
{
    // UndoManager::clearRedo() (hook): discards the entries after the cursor, as addAction() does.
    manager.clearRedo();
}

void ControlHistory::record(UndoManager& manager, std::unique_ptr<ControlOriginAction> action)
{
    manager.addAction(std::move(action));
}

// ---- Undo actions of OpenTune -------------------------------------------------------------------------

std::string controlUndoActionName(const UndoAction& action)
{
    if (dynamic_cast<const PianoRollEditAction*>(&action) != nullptr)
        return "PianoRollEditAction";
    if (dynamic_cast<const PianoRollNotePatchAction*>(&action) != nullptr)
        return "PianoRollNotePatchAction";
    if (dynamic_cast<const PitchShiftEditAction*>(&action) != nullptr)
        return "PitchShiftEditAction";
    if (dynamic_cast<const TimeGridEditAction*>(&action) != nullptr)
        return "TimeGridEditAction";
    if (dynamic_cast<const ScissorsUndoAction*>(&action) != nullptr)
        return "ScissorsUndoAction";
    if (dynamic_cast<const VolumeEnvelopeEditAction*>(&action) != nullptr)
        return "VolumeEnvelopeEditAction";
    if (dynamic_cast<const SplitPlacementAction*>(&action) != nullptr)
        return "SplitPlacementAction";
    if (dynamic_cast<const MergePlacementAction*>(&action) != nullptr)
        return "MergePlacementAction";
    if (dynamic_cast<const DeletePlacementAction*>(&action) != nullptr)
        return "DeletePlacementAction";
    if (dynamic_cast<const MultiMovePlacementAction*>(&action) != nullptr)
        return "MultiMovePlacementAction";
    if (dynamic_cast<const GainChangeAction*>(&action) != nullptr)
        return "GainChangeAction";
    if (dynamic_cast<const TrimPlacementAction*>(&action) != nullptr)
        return "TrimPlacementAction";
    if (dynamic_cast<const FadeChangeAction*>(&action) != nullptr)
        return "FadeChangeAction";
    return "UndoAction";
}

} // namespace OpenTune
