// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * The key and AUTO methods of capability edit.v1, on the message thread:
 * - content.setKey (methods/content.setKey.schema.json): sets the key of a content with
 *   OpenTuneAudioProcessor::setContentDetectedKey(), as PluginEditor::scaleChanged() does for the selected
 *   clip. As in the UI there is no undo entry (reason ui_parity); the result gives content.setKey with the
 *   previous root and scale as revert recipe. An unchanged manual key is a no-op, as in the UI.
 * - content.autoTune (methods/content.autoTune.schema.json), modes regenerate, snapNotes and notesOnly:
 *   regenerate runs OpenTuneAudioProcessor::autoTuneContentRangeByContentKey() (what
 *   ContentEditCommands::autoTuneContentRange() calls for the AUTO button in the openTune scheme) once per
 *   range, with the explicit key and NoteGeneratorParams{} plus the given params (never the UI's knobs), and
 *   records one undo entry for the call: the PianoRollEditActions that ContentEditTransaction builds as the
 *   piano roll does, in one ControlOriginAction. snapNotes is SNAP, the AUTO button in the openDyne scheme
 *   (PianoRollComponent::applyAutoSnapToAllNotes(), ported in Edit/NoteCorrection.h) with the explicit key:
 *   every note without ranges, else per range the notes that overlap it; one undo entry for the call, built as
 *   for regenerate. notesOnly runs OpenTuneAudioProcessor::generateNotesOnlyByContentKey()
 *   (ContentEditCommands::generateNotesOnly()), which replaces every note without an undo entry, as OpenTune
 *   does when it generates notes for a clip.
 *
 * Both check, in order: the handle, the busy state, the readiness gate (PROTOCOL.md section 11), the expected
 * tokens. Changes without an undo entry (setKey, notesOnly) are never taken back by an undo step: entries
 * recorded through the API stop putting back the aspect they changed, and the redo history is cleared
 * (section 15.3 rule 9, ControlHistory::onChangeWithoutEntry()).
 */
void registerContentTuneOps(ControlMethodTable& table);

} // namespace OpenTune
