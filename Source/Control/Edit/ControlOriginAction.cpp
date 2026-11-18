// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlOriginAction.h"

#include "Content/ContentEditCommands.h"
#include "Utils/AppLogger.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/PianoRollNotePatchAction.h"
#include "Utils/PitchShiftEditAction.h"
#include "Utils/PlacementActions.h"
#include "Utils/ScissorsUndoAction.h"
#include "Utils/TimeGridEditAction.h"
#include "Utils/VolumeEnvelopeEditAction.h"

#include <algorithm>

namespace OpenTune {

// ---- ControlOriginAction ----------------------------------------------------------------------------

ControlOriginAction::ControlOriginAction(std::shared_ptr<ControlHistory> history, std::string undoId,
                                         juce::String description, std::vector<Inner> inner)
    : history_(std::move(history))
    , undoId_(std::move(undoId))
    , description_(std::move(description))
    , inner_(std::move(inner))
{
    if (history_ != nullptr) {
        history_->add(this);
    }
}

ControlOriginAction::~ControlOriginAction()
{
    if (history_ != nullptr)
        history_->remove(this);
}

void ControlOriginAction::undo()
{
    bool executed = false;
    for (auto it = inner_.rbegin(); it != inner_.rend(); ++it)
        executed = apply(*it, true) || executed;
    lastStepTookEffect_ = executed;
}

void ControlOriginAction::redo()
{
    bool executed = false;
    for (auto& inner : inner_)
        executed = apply(inner, false) || executed;
    lastStepTookEffect_ = executed;
}

bool ControlOriginAction::retainsRetiredPlacement(uint64_t trackIdentity, uint64_t placementId) const
{
    return std::any_of(inner_.begin(), inner_.end(), [trackIdentity, placementId](const Inner& inner) {
        return inner.action != nullptr && inner.action->retainsRetiredPlacement(trackIdentity, placementId);
    });
}

bool ControlOriginAction::retainsRetiredContent(ContentKey key) const
{
    return std::any_of(inner_.begin(), inner_.end(), [key](const Inner& inner) {
        return inner.action != nullptr && inner.action->retainsRetiredContent(key);
    });
}

bool ControlOriginAction::dependsOnTrackIdentity(uint64_t identity) const
{
    return std::any_of(inner_.begin(), inner_.end(), [identity](const Inner& inner) {
        return inner.action != nullptr && inner.action->dependsOnTrackIdentity(identity);
    });
}

bool ControlOriginAction::dependsOnPlacement(uint64_t trackIdentity, uint64_t placementId) const
{
    return std::any_of(inner_.begin(), inner_.end(), [trackIdentity, placementId](const Inner& inner) {
        return inner.action != nullptr && inner.action->dependsOnPlacement(trackIdentity, placementId);
    });
}

bool ControlOriginAction::dependsOnContent(ContentKey key) const
{
    return std::any_of(inner_.begin(), inner_.end(), [key](const Inner& inner) {
        return inner.content == key
            || (inner.action != nullptr && inner.action->dependsOnContent(key));
    });
}

bool ControlOriginAction::dependenciesExist() const
{
    return std::all_of(inner_.begin(), inner_.end(), [](const Inner& inner) {
        return inner.action != nullptr && inner.action->dependenciesExist();
    });
}

bool ControlOriginAction::apply(Inner& inner, bool undo)
{
    if (inner.action == nullptr || masked(inner.content, inner.aspect))
        return false;
    if (inner.aspect == ControlContentAspect::PitchShift && inner.pitchShift && inner.commands != nullptr
        && masked(inner.content, ControlContentAspect::NotesAndPitch)) {
        // PitchShiftEditAction would put back the notes it saved; the notes of the content were replaced
        // since by a change without an undo entry. ContentEditCommands::commitPitchShiftEdit() sets the
        // settings and scales the current notes, as the pitch-shift dialog does; its new action is not kept.
        // It applies nothing and returns null when the content has no pitch curve or already has a shift of
        // the same total cents; this step then changes nothing.
        const auto applied = inner.commands->commitPitchShiftEdit(inner.content, undo ? inner.pitchShift->first
                                                                                      : inner.pitchShift->second);
        if (applied == nullptr) {
            AppLogger::logNoThrow(juce::String("control API: the ") + (undo ? "undo" : "redo")
                                  + " of a pitch shift changed nothing (no pitch curve, or the shift is already set)");
            return false;
        }
        return true;
    }
    if (undo)
        inner.action->undo();
    else
        inner.action->redo();
    return true;
}

std::vector<std::string> ControlOriginAction::innerActionNames() const
{
    std::vector<std::string> names;
    for (const auto& inner : inner_)
        names.push_back(inner.name);
    return names;
}

bool ControlOriginAction::unsafe() const
{
    return std::any_of(inner_.begin(), inner_.end(), [](const Inner& inner) {
        return inner.action != nullptr && controlIsUnsafeUndoAction(*inner.action);
    });
}

void ControlOriginAction::mask(const ContentKey& content, ControlContentAspect aspect)
{
    // A pitch shift's action restores the notes as well, so a change of the notes concerns it too.
    const bool touches = std::any_of(inner_.begin(), inner_.end(), [&](const Inner& inner) {
        return inner.content == content
               && (inner.aspect == aspect
                   || (aspect == ControlContentAspect::NotesAndPitch && inner.aspect == ControlContentAspect::PitchShift
                       && inner.pitchShift.has_value()));
    });
    if (touches && !masked(content, aspect))
        masked_.emplace_back(content, aspect);
}

bool ControlOriginAction::masked(const ContentKey& content, ControlContentAspect aspect) const
{
    return std::find(masked_.begin(), masked_.end(), std::make_pair(content, aspect)) != masked_.end();
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
        entry.unsafe = origin->unsafe();
        return entry;
    }
    entry.unsafe = controlIsUnsafeUndoAction(action);
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

void ControlHistory::onChangeWithoutEntry(UndoManager& manager, const ContentKey& content, ControlContentAspect aspect)
{
    for (auto* action : actions_)
        action->mask(content, aspect);
    // UndoManager::clearRedo() (hook): discards the entries after the cursor, as addAction() does.
    manager.clearRedo();
}

void ControlHistory::record(UndoManager& manager, std::unique_ptr<ControlOriginAction> action)
{
    manager.addAction(std::move(action));
}

void ControlHistory::add(ControlOriginAction* action)
{
    actions_.push_back(action);
}

void ControlHistory::remove(ControlOriginAction* action)
{
    actions_.erase(std::remove(actions_.begin(), actions_.end(), action), actions_.end());
}

// ---- Undo actions of OpenTune -------------------------------------------------------------------------

bool controlIsUnsafeUndoAction(const UndoAction& action)
{
    return dynamic_cast<const DeletePlacementAction*>(&action) != nullptr;
}

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
