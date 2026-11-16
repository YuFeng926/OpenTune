// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers app.quit (capability app.quit).
 *
 * The call ends OpenTune the way the window's close button does (the JUCE Standalone window: the plug-in state is
 * saved, then the application quits through JUCEApplicationBase::quit()), so the normal shutdown runs: the control
 * service stops, the listener closes and the discovery file is deleted. The call never saves the project or asks the
 * user; a dirty project requires explicit discardUnsaved authorization, which reaches the final close path.
 *
 * It checks, in this order, and changes nothing when one check fails (executed false):
 * 1. SHUTTING_DOWN, answered by the dispatcher before the method starts;
 * 2. UNSUPPORTED(no_editor_window) without an editor and UNSUPPORTED(capability_missing) with a bridge that cannot
 *    close the window (ControlUiBridge::canQuit()): only the editor's window can end the Standalone application;
 * 3. BUSY, the five reasons of every write (controlCheckBusy()) and then any job that is not finished: its kind says
 *    the reason (import: import_in_progress, export: export_in_progress, projectOpen, projectSave and projectCopy:
 *    project_operation, any other kind: job_in_progress); with several jobs the specific reasons come first in the
 *    order import, export, project operation, and job_in_progress last. BUSY comes before UNSAVED_CHANGES: the user's open dialog
 *    or running operation must not be answered with a question about what to discard;
 * 4. UNSAVED_CHANGES(project_dirty) when the project has unsaved changes and discardUnsaved is not true. The flag is
 *    the one that project.open checks (ProjectSession::isDirty(), read through ControlUiBridge::projectInfo()).
 *
 * The result is {quitting, discardedUnsavedChanges, projectPath, exitDeadlineMs, warnings, nullReasons?}, not a write
 * envelope: the instance is going away, so there is no revision vector, undo or revert to report. Discarding changes
 * adds the warning IRREVERSIBLE with the project as its subject; a project that was never saved has projectPath null
 * with the reason no_project_path.
 *
 * The method returns the result first and quits afterwards. It hands the exit step to the dispatcher
 * (ControlOpContext::endInstanceAfterReply()), which answers every other request with SHUTTING_DOWN from then on and
 * runs the step on the message thread after the complete reply including LF is written. Send failure and connection
 * closure also finish the accepted quit request; slow readers use the connection's bounded final flush. The step
 * stops the control service (connections close, the discovery file goes) and then asks the bridge to close the window
 * (ControlUiBridge::requestQuit()), with the discard authorization already checked above. The call never stops the
 * service itself while it runs on behalf of a request.
 *
 * If the editor has gone or its window declines the close, the service has already stopped and the log records the
 * window's outcome. An exit post rejected during application teardown is covered by the application's own shutdown.
 */
void registerAppQuitOps(ControlMethodTable& table);

} // namespace OpenTune
