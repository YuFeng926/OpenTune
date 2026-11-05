// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "PitchShiftLedger.h"

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

/** The aspects of a content that an undo entry recorded through the control API puts back (PROTOCOL.md
    section 15.3 rule 9). */
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
 * entry leaves that aspect as the later change set it (section 15.3 rule 9).
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
        /** The track slots the wrapped OpenTune action is keyed by (a clip move or trim, a split, a merge, ...):
            empty for an action that addresses a content only. The action keeps these numbers for good (for example
            MultiMovePlacementAction::Entry::sourceTrackId), so undo() and redo() write to these slots whatever
            track deletions happened since. Never changed after construction. */
        std::vector<int> tracks;
        /** An approximation of the positions the clips of the action are at now: a copy of tracks when the entry is
            made, renumbered by the barrier of every track deletion for the shown tracks that moved down
            (removeTrackAndShift()). After an undo() or redo() in which the action was executed (not blocked by a mask)
            the slots of tracks are merged into it (without duplicates), so followed covers tracks from then on, and a
            deletion that hits one of them reports the entry possibly stale (section 15.3 rule 6: too often rather than
            too rarely). "Executed" is all that is known: the value apply() returns does not reflect an upstream action
            that rejects the change on its own (for example because its clip is gone), which only makes followed cover
            more tracks than needed. The real protection comes from tracks: the action reads and writes only those
            slots. Filled by the constructor when left empty. */
        std::vector<int> followed;
    };

    ControlOriginAction(std::shared_ptr<ControlHistory> history, std::string undoId, juce::String description,
                        std::vector<Inner> inner);
    ~ControlOriginAction() override;

    ControlOriginAction(const ControlOriginAction&) = delete;
    ControlOriginAction& operator=(const ControlOriginAction&) = delete;

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

    const std::string& undoId() const noexcept { return undoId_; }
    /** The class names of the inner actions, in order. */
    std::vector<std::string> innerActionNames() const;
    /** True when an inner action is a clip split, merge or delete (section 15.4 rule 1). */
    bool unsafe() const;
    /** False when the last undo() or redo() applied none of the inner actions: every one was masked, or the
        pitch shift it puts back through commitPitchShiftEdit() applied nothing (see the class comment). The step
        then changed nothing, and edit.undo / edit.redo report UNDO_NO_EFFECT (section 15.3 rules 5 and 9). An
        action that was executed counts even if the upstream action rejected the change on its own: apply() cannot
        tell. True before the first step. */
    bool lastStepTookEffect() const noexcept { return lastStepTookEffect_; }
    /** A change without an undo entry changed this aspect of the content: from now on the entry no longer
        puts it back. Called by ControlHistory. */
    void mask(const ContentKey& content, ControlContentAspect aspect);

    /** A track was deleted (section 15.3 rule 6); visibleCount is the number of shown tracks before the deletion.
        The entry becomes possibly stale when a track it is keyed by (Inner::tracks) or a position its clips are at
        (Inner::followed) is the deleted one or one of the shown tracks after it (removeTrackAndShift() moves only the
        shown tracks after the deleted one down; a hidden track keeps its position and its clips). Only the followed
        positions are renumbered to where the clips are now: the slots of the wrapped action never change. An entry
        that addresses no track, only tracks before the deleted one or only hidden tracks stays as it is. True when
        the entry was not possibly stale before and is now. Called by ControlHistory. */
    bool onTrackDeleted(int deletedTrack, int visibleCount);
    /** True from a track deletion that made the entry possibly stale until a call with force applied the entry. */
    bool possiblyStale() const noexcept { return possiblyStale_; }
    /** The end user confirmed the entry by asking for its step with force: it is no longer possibly stale. */
    void confirmStale() noexcept { possiblyStale_ = false; }
    /** True while the entry sits on the redo side of the history: after its undo() and before its next redo().
        A new entry starts on the undo side. */
    bool onRedoSide() const noexcept { return onRedoSide_; }
    /** The value of ControlHistory::barrierGeneration() when the entry was made: an entry made after the last
        track deletion carries the current value. */
    std::uint64_t barrierGeneration() const noexcept { return barrierGeneration_; }
    /** True when the entry sits at a position below the history's stale mark (ControlHistory::record() found
        it so, or the history was full and positions moved): the entry is newer than the last track deletion, but
        its position still counts among the positions recorded before it. A track deletion then must not take it
        off the count of newer positions (see ControlHistory::onTrackDeleted()). */
    bool belowStaleMark() const noexcept { return belowStaleMark_; }
    /** Called by ControlHistory. */
    void markBelowStaleMark() noexcept { belowStaleMark_ = true; }

