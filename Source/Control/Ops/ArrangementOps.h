// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * The clip methods of capability arrangement.v1, on the message thread, through OpenTune's arrangement, its
 * processor functions for clips and the undo actions of its arrangement view (Utils/PlacementActions.h).
 *
 * - arrangement.edit (methods/arrangement.edit.schema.json) applies a list of ops to clips that exist when the call
 *   starts. The in-place ops follow the arrangement view: move (a drag, MultiMovePlacementAction), trim (an edge drag,
 *   TrimPlacementAction; the start edge computes the new clipIn through the content's time grid as the drag does),
 *   setGain (GainChangeAction) and setFade (FadeChangeAction). Every op is checked against a model of the arrangement
 *   first, so that a call that fails changes nothing; the ops then run in order and form one undo entry (a
 *   ControlOriginAction around OpenTune's actions). A trim records the clip's fades in a FadeChangeAction before the
 * trim when it shortens them, so that undoing it restores them (the UI's trim undo does not). split, merge and delete
 * call the processor functions of the arrangement view's keys (splitPlacementAtSeconds, mergePlacements,
 * deletePlacement) and push the same undo actions as the UI; OpenTune's reclaim sweep discards the retired clips on the
 * next message turn, so undoing them does not restore the clips: the result reports upstream_undo_defect, and edit.undo
 * refuses such an entry. A split's revert recipe merges the two clips again, a merge's splits the clip at the former
 * boundary; a deletion has none (IRREVERSIBLE) and needs confirm. With dryRun nothing changes and the result predicts
 * the in-place changes; for split and merge it names the clips that would be replaced, not the new ones.
 * - arrangement.copy (methods/arrangement.copy.schema.json) copies clips as pasting (mode window,
 *   OpenTuneAudioProcessor::copyContentRange()) and duplicating (mode full, cloneContent()) do: every copy gets a new
 *   content and keeps the gain and fades of its source. No undo entry, as in the UI (ui_parity); the revert recipe
 *   deletes the copies with arrangement.edit and needs the end user's consent.
 *
 * Both methods check, in the order of PROTOCOL.md section 13.1: the handles (NOT_FOUND), the busy state, the expected
 * arrangement revision (REVISION_CONFLICT(arrangement_changed)), then their own validation. A clip moved or copied to
 * a hidden track shows the track (ControlUiBridge::revealTracks(), TRACK_AUTO_REVEALED). After a change the project is
 * marked as changed and the editor redraws the arrangement (ControlUiBridge::onStructureChanged()).
 */
void registerArrangementOps(ControlMethodTable& table);

} // namespace OpenTune
