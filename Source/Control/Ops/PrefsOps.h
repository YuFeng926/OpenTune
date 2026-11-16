// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers prefs.set (capability prefs.write).
 *
 * prefs.set changes the processing preferences that OpenTune's preferences window changes: the frequency of A4
 * (tuningHz), the editing scheme, the hybrid (light pitch correction) switch, the experimental features, the vocoder
 * weight, the rendering priority and the pitch model. It also changes the user-global accessibilityModeEnabled UI
 * preference. Both entry points use applyProcessingPreferencePatch(), which owns change detection, runtime admission,
 * persistence, cache effects and the editor's onPreferencesChanged() synchronization.
 *
 * - A call that changes none of the editing scheme, the vocoder weight and the rendering priority applies at once and
 *   returns the write envelope. Changes are read from the persisted state after the shared command. The result carries
 *   GLOBAL_PREFERENCE, has no undo entry (reason ui_parity), and includes a revert recipe when the previous values fit
 *   the wire schema. A value equal to the current one is a no-op.
 * - The vocoder weight and the pitch model must be among processing.options of prefs.get (the files and models that the
 *   window lists); a value that is not gives INVALID_ARGUMENT(option_unavailable) and changes nothing. The pitch model
 *   is switched first: when the processor refuses, the call fails with MODEL_UNAVAILABLE(model_missing) and nothing
 *   changes.
 * - A call that changes the editing scheme, vocoder weight or rendering priority hands its work to a job of kind
 *   prefsApply. The job applies the shared command on the message thread and waits for runtime completion events when a
 *   runtime operation was queued. It reports the persisted state and completed backend effects. A runtime completion
 *   failure returns MODEL_UNAVAILABLE(vocoder_load_failed) with the committed envelope and executed true. Switching to
 *   openDyne can make the editor generate notes for the clip open in the piano roll; the job reports
 *   sideEffects.notesGenerated with SCHEME_GENERATED_NOTES and IRREVERSIBLE, and marks the project as changed. A
 *   completed backend change reports sideEffects.reRenderQueued for the contents it actually queued.
 *
 * accessibilityModeEnabled is the one UI preference exposed by prefs.set; other UI preferences remain outside the
 * closed params schema.
 *
 * Threading: parse on the connection thread; run, the job and everything it touches on the message thread.
 */
void registerPrefsOps(ControlMethodTable& table);

} // namespace OpenTune
