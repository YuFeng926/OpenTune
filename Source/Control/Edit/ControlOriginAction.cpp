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
    // The positions the clips are at start as the slots the actions are keyed by (see Inner::followed).
    for (auto& entry : inner_)
        if (entry.followed.empty())
            entry.followed = entry.tracks;
    if (history_ != nullptr) {
        barrierGeneration_ = history_->barrierGeneration();
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
        executed = applyAndFollow(*it, true) || executed;
    lastStepTookEffect_ = executed;
    onRedoSide_ = true;
}

void ControlOriginAction::redo()
{
    bool executed = false;
    for (auto& inner : inner_)
        executed = applyAndFollow(inner, false) || executed;
    lastStepTookEffect_ = executed;
    onRedoSide_ = false;
}

bool ControlOriginAction::applyAndFollow(Inner& inner, bool undo)
{
    const bool executed = apply(inner, undo);
    if (executed) {
        // The action was executed (not blocked by a mask), so it may have written to the slots it is keyed by and a
        // clip may now be at any of them. apply() cannot tell whether the upstream action then rejected the change
        // itself; that only costs a deletion report that was not needed. followed approximates where the clips are:
        // take the slots in (without duplicates), so that a deletion that hits a slot is never missed.
        for (const int slot : inner.tracks)
            if (std::find(inner.followed.begin(), inner.followed.end(), slot) == inner.followed.end())
                inner.followed.push_back(slot);
    }
    return executed;
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

bool ControlOriginAction::onTrackDeleted(int deletedTrack, int visibleCount)
{
    // The deleted track's clips are gone and every later shown track moved down by one position, so an action keyed
    // by one of these positions may now address another track. A hidden track (visibleCount and above) keeps its
    // position and its clips, and every track before the deleted one keeps its position. Two lists are compared, and
    // either one makes the entry possibly stale:
    // - tracks: the slots the wrapped OpenTune action is keyed by. They are fixed inside the action, so undo() and
    //   redo() write to these slots after any number of deletions; they are compared as they were recorded.
    // - followed: where the clips of the action are now. The clips of the shown tracks after the deleted one moved
    //   down with their tracks, so these are renumbered for the deletions that follow.
    // An action that addresses no track (a note edit addresses its content) or only tracks that did not move is
    // unaffected.
    const auto moved = [&](int track) { return track >= deletedTrack && track < visibleCount; };
    bool addresses = false;
    for (auto& inner : inner_) {
        for (const int track : inner.tracks)
            if (moved(track))
                addresses = true;
        for (int& track : inner.followed) {
            if (!moved(track))
                continue;
            addresses = true;
            // A track equal to the deleted one cannot be followed: its clip was removed with the track.
            if (track > deletedTrack)
                --track;
        }
    }
    if (!addresses || possiblyStale_)
        return false;
    possiblyStale_ = true;
    return true;
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
        // The history is empty, so no entry is left to be possibly stale (section 15.3 rule 6: opening a project
        // clears both histories, and with them every barrier).
        staleBelow_ = 0;
    }
    epoch_ = epoch;
    // A full history that changed since it was last observed may have dropped its oldest entry
    // (UndoManager::addAction() at its capacity), which moves every entry down by one position; that
    // position may now hold another entry, also one that took the memory of a dropped one. The positions of
    // UI entries can no longer be confirmed, so their undoIds are forgotten.
    const auto now = observe(manager);
    if (now.size >= kUndoHistoryCapacity && lastObserved_ != now) {
        uiEntries_.clear();
        // The positions moved down by one, so no entry can be told to sit at or above the mark: the count of a
        // later deletion then reports them as possibly stale, never too few.
        for (auto* action : actions_)
            action->markBelowStaleMark();
    }
    lastObserved_ = now;
    // A history that became shorter than the position the last deletion marked lost entries: a new entry may
    // have taken over a position below the mark, which then counts as possibly stale (see onTrackDeleted()).
    staleBelow_ = std::min(staleBelow_, now.size);
    uiEntries_.erase(std::remove_if(uiEntries_.begin(), uiEntries_.end(),
                                    [&manager](const UiEntry& entry) { return entry.index >= manager.size(); }),
                     uiEntries_.end());
}

void ControlHistory::noteOwnStep(const UndoManager& manager)
{
    lastObserved_ = observe(manager);
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
        entry.possiblyStale = origin->possiblyStale();
        return entry;
    }
    entry.unsafe = controlIsUnsafeUndoAction(action);
    const auto* known = findUiEntry(action, index);
    if (known != nullptr)
        entry.undoId = known->undoId;
    // The server cannot inspect an entry of the UI: one recorded before a track deletion is possibly stale until a
    // call with force applied it.
    entry.possiblyStale = index < staleBelow_ && !(known != nullptr && known->staleConfirmed);
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

