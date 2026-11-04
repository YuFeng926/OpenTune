// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsSupport.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenTune {

// ---- Checks before a write ------------------------------------------------------------------------

/** Which busy conditions a write checks: every condition, or every one but modal_open for the method
    whose purpose is closing dialogs (ui.dismiss). */
enum class ControlBusyScope {
    Write,
    DialogDismiss
};

/**
 * The busy checks of PROTOCOL.md section 13.2, made on the message thread immediately before a write
 * applies its change: BUSY with pointer_down, modal_open, import_in_progress, export_in_progress or
 * project_operation, in this order, as an error that changed nothing. export_in_progress also while the
 * processor's export lock is held (an export of the editor or of export.start), with or without an editor.
 * nullopt when the write may go on: without an editor nothing else is busy (no user is working in a
 * window), and the editor's conditions count only when the bridge can tell. Reads, session.* and job.*
 * never call it: they never return BUSY because of the UI (section 13.3).
 */
std::optional<Error> controlCheckBusy(ControlOpContext& context, ControlBusyScope scope = ControlBusyScope::Write);

// ---- Pieces of a write result ----------------------------------------------------------------------

/** The project's unsaved-changes flag as the editor shows it (ControlUiBridge::projectInfo()); false
    without an editor, which has no project session. */
bool controlProjectDirty(ControlOpContext& context);

/** Marks the project as changed through the editor (ControlUiBridge::markDirty(), the editor's
    ProjectSession::markDirty()), after a change that is saved with the project, also where the UI does
    not (section 15.5). Without an editor nothing. */
void controlMarkDirty(ControlOpContext& context);

/** True when the instance reports the capability: a revert recipe whose method needs a capability the
    instance does not report cannot restore anything (section 15.2 rule 2). */
bool controlReportsCapability(ControlOpContext& context, std::string_view capability);

/** A revert recipe (common.schema.json#/$defs/revert). params must not carry consent members
    (section 15.2 rule 4); requiresConsent says that the client adds them after the end user agreed. */
JsonValue controlRevertRecipe(std::string method, JsonValue params, bool requiresConsent = false);

/**
 * The generic UI snapshot (section 14.6): {workspace, pianoRollContentId, activeTrackId,
 * selectedPlacementIds, visibleTrackCount}. null with nullReason no_editor without an editor, or
 * unavailable when the bridge cannot report the view or the selection.
 */
JsonValue controlUiSnapshot(ControlOpContext& context, std::string& nullReason);

/**
 * revisionDiff (section 14.4) of two captures of the world revision vector: arrangement and trackState as
 * {before, after} or null, and one entry per content with each changed component (before null for a new
 * content, after null for a removed one). With onlyContents, other contents are left out.
 */
JsonValue controlRevisionDiff(const ControlWorldVersions& before, const ControlWorldVersions& after,
                              const std::vector<std::string>* onlyContents = nullptr);

// ---- The write envelope ----------------------------------------------------------------------------

/**
 * Builds the result of one write (section 14), whatever the method: the operation creates it on the
 * message thread immediately before its change, which captures the world revision vector and the dirty
 * flag; records what it changed, created and removed, the undo descriptor, side effects and warnings while
 * it works; and ends with exactly one of committed(), predicted() or handedToJob(), which capture the
 * vector again where the state requires it and return the envelope with every member (section 14.1) in one
 * of the four states of section 14.2.
 *
 * - committed(): applied. noOp when nothing was recorded (changes, created, removed, idMap or side effects)
 *   and revisionDiff is empty; the undo descriptor is then the one the method set, or {undoable: false,
 *   reason: no_op, revert: null} when it set none (section 14.2: a step of edit.undo that changes nothing
 *   keeps its own descriptor, section 15.3 rule 5). A change that is neither undoable nor revertible gets
 *   the warning IRREVERSIBLE (section 15.2 rule 2), unless its reason is not_persisted_state (view state,
 *   rule 6) or no_op (the method itself changed nothing).
 * - predicted(): dryRun; revisionDiff empty, tokens current, dirty unchanged, undo null.
 * - handedToJob(): the work was handed to a job; the job's result carries the envelope of what it commits.
 *   changes, created, removed, idMap and sideEffects are empty in this state (section 14.2 rule 3), so the
 *   method must not change any state before it hands its work over: nothing would explain a non-empty
 *   revisionDiff (section 14.4). Debug builds assert that it is empty.
 *
 * The result says what the server observed: tokens are read back after the commit (section 10 rule 5),
 * dirty after is the editor's flag after the call, ui is the snapshot after the call.
 *
 * Threading: message thread; the instance may outlive one request (a job keeps it across its steps), so
 * every call takes the context of the current step.
 */
