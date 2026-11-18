// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Content/ContentKey.h"
#include "Utils/UndoManager.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <vector>

namespace OpenTune {

/**
 * One control API undo entry wraps the same upstream UndoActions as an OpenTune UI change and adds an undoId.
 * Undo runs inner actions in reverse order; redo runs them in order.
 *
 * Threading: message thread only, like the UndoManager.
 */
class ControlOriginAction final : public UndoAction
{
public:
    /** One upstream undo action and its reported class name. */
    struct Inner
    {
        std::unique_ptr<UndoAction> action;
        /** The OpenTune class name, reported in the undo descriptor's innerActions. */
        std::string name;
    };

    ControlOriginAction(std::string undoId, juce::String description, std::vector<Inner> inner);

    ControlOriginAction(const ControlOriginAction&) = delete;
    ControlOriginAction& operator=(const ControlOriginAction&) = delete;

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }
    bool replayStructure(UndoReplayState& state, bool undo) const override;
    bool retainsRetiredPlacement(uint64_t trackIdentity, uint64_t placementId) const override;
    bool retainsRetiredContent(ContentKey key) const override;
    bool dependsOnContent(ContentKey key) const override;

    const std::string& undoId() const noexcept { return undoId_; }
    /** The class names of the inner actions, in order. */
    std::vector<std::string> innerActionNames() const;

private:
    std::string undoId_;
    juce::String description_;
    std::vector<Inner> inner_;
};

/**
 * What the control API knows about OpenTune's undo history beyond the UndoManager: the undoIds it hands out and
 * the ids it assigned to entries created in the OpenTune UI when edit.undo or edit.redo applied them.
 *
 * An entry of the UI is recognised by its object, its class, its description and its position in the
 * history. OpenTune's history has no identity of its own for entries; if the user discards such an entry
 * in the UI and makes the same kind of edit at the same position, the new entry can take the old one's
 * place in memory and would carry its undoId. Opening a project forgets all of them (the content epoch
 * changes and OpenTune clears its history).
 *
 * UI entry indices are rebuilt from their action identities whenever the history changes. At capacity, UI undoIds
 * are forgotten because an action address may be reused after the oldest entry is destroyed.
 *
 * Threading: message thread only. Owned by ControlService.
 */
class ControlHistory
{
public:
    /** The number of entries OpenTune's UndoManager keeps (its private maxSize_); addAction() drops the oldest
        entry beyond it. */
    static constexpr int kUndoHistoryCapacity = 500;

    /** An entry of the history. */
    struct Entry
    {
        /** nullopt for an entry created in the UI that no edit.undo or edit.redo has applied. */
        std::optional<std::string> undoId;
        /** Recorded through the control API (origin control) or in the UI (origin ui). */
        bool control = false;
        bool unsafe = false;
        juce::String description;
    };

    /** A new undoId of the instance: u:<instanceShort>:<seq>, seq from 1. */
    std::string nextUndoId(const std::string& instanceShort);

    /** The entry at position index of the manager's history (0 is the oldest). epoch is the current content
        epoch: undoIds of UI entries do not survive opening a project. */
    Entry describe(const UndoManager& manager, const UndoAction& action, int index, std::uint64_t epoch);

    /** Gives a UI entry the next undoId and returns it; an entry that has one keeps it. */
    std::string assignUndoId(const UndoManager& manager, const UndoAction& action, int index, std::uint64_t epoch,
                             const std::string& instanceShort);

    /**
     * A change without an undo entry clears redo history so that no redo step writes older values over the change.
     */
    void onChangeWithoutEntry(UndoManager& manager);

    /** The API has just applied UndoManager::undo() or redo() itself, right after describe() checked the top:
        a step moves the cursor but no entry, so the history as it is now counts as observed. */
    void noteOwnStep(const UndoManager& manager);
    void synchronize(const UndoManager& manager, std::uint64_t epoch);

    /**
     * Pushes the entry onto OpenTune's history.
     */
    void record(UndoManager& manager, std::unique_ptr<ControlOriginAction> action);

private:
    struct UiEntry
    {
        const UndoAction* action = nullptr;
        int index = 0;
        std::type_index type = typeid(void);
        juce::String description;
        std::string undoId;
    };

    /** What the read hooks show of the history. */
    struct Observation
    {
        int size = 0;
        int cursor = 0;
        const UndoAction* undoTop = nullptr;
        const UndoAction* redoTop = nullptr;

        bool operator==(const Observation& other) const
        {
            return size == other.size && cursor == other.cursor && undoTop == other.undoTop && redoTop == other.redoTop;
        }
        bool operator!=(const Observation& other) const { return !(*this == other); }
    };

    static Observation observe(const UndoManager& manager);
    void prune(const UndoManager& manager, std::uint64_t epoch);
    const UiEntry* findUiEntry(const UndoAction& action, int index) const;

    std::uint64_t nextSeq_ = 1;
    std::optional<std::uint64_t> epoch_;
    std::optional<Observation> lastObserved_;
    std::vector<UiEntry> uiEntries_;
};

/** The OpenTune class name of an upstream undo action, for innerActions ("UndoAction" when unknown). */
std::string controlUndoActionName(const UndoAction& action);

} // namespace OpenTune
