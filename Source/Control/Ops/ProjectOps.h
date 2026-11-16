// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers project.save, project.saveAs, project.open and project.clearRecent (capability project.io) and
 * project.saveCopy (capability project.copy).
 *
 * - project.save, project.saveAs and project.saveCopy use the editor owner's save preparation (gesture settlement and
 *   ProjectSession snapshot), then a worker writes the project and media. The save job commits the current file, dirty
 *   generation and recent list through the same owner as the GUI; Copy mode writes the new file without changing
 *   project state. API path identity, confirmation, temporary-file rename and job results stay at this boundary.
 *   Save jobs cannot be cancelled.
 * - project.open hands the opening to a job of kind projectOpen (OpenTuneAudioProcessorEditor::startOpenProject): a
 *   worker reads the project and media files (ProjectSession::prepareOpen), the message thread prepares imports and
 *   commits it (ProjectSession::commitPreparedOpen), then refreshes the editor through its owner. It can be cancelled
 *   until the file is prepared, never saves the project
 *   that is open, and fails with UNSAVED_CHANGES unless the caller allowed discarding its changes.
 * - project.clearRecent clears the list of recent projects.
 * - project.saveCopy writes a Copy-mode snapshot to a new file as a job of kind projectCopy, with the same worker
 *   write, temporary file and final rename as a save. It never replaces a file (ALREADY_EXISTS, also for the
 *   project's own file); ProjectSession keeps the project's file, dirty flag, recent projects, undo history and title.
 *   The job result names the file written, the project's file and the flag.
 *
 * The project session and its worker pool belong to the editor and are reached through ControlUiBridge; a job holds
 * the editor's project-operation flag from its start to its end, so that the editor's own file commands and every
 * other write answer BUSY(project_operation) meanwhile. Without an editor, the save and open methods answer
 * UNSUPPORTED(no_editor_window).
 */
void registerProjectOps(ControlMethodTable& table);

} // namespace OpenTune