class ControlWriteEnvelope
{
public:
    /** Captures the world revision vector and the dirty flag before the write. */
    explicit ControlWriteEnvelope(ControlOpContext& context);

    /** A changed entity (section 14.3): entity name, entityRef, label (string or null) and fields, a map of
        member name to {before, after, unit}. */
    void addChange(std::string entity, JsonValue ref, JsonValue label, JsonValue fields);
    /** An entity created or removed: {entity, ref, label}. */
    void addCreated(std::string entity, JsonValue ref, JsonValue label);
    void addRemoved(std::string entity, JsonValue ref, JsonValue label);
    /** An identity change {entity, relation, from[], to[]}. */
    void addIdMapEntry(JsonValue entry);
    /** A warning (section 18): a registered code, an entityRef or null, an object or null. */
    void addWarning(std::string code, JsonValue subject = JsonValue(), JsonValue data = JsonValue());
    /** A side effect that happened (section 14.6); effect is an object with at least revert. */
    void addSideEffect(std::string name, JsonValue effect);

    /** The write had an effect that none of the records above describes (a project save wrote a file): the result is
        no no-op even when nothing else was recorded and nothing changed in the world revision vector. */
    void markEffective() noexcept { effective_ = true; }

    /** A content or placement the write touched: its tokens are reported after the write. */
    void touchContent(const std::string& contentId);
    void touchPlacement(std::uint64_t placementId);
    /** Reports in revisionDiff only the contents the write touched, for writes that run as a job, while
        the user may change other contents. */
    void limitDiffToTouchedContents() noexcept { limitDiff_ = true; }

    /** The change is one entry of OpenTune's undo history (section 15.1). */
    void setUndoable(std::string undoId, std::string description, std::vector<std::string> innerActions);
    /** The change has no undo entry: a reason of the undo scope of registry/reasons.json and a revert
        recipe (controlRevertRecipe) or null when no request of this instance can restore the state. */
    void setNotUndoable(std::string reason, JsonValue revert = JsonValue());

    /** The envelope of a committed write. */
    JsonValue committed(ControlOpContext& context);
    /** The envelope of a prediction (dryRun). */
    JsonValue predicted(ControlOpContext& context);
    /** The envelope of a call that handed its work to the job jobRef ({jobId, kind, state}). */
    JsonValue handedToJob(ControlOpContext& context, JsonValue jobRef);

    bool dirtyBefore() const noexcept { return dirtyBefore_; }

private:
    enum class State {
        Committed,
        Predicted,
        HandedToJob
    };

    JsonValue build(ControlOpContext& context, State state, JsonValue jobRef);
    JsonValue tokensJson(ControlOpContext& context, const ControlWorldVersions& after) const;

    ControlWorldVersions before_;
    bool dirtyBefore_ = false;
    JsonValue changes_ = JsonValue::array();
    JsonValue created_ = JsonValue::array();
    JsonValue removed_ = JsonValue::array();
    JsonValue idMap_ = JsonValue::array();
    JsonValue warnings_ = JsonValue::array();
    JsonValue sideEffects_ = JsonValue::object();
    std::vector<std::string> touchedContents_;
    std::vector<std::uint64_t> touchedPlacements_;
    bool limitDiff_ = false;
    bool effective_ = false;
    JsonValue undo_;
    /** setUndoable() or setNotUndoable() was called. */
    bool undoSet_ = false;
    bool undoable_ = false;
    std::string undoReason_;
    bool revertNull_ = true;
};

} // namespace OpenTune
