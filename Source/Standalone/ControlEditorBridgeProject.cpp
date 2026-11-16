// SPDX-License-Identifier: AGPL-3.0-or-later
// The project lifecycle entry points shared by the editor and Control API jobs.
#include "ControlEditorBridge.h"

#include "PluginEditor.h"

namespace OpenTune {

void ControlEditorBridge::onProjectReloaded()
{
    editor_.finishProjectOpen();
}

std::optional<ProjectSession::SaveTask> ControlEditorBridge::prepareProjectSave(
    const juce::File& file, ProjectSession::SaveTask::Mode mode)
{
    return editor_.prepareProjectSave(file, mode);
}

std::optional<ProjectSession::OpenCommitPolicy> ControlEditorBridge::prepareProjectOpen(bool discardUnsaved)
{
    return editor_.prepareProjectOpen(discardUnsaved);
}

void ControlEditorBridge::commitProjectSave(const juce::File& file, ProjectSession::SaveTask::Mode mode,
                                            uint64_t snapshotGeneration)
{
    editor_.commitProjectSave(file, mode, snapshotGeneration);
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
    if (!running) {
        editor_.maybeStartOnboarding();
        editor_.updateDemoTour();
        busyStateChanged();
    }
}

} // namespace OpenTune
