// SPDX-License-Identifier: AGPL-3.0-or-later
// The project flows of the editor's side of ControlUiBridge: what the editor does around a project save or open that
// the control API runs through ProjectSession itself (Control/Ops/ProjectOps.cpp).
#include "ControlEditorBridge.h"

#include "PluginEditor.h"

namespace OpenTune {

void ControlEditorBridge::onProjectReloaded()
{
    // The steps at the end of OpenTuneAudioProcessorEditor::startOpenProject, after ProjectSession::commitPreparedOpen:
    // the recent-projects menu and the title follow the new project, and every part of the UI reads the restored
    // arrangement.
    editor_.syncRecentProjectsToMenu();
    editor_.updateTitleWithProjectPath();
    editor_.refreshAllUIFromProject();
}

void ControlEditorBridge::onProjectSaved()
{
    // The steps of OpenTuneAudioProcessorEditor::saveProject after a successful write: the recent-projects menu and
    // the title show the saved file.
    editor_.syncRecentProjectsToMenu();
    editor_.updateTitleWithProjectPath();
}

void ControlEditorBridge::onRecentProjectsChanged()
{
    // OpenTuneAudioProcessorEditor::syncRecentProjectsToMenu(), as the editor's clearRecentProjectsRequested() does
    // after it cleared the list.
    editor_.syncRecentProjectsToMenu();
}

void ControlEditorBridge::setProjectOperationRunning(bool running)
{
    // The flag that rejectProjectOperationIfBusy() checks, which saveProject, openProjectFile and the file commands set
    // for the duration of their own project operation.
    editor_.projectOperationBusy_ = running;
    // As at the ends of startOpenProject: a first-run overlay that the operation held back can start now.
    if (!running)
        editor_.maybeStartOnboarding();
}

} // namespace OpenTune
