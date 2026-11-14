// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * The key and AUTO methods of capability edit.v1, on the message thread:
 * - content.setKey: sets the key of a content with
 *   ContentEditCommands::setDetectedKey(), as PluginEditor::scaleChanged() does for the selected
 *   clip. As in the UI there is no undo entry (reason ui_parity); the result gives content.setKey with the
 *   previous root and scale as revert recipe. An unchanged manual key is a no-op, as in the UI.
 * - content.autoTune, modes regenerate, snapNotes and notesOnly:
 *   regenerate uses the shared range planner and commits the composed result through the owner as one native
 *   piano-roll undo action. snapNotes uses ContentEditCommands::planAutoSnap(), the same planner as the AUTO button
 *   in the openDyne scheme (PianoRollComponent::applyAutoSnapToAllNotes()), then commits once through the owner.
 *   notesOnly runs ContentEditCommands::generateNotesOnly(), which replaces every note without an undo entry, as
 *   OpenTune does when it generates notes for a clip.
 *
 * Both check, in order: the handle, the busy state, the readiness gate, the expected
 * tokens. Changes without an undo entry (setKey, notesOnly) are never taken back by an undo step: entries
 * recorded through the API stop putting back the aspect they changed, and the redo history is cleared
 * (ControlHistory::onChangeWithoutEntry()). The shared content owner handles completed-edit notification.
 */
void registerContentTuneOps(ControlMethodTable& table);

} // namespace OpenTune