private:
    bool masked(const ContentKey& content, ControlContentAspect aspect) const;
    /** Undoes (undo true) or redoes one inner action, as the masks of its content allow; false when it applied
        nothing. */
    bool apply(Inner& inner, bool undo);
    /** apply(), then merges the slots of the inner action into followed when the action was executed (not blocked
        by a mask). The value apply() returns does not reflect an upstream action that rejects the change itself. */
    bool applyAndFollow(Inner& inner, bool undo);

    std::shared_ptr<ControlHistory> history_;
    std::string undoId_;
    juce::String description_;
    std::vector<Inner> inner_;
    std::vector<std::pair<ContentKey, ControlContentAspect>> masked_;
    bool lastStepTookEffect_ = true;
    bool possiblyStale_ = false;
    bool onRedoSide_ = false;
    bool belowStaleMark_ = false;
    std::uint64_t barrierGeneration_ = 0;
};

/**
 * What the control API knows about OpenTune's undo history beyond the UndoManager (PROTOCOL.md section
 * 15.3): the undoIds it hands out, the entries it recorded (each ControlOriginAction registers itself), and
 * the undoIds it assigned to entries created in the OpenTune UI when edit.undo or edit.redo applied them
 * (rule 8).
 *
 * An entry of the UI is recognised by its object, its class, its description and its position in the
 * history. OpenTune's history has no identity of its own for entries; if the user discards such an entry
 * in the UI and makes the same kind of edit at the same position, the new entry can take the old one's
 * place in memory and would carry its undoId. Opening a project forgets all of them (the content epoch
 * changes and OpenTune clears its history).
 *
 * A full history moves every position: once it holds kUndoHistoryCapacity entries, UndoManager::addAction()
 * drops the oldest one and every entry moves down by one, and the read hooks cannot tell that apart from an
 * undo followed by a new entry. Whenever the history is found full and changed since it was last observed,
 * the undoIds of UI entries are therefore forgotten: such an entry is never confirmed at a position that may
 * hold another entry, and a call that names it fails with UNDO_CONFLICT(top_mismatch) instead. Steps the API
 * applies itself (noteOwnStep) move no position and keep them.
 *
 * Threading: message thread only. Owned by ControlService and shared with every ControlOriginAction.
 */
class ControlHistory
{
public:
    /** The number of entries OpenTune's UndoManager keeps (its private maxSize_); addAction() drops the oldest
        entry beyond it. */
    static constexpr int kUndoHistoryCapacity = 500;

    /** An entry of the history as the protocol describes it (common.schema.json#/$defs/historyEntry). */
    struct Entry
    {
        /** nullopt for an entry created in the UI that no edit.undo or edit.redo has applied. */
        std::optional<std::string> undoId;
        /** Recorded through the control API (origin control) or in the UI (origin ui). */
        bool control = false;
        bool unsafe = false;
        /** Recorded before a track deletion that may have moved a track the entry addresses (section 15.3 rule 6):
            an entry made through the API when the deletion removed or moved a track it addresses, an entry of the
            UI when it was recorded before the deletion, since the server cannot inspect it. */
        bool possiblyStale = false;
        juce::String description;
    };

    /** A new undoId of the instance: u:<instanceShort>:<seq>, seq from 1 (PROTOCOL.md section 9). */
    std::string nextUndoId(const std::string& instanceShort);

    /** The entry at position index of the manager's history (0 is the oldest). epoch is the current content
        epoch: undoIds of UI entries do not survive opening a project. */
    Entry describe(const UndoManager& manager, const UndoAction& action, int index, std::uint64_t epoch);

    /** Gives a UI entry the next undoId (rule 8) and returns it; an entry that has one keeps it. */
    std::string assignUndoId(const UndoManager& manager, const UndoAction& action, int index, std::uint64_t epoch,
                             const std::string& instanceShort);

    /**
     * A change without an undo entry changed this aspect of the content: entries recorded through the API no
     * longer put it back, and the redo history is cleared, so that no redo step writes older values over the
     * change (section 15.3 rule 9). UndoManager::clearRedo() is the hook that clears it.
     */
    void onChangeWithoutEntry(UndoManager& manager, const ContentKey& content, ControlContentAspect aspect);

    /** The API has just applied UndoManager::undo() or redo() itself, right after describe() checked the top:
        a step moves the cursor but no entry, so the history as it is now counts as observed. */
    void noteOwnStep(const UndoManager& manager);

    /**
     * Pushes the entry onto OpenTune's history (UndoManager::addAction(), which discards the redo history) and
     * notes where it landed: UndoManager::cursor() minus one is its position. An entry that lands below the
     * position a track deletion marked (the history was undone past the mark first) is flagged
     * (ControlOriginAction::belowStaleMark()), so that the next deletion's count does not subtract it from the
     * positions above the mark. Every push of the API goes through here.
     */
    void record(UndoManager& manager, std::unique_ptr<ControlOriginAction> action);

    /** Called by ControlOriginAction for its lifetime. */
    void add(ControlOriginAction* action);
    void remove(ControlOriginAction* action);