ControlHistory::StaleCounts ControlHistory::onTrackDeleted(const UndoManager& manager, int deletedTrack,
                                                           int visibleCount, std::uint64_t epoch)
{
    prune(manager, epoch);
    StaleCounts counts;
    // UndoManager::cursor() and size() (read hook): the undo side holds the positions below the cursor.
    const int cursor = manager.cursor();
    const int size = manager.size();
    int freshControlUndo = 0;
    int freshControlRedo = 0;
    for (auto* action : actions_) {
        const bool redoSide = action->onRedoSide();
        if (action->barrierGeneration() == barrierGeneration_ && !action->belowStaleMark())
            ++(redoSide ? freshControlRedo : freshControlUndo);
        if (action->onTrackDeleted(deletedTrack, visibleCount))
            ++(redoSide ? counts.redo : counts.undo);
    }
    // Entries of the UI that were not possibly stale before: the positions at or above the last mark, except those
    // taken by entries of the API made since the last deletion, and the entries a call with force confirmed.
    int confirmedUndo = 0;
    int confirmedRedo = 0;
    for (const auto& entry : uiEntries_)
        if (entry.staleConfirmed && entry.index < staleBelow_)
            ++(entry.index < cursor ? confirmedUndo : confirmedRedo);
    const int freshUndo = std::max(0, cursor - staleBelow_);
    const int freshRedo = std::max(0, size - std::max(staleBelow_, cursor));
    counts.undo += std::max(0, freshUndo - freshControlUndo) + confirmedUndo;
    counts.redo += std::max(0, freshRedo - freshControlRedo) + confirmedRedo;

    staleBelow_ = size;
    for (auto& entry : uiEntries_)
        entry.staleConfirmed = false;
    ++barrierGeneration_;
    return counts;
}

void ControlHistory::confirmStale(const UndoManager& manager, const UndoAction& action, int index, std::uint64_t epoch,
                                  const std::string& instanceShort)
{
    for (auto* origin : actions_)
        if (static_cast<const UndoAction*>(origin) == &action) {
            origin->confirmStale();
            return;
        }
    // An entry of the UI: it needs an undoId to be told apart from now on.
    assignUndoId(manager, action, index, epoch, instanceShort);
    for (auto& entry : uiEntries_)
        if (entry.action == &action && entry.index == index)
            entry.staleConfirmed = true;
}

void ControlHistory::markContentsRemoved(const std::vector<ContentKey>& contents, std::uint64_t epoch)
{
    if (removedContentsEpoch_ != epoch) {
        removedContents_.clear();
        removedContentsEpoch_ = epoch;
    }
    for (const auto& key : contents)
        if (std::find(removedContents_.begin(), removedContents_.end(), key) == removedContents_.end())
            removedContents_.push_back(key);
}

void ControlHistory::reviveShownContents(const std::vector<ContentKey>& shown)
{
    removedContents_.erase(std::remove_if(removedContents_.begin(), removedContents_.end(),
                                          [&shown](const ContentKey& key) {
                                              return std::find(shown.begin(), shown.end(), key) != shown.end();
                                          }),
                           removedContents_.end());
}

bool ControlHistory::isContentRemoved(const ContentKey& content, std::uint64_t epoch) const
{
    return removedContentsEpoch_ == epoch
           && std::find(removedContents_.begin(), removedContents_.end(), content) != removedContents_.end();
}

void ControlHistory::record(UndoManager& manager, std::unique_ptr<ControlOriginAction> action)
{
    auto* const entry = action.get();
    manager.addAction(std::move(action));
    // UndoManager::addAction() drops the action during an undo or redo: mark only an entry that landed, compared
    // by address (UndoManager::peekUndo(), read hook) without touching the possibly destroyed action.
    if (manager.peekUndo() != entry)
        return;
    // UndoManager::cursor() (read hook): addAction() put the entry on the undo side, at cursor() - 1.
    if (manager.cursor() - 1 < staleBelow_)
        entry->markBelowStaleMark();
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
    // After these, OpenTune's reclaim sweep frees objects their undo needs (PROTOCOL.md section 15.4 rule 1).
    return dynamic_cast<const SplitPlacementAction*>(&action) != nullptr
           || dynamic_cast<const MergePlacementAction*>(&action) != nullptr
           || dynamic_cast<const DeletePlacementAction*>(&action) != nullptr;
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
