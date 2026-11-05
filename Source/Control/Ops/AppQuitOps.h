// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers app.quit (capability app.quit; methods/app.quit.schema.json).
 *
 * The call ends OpenTune the way the window's close button does (the stock JUCE StandaloneFilterWindow: the plug-in
 * state is saved, then the application quits through JUCEApplicationBase::quit()), so the normal shutdown runs: the
 * control service stops, the listener closes and the discovery file is deleted. The call never saves the project,
 * never asks the user and never forces anything.
 *
 * It checks, in this order, and changes nothing when one check fails (executed false):
 * 1. SHUTTING_DOWN, answered by the dispatcher before the method starts;
 * 2. UNSUPPORTED(no_editor_window) without an editor and UNSUPPORTED(capability_missing) with a bridge that cannot
 *    close the window (ControlUiBridge::canQuit()): only the editor's window can end the Standalone application;
 * 3. BUSY, the five reasons of every write (controlCheckBusy()) and then any job that is not finished: its kind says
 *    the reason (import: import_in_progress, export: export_in_progress, projectOpen, projectSave and projectCopy:
 *    project_operation, any other kind: job_in_progress); with several jobs the reasons of section 13.2 come first, in
 *    the order of that section, and job_in_progress last. BUSY comes before UNSAVED_CHANGES: the user's open dialog
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
 * runs the step on the message thread only once the result is written to the connection (ControlDispatcher). The step
 * stops the control service (connections close, the discovery file goes) and then asks the bridge to close the window
 * (ControlUiBridge::requestQuit()). The call never stops the service itself while it runs on behalf of a request: the
 * network thread that waits for it holds no lease, and the service joins that very thread when it stops.
 *
 * Two cases leave the client with a result that the instance cannot honour, because the result is written before they
 * can be known: the editor is gone or its window does not close when the exit step runs, and the exit step cannot be
 * queued (ControlDispatcher::runAfterReply()). In the first the service is stopped all the same (before the window is
 * asked, never after) and OpenTune keeps running without its control API; the log says that the client was told
 * `quitting: true` and the window was not closed. In the second the dispatcher answers SHUTTING_DOWN, and the log says
 * which of two reasons it was (ControlMessageThread::isAlive()): the message queue refused the step while the
 * service was alive, so the listener and the discovery file stay until the application closes itself and its own
 * shutdown stops the service; or the service had been stopped already, so nothing is left to stop.
 */
void registerAppQuitOps(ControlMethodTable& table);

} // namespace OpenTune