    /** The pitch-shift steps that the revision tokens leave out (controlContentVersions()). */
    ControlPitchShiftLedger& pitchShiftLedger() noexcept { return pitchShiftLedger_; }

    // ---- The barrier of a track deletion (section 15.3 rule 6) ----------------------------------------

    /** How many entries of each side of the history a track deletion made possibly stale. */
    struct StaleCounts
    {
        int undo = 0;
        int redo = 0;
    };

    /**
     * A track was deleted: every entry that may address a track which moved is possibly stale from now on.
     * visibleCount is the number of shown tracks before the deletion. Only the shown tracks after the deleted one
     * move: removeTrackAndShift() shifts the shown tracks down and leaves the hidden slots where they are, their
     * clips and their mix with them, so an entry that addresses a hidden track only is not possibly stale. An entry of
     * the API compares both the slots its OpenTune actions are keyed by, which never change, and the positions its
     * clips moved to, which are renumbered (ControlOriginAction::onTrackDeleted()), so that a later deletion sees
     * where the clips are then and where undo() would write. An entry recorded in the UI cannot be
     * inspected, so each of them recorded before the deletion is possibly stale. OpenTune's UndoManager gives no
     * access to an entry by position, only to its two tops, its size and its cursor, so an entry of the UI is told by
     * its position: every position below the size the history had at the deletion holds an entry recorded before it.
     * A position that a later entry took over after an undo keeps counting until prune() sees the history shorter;
     * that can report too many entries as possibly stale, never too few. An entry of the API that was made below the
     * mark is not subtracted from those positions either (record()). A deletion made in the UI itself is not
     * observed (there is no hook for it).
     * Returns how many entries on each side the deletion newly made possibly stale: an entry that already was
     * possibly stale is not counted again; one that a call with force had confirmed is.
     */
    StaleCounts onTrackDeleted(const UndoManager& manager, int deletedTrack, int visibleCount, std::uint64_t epoch);

    /** The end user asked for the step of this entry with force: it is no longer possibly stale, until another
        track deletion makes it so again (section 15.3 rule 6). An entry of the UI gets its undoId first
        (assignUndoId), so that it can be told apart. */
    void confirmStale(const UndoManager& manager, const UndoAction& action, int index, std::uint64_t epoch,
                      const std::string& instanceShort);

    /** The number of track deletions this history has seen; the entries made after the last one carry it. */
    std::uint64_t barrierGeneration() const noexcept { return barrierGeneration_; }

    // ---- Contents that a track deletion removed from the project -------------------------------------

    /**
     * Contents that left the project when a track deletion removed the last clip that showed them. OpenTune leaks
     * such a content: StandaloneArrangement::removeTrackAndShift() drops the placements without retiring them or
     * their contents, so the content repository still lists the content as live. The control API does not show it
     * any more (controlLiveContents(), controlLiveSnapshot()), as a content that left the project. Forgotten when
     * the content epoch changes (a project was opened), and for a content that a clip shows again (the undo of an
     * earlier clip deletion brings such a clip back): see reviveShownContents(). Message thread only.
     */
    void markContentsRemoved(const std::vector<ContentKey>& contents, std::uint64_t epoch);
    bool hasRemovedContents() const { return !removedContents_.empty(); }
    /** Forgets the marks of the contents in \p shown: a clip of the arrangement shows them again. */
    void reviveShownContents(const std::vector<ContentKey>& shown);
    bool isContentRemoved(const ContentKey& content, std::uint64_t epoch) const;

private:
    struct UiEntry
    {
        const UndoAction* action = nullptr;
        int index = 0;
        std::type_index type = typeid(void);
        juce::String description;
        std::string undoId;
        /** A call with force applied the entry since the last track deletion: it is not possibly stale. */
        bool staleConfirmed = false;
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
    /** Positions below this one hold entries recorded before the last track deletion (see onTrackDeleted()). */
    int staleBelow_ = 0;
    std::uint64_t barrierGeneration_ = 0;
    std::optional<std::uint64_t> removedContentsEpoch_;
    std::vector<ContentKey> removedContents_;
    std::optional<std::uint64_t> epoch_;
    std::optional<Observation> lastObserved_;
    std::vector<UiEntry> uiEntries_;
    std::vector<ControlOriginAction*> actions_;
    ControlPitchShiftLedger pitchShiftLedger_;
};

/** True for the undo actions OpenTune cannot undo correctly: a clip split, merge or delete
    (SplitPlacementAction, MergePlacementAction, DeletePlacementAction; section 15.4 rule 1). */
bool controlIsUnsafeUndoAction(const UndoAction& action);

/** The OpenTune class name of an upstream undo action, for innerActions ("UndoAction" when unknown). */
std::string controlUndoActionName(const UndoAction& action);

} // namespace OpenTune
