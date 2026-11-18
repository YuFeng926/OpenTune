// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Content/ContentKey.h"
#include "Utils/PitchShiftSettings.h"
#include "Utils/UndoManager.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace OpenTune {

class ContentEditCommands;
class ControlHistory;

/** The aspects of a content that an undo entry recorded through the control API puts back. */
enum class ControlContentAspect {
    /** The notes with the corrected pitch curve (PianoRollEditAction). */
    NotesAndPitch,
    Key,
    PitchShift,
    TimeGrid
};

/**
 * One undo entry made through the control API: the undo actions the OpenTune UI pushes for the same change
 * (every upstream UndoAction class has a public constructor), wrapped with the entry's undoId, so that
 * edit.undo and edit.redo can tell whether the top of the history is this entry, and which aspects of which
 * contents it puts back.
 *
 * undo() runs the inner actions in reverse order and redo() in order, as OpenTune would run them one by one.
 * An inner action is skipped once a later change without an undo entry (content.setKey, content.autoTune in
 * mode notesOnly) changed the aspect of the content it puts back: such a change is never taken back, so the
 * entry leaves that aspect as the later change set it.
 *
 * A pitch shift is the exception: OpenTune's PitchShiftEditAction puts back the notes and correction segments
 * it saved together with the settings. Once a change without an undo entry replaced the notes of its content,
 * the entry puts back the settings only, as content.setPitchShift sets them: through
 * ContentEditCommands::commitPitchShiftEdit(), which scales the notes the content has now (the action that
 * call returns is discarded, this entry is already on the history). That call applies nothing when the content
 * has no pitch curve or already has a shift of the same total cents; the step then has no effect, which
 * lastStepTookEffect() tells and AppLogger records.
 *
 * The entry registers itself with the instance's ControlHistory for as long as it exists. OpenTune's
 * UndoManager owns it and destroys it when it leaves the history; the entry keeps the history alive through
 * a shared pointer, because the processor destroys its UndoManager after its control service.
 *
 * Threading: message thread only, like the UndoManager.
 */
class ControlOriginAction final : public UndoAction
{
public:
    /** One upstream undo action and what it puts back. */
    struct Inner
    {
        std::unique_ptr<UndoAction> action;
        /** The OpenTune class name, reported in the undo descriptor's innerActions. */
        std::string name;
        ContentKey content;
        ControlContentAspect aspect = ControlContentAspect::NotesAndPitch;
        /** Aspect PitchShift: the settings before and after the shift, and the content commands that apply
            them while the notes of the content are masked (see the class comment). */
        std::optional<std::pair<PitchShiftSettings, PitchShiftSettings>> pitchShift;
        std::shared_ptr<ContentEditCommands> commands;
    };

    ControlOriginAction(std::shared_ptr<ControlHistory> history, std::string undoId, juce::String description,
                        std::vector<Inner> inner);
    ~ControlOriginAction() override;

    ControlOriginAction(const ControlOriginAction&) = delete;
    ControlOriginAction& operator=(const ControlOriginAction&) = delete;

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }
    bool retainsRetiredPlacement(uint64_t trackIdentity, uint64_t placementId) const override;
    bool retainsRetiredContent(ContentKey key) const override;
    bool dependsOnTrackIdentity(uint64_t identity) const override;
    bool dependsOnPlacement(uint64_t trackIdentity, uint64_t placementId) const override;
    bool dependsOnContent(ContentKey key) const override;
    bool dependenciesExist() const override;

    const std::string& undoId() const noexcept { return undoId_; }
    /** The class names of the inner actions, in order. */
    std::vector<std::string> innerActionNames() const;
    /** True when an inner action cannot safely restore a deleted clip. */
    bool unsafe() const;
    /** False when the last undo() or redo() applied none of the inner actions: every one was masked, or the
        pitch shift it puts back through commitPitchShiftEdit() applied nothing (see the class comment). The step
        then changed nothing, and edit.undo / edit.redo report UNDO_NO_EFFECT. An
        action that was executed counts even if the upstream action rejected the change on its own: apply() cannot
        tell. True before the first step. */
    bool lastStepTookEffect() const noexcept { return lastStepTookEffect_; }
    /** A change without an undo entry changed this aspect of the content: from now on the entry no longer
        puts it back. Called by ControlHistory. */
    void mask(const ContentKey& content, ControlContentAspect aspect);

private:
    bool masked(const ContentKey& content, ControlContentAspect aspect) const;
    /** Undoes (undo true) or redoes one inner action, as the masks of its content allow; false when it applied
        nothing. */
    bool apply(Inner& inner, bool undo);
    std::shared_ptr<ControlHistory> history_;
    std::string undoId_;
    juce::String description_;
    std::vector<Inner> inner_;
    std::vector<std::pair<ContentKey, ControlContentAspect>> masked_;
    bool lastStepTookEffect_ = true;
};

/**
 * What the control API knows about OpenTune's undo history beyond the UndoManager: the undoIds it hands out,
 * the entries it recorded (each ControlOriginAction registers itself), and
 * the undoIds it assigned to entries created in the OpenTune UI when edit.undo or edit.redo applied them
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
 * Threading: message thread only. Owned by ControlService and shared with every ControlOriginAction.
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
     * A change without an undo entry changed this aspect of the content: entries recorded through the API no
     * longer put it back, and the redo history is cleared, so that no redo step writes older values over the
     * change. UndoManager::clearRedo() is the hook that clears it.
     */
    void onChangeWithoutEntry(UndoManager& manager, const ContentKey& content, ControlContentAspect aspect);

    /** The API has just applied UndoManager::undo() or redo() itself, right after describe() checked the top:
        a step moves the cursor but no entry, so the history as it is now counts as observed. */
    void noteOwnStep(const UndoManager& manager);
    void synchronize(const UndoManager& manager, std::uint64_t epoch);

    /**
     * Pushes the entry onto OpenTune's history and tracks its action identity for history descriptions.
     */
    void record(UndoManager& manager, std::unique_ptr<ControlOriginAction> action);

    /** Called by ControlOriginAction for its lifetime. */
    void add(ControlOriginAction* action);
    void remove(ControlOriginAction* action);

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
    std::vector<ControlOriginAction*> actions_;
};

/** True for the undo action OpenTune cannot undo correctly: DeletePlacementAction. */
bool controlIsUnsafeUndoAction(const UndoAction& action);

/** The OpenTune class name of an upstream undo action, for innerActions ("UndoAction" when unknown). */
std::string controlUndoActionName(const UndoAction& action);

} // namespace OpenTune
