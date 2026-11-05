// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers ui.getState and ui.dismiss (capability ui.bridge; methods/ui.getState.schema.json and
 * methods/ui.dismiss.schema.json). ui.setSelection, the third method of ui.bridge, is in SelectionOps.h.
 *
 * - ui.getState reports the busy conditions of PROTOCOL.md section 13.2, the editor's modal dialogs from the bottom of
 *   the stack to the top (without the file choosers of the operating system), the first-run onboarding, the generic UI
 *   snapshot and the scale of the UI. It is a read: it never answers BUSY.
 * - ui.dismiss with target dialogs closes every dialog from the top down as Escape does (ControlUiBridge::
 *   closeTopDialog()). A dialog whose closeCommits is true (the track-colour dialog, a dialog the bridge cannot
 *   classify) is closed only with confirm; otherwise the call fails with INVALID_ARGUMENT(dialog_commits_on_close) and
 *   closes nothing. A committed track colour is reported as a change of the track, with the warning
 *   DIALOG_CLOSE_COMMITTED; as in OpenTune it has no undo entry (ui_parity), and its revert recipe is track.set where
 *   the instance reports tracks.v1. It stops below a file chooser of the operating system, which it cannot close.
 * - ui.dismiss with target onboarding prevents the overlay while it is not showing (the preference onboardingShown,
 *   which the editor checks before it starts the overlay), and closes it while it shows, as Escape does: the dialogs
 *   above it first (the overlay's own choice dialog finishes the overlay when it closes), then the overlay. Closing
 *   needs the capability ui.onboardingClose, which the instance reports together with ui.bridge.
 *
 * ui.dismiss is the only write that a modal dialog does not refuse (ControlBusyScope::DialogDismiss). Both methods
 * describe the editor window: without an editor they answer UNSUPPORTED(no_editor_window), and with a bridge that
 * does not report its dialogs UNSUPPORTED(capability_missing).
 */
void registerUiOps(ControlMethodTable& table);

} // namespace OpenTune
