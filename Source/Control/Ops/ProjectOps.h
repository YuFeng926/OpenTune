// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers project.save, project.saveAs, project.open and project.clearRecent (capability project.io) and
 * project.saveCopy (capability project.copy; methods/project.*.schema.json).
 *
 * - project.save and project.saveAs hand the save to a job of kind projectSave, as the editor's Save and Save As do
 *   (OpenTuneAudioProcessorEditor::saveProject): the message thread takes the snapshot (ProjectSession::prepareSave),
 *   a worker of the editor's project pool writes the project and its media next to a temporary file in the target
 *   directory, and the job moves that file into place on the message thread with the final rename of PROTOCOL.md
 *   section 20.4, which replaces another project file only when the caller confirmed it. The job cannot be cancelled.
 * - project.open hands the opening to a job of kind projectOpen (OpenTuneAudioProcessorEditor::startOpenProject): a
 *   worker reads and prepares the file (ProjectSession::prepareOpen), the job commits it on the message thread
 *   (ProjectSession::commitPreparedOpen). It can be cancelled until the file is prepared, never saves the project
 *   that is open, and fails with UNSAVED_CHANGES unless the caller allowed discarding its changes.
 * - project.clearRecent clears the list of recent projects.
 * - project.saveCopy writes the project to a new file as a job of kind projectCopy, with the same snapshot, worker
 *   write, temporary file and final rename as a save, but never replaces a file (ALREADY_EXISTS, also for the
 *   project's own file) and changes nothing of the project: its file, its unsaved-changes flag, the recent projects,
 *   the undo history and the window title stay as they were. The job result names the file written, the project's
 *   file and the flag.
 *
 * The project session and its worker pool belong to the editor and are reached through ControlUiBridge; a job holds
 * the editor's project-operation flag from its start to its end, so that the editor's own file commands and every
 * other write answer BUSY(project_operation) meanwhile. Without an editor, the save and open methods answer
 * UNSUPPORTED(no_editor_window).
 */
void registerProjectOps(ControlMethodTable& table);

} // namespace OpenTune
