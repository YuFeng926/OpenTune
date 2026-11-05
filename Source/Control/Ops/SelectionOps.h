// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers ui.setSelection (capability ui.bridge; methods/ui.setSelection.schema.json), the third method of ui.bridge
 * after ui.getState and ui.dismiss (UiOps.h).
 *
 * The call selects through the editor (ControlUiBridge::select()) as the arrangement view does when the user clicks: a
 * placementId makes that clip the only selected clip and its track the active track, as a click on it does; openEditor
 * also opens it in the piano roll, as a double click does; a trackId makes that track the active track with no clip
 * selected and the piano roll empty. Without an editor the call answers UNSUPPORTED(no_editor_window).
 *
 * Selecting can change more than the selection, as a click does, and the result reports what the call observed:
 * - notesGenerated (warnings SELECTION_GENERATED_NOTES and IRREVERSIBLE, the project marked as changed): in the
 * openDyne scheme the piano roll generates the notes of the clip it shows when they were never generated and its pitch
 *   analysis is ready (PianoRollComponent::ensureOpenDyneNotesIfNeeded, called by setEditedContentAndProjection when
 *   the piano roll gets another clip), with no undo entry;
 * - uiScaleChanged: the transport bar shows the clip's key (applyResolvedScaleForPlacementContent);
 * - workspaceSwitched: openEditor switched from the arrangement to the piano roll (placementDoubleClicked).
 * - tempoRounded (and the change of the singleton tempo, as transport.setTempo reports it): the switch moved the
 *   keyboard focus out of the transport bar's BPM field, which then committed the tempo it shows with one decimal, so a
 *   project tempo with more decimals (120.27) became 120.3 and the project was marked as changed. Upstream behaviour,
 *   reported as ui.setWorkspace reports it (see WorkspaceOps.h): the call compares the processor's tempo before and
 *   after. dryRun does not predict it, because it depends on where the keyboard focus is.
 *
 * The call predicts these effects from the same state first; dryRun returns the prediction and changes nothing, the
 * selection included. A committed call reports what happened; a difference from the prediction is logged. The
 * selection is view state: the undo reason is not_persisted_state, and the revert recipe selects the active track and
 * its selected clip from before the call with ui.setSelection (never taking back notes or switching the window back).
 * Selecting what is already selected, with no other effect, is a no-op. The busy checks of PROTOCOL.md section 13.2
 * apply, also to dryRun.
 */
void registerSelectionOps(ControlMethodTable& table);

} // namespace OpenTune
