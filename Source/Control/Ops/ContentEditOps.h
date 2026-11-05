// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * The note edits and the pitch shift of capability edit.v1, on the message thread:
 * - content.editNotes (methods/content.editNotes.schema.json): ops setTargetPitch, shiftCents, snapToScale and
 *   setParams on notes selected by index (at expectedRevision), by a content-clock range or all of them. Each op
 *   that changes a note is committed as one gesture of the piano roll: the notes it changed, the correction
 *   segments rebuilt over their frame range (Edit/NoteCorrection.h), ContentEditCommands::commitNotesAndSegments()
 *   and a PianoRollEditAction with the UI's description (LOC_RAW keys "Move note", "Pitch snap", "Edit note",
 *   "Drift correction", in the language of the window). The actions of one call are one ControlOriginAction, so one
 * call is one undo step; the step's description is the first committed op's. dryRun predicts the same changes and side
 * effects and commits nothing.
 * - content.setPitchShift (methods/content.setPitchShift.schema.json): ContentEditCommands::commitPitchShiftEdit()
 *   and its PitchShiftEditAction in a ControlOriginAction, as the pitch-shift dialog
 *   (PluginEditor::pitchShiftRequested()) pushes it; the key is not transposed (warning KEY_NOT_TRANSPOSED). The
 *   revision tokens leave the step out (ControlPitchShiftLedger): `revision` stays, the pitchShift component of
 *   the revision vector changes. The current shift, or one OpenTune refuses as equal (the same total cents), is a
 *   no-op.
 *
 * Both check, in order: the handle, the busy state, the readiness gate (PROTOCOL.md section 11), the expected
 * tokens (editNotes), then their own validation. Both mark the project as changed, as the UI does after them.
 */
void registerContentEditOps(ControlMethodTable& table);

} // namespace OpenTune
