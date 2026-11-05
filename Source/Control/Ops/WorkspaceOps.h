// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers ui.setWorkspace (capability ui.workspace; methods/ui.setWorkspace.schema.json).
 *
 * The call switches the window between the arrangement view and the piano roll through the editor
 * (ControlUiBridge::setWorkspace()), as the two view buttons of the transport bar do. Only the view changes: no
 * content, key, note, clip or track is touched and nothing is generated or queued, in either editing scheme, so
 * revisionDiff is empty and dirty does not change (the workspace is not saved with the project). Switching to the
 * piano roll with no clip selected shows an empty piano roll, as the window does, and is no error.
 *
 * One exception, which is upstream behaviour that the call does not correct but reports as it happened: the switch
 * moves the keyboard focus, and when the focus is in the transport bar's BPM field the field loses it and commits what
 * it shows, the tempo with one decimal (BpmValueField::commit -> OpenTuneAudioProcessorEditor::bpmChanged). A project
 * tempo with more decimals (120.27) is then rewritten (120.3) and the project is marked as changed, so dirty.after is
 * true. The call compares the processor's tempo before and after the switch and, when it differs, lists the change of
 * the singleton tempo that transport.setTempo reports (entity tempo, member bpm {before, after, unit bpm}) and the side
 * effect tempoRounded {fromBpm, toBpm, revert} (revert: transport.setTempo back to fromBpm, or null where the instance
 * does not report transport.v1). The time signature is not touched: the field commits the tempo only.
 *
 * The result lists one change (entity ui, ref {singleton: workspace}, member workspace {before, after}) and the side
 * effect workspaceSwitched {from, to, revert: null}, the same one that ui.setSelection reports when it opens the
 * editor. The workspace is view state: the undo reason is not_persisted_state, and the revert recipe is
 * ui.setWorkspace to the workspace the window showed before. Asking for the workspace the window already shows is a
 * no-op (no change, no side effect, undo reason no_op). The busy checks of PROTOCOL.md section 13.2 apply. Without an
 * editor the call answers UNSUPPORTED(no_editor_window), and with a bridge that cannot report or switch the workspace
 * UNSUPPORTED(capability_missing).
 */
void registerWorkspaceOps(ControlMethodTable& table);

} // namespace OpenTune
