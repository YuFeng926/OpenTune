// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlEditorBridge.h"

#include "PluginEditor.h"

#include "ControlEditorBridgeColour.h"
#include "Control/ControlService.h"
#include "Utils/NoteGeneratorTypes.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
// After the modules it uses, as in PluginEditor.cpp.
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

#include <algorithm>

namespace OpenTune {

ControlEditorBridge::ControlEditorBridge(OpenTuneAudioProcessorEditor& editor)
    : editor_(editor)
    , colourWatch_(std::make_unique<ControlTrackColourWatch>(editor.trackPanel_))
{
    // OpenTuneAudioProcessor::getControlService(): the processor's control service reaches the editor only
    // through the bridge it has registered.
    editor_.processorRef_.getControlService().setUiBridge(this);
    // OpenTuneAudioProcessorEditor::startDemoTourIfFirstRun(): the first-run demo project would replace the project the
    // client starts from, and it writes outside OPENTUNE_DATA_DIR. The flag stops it for this run only; the preference
    // stays as it is, so a later run without the API still shows the tour.
    if (editor_.processorRef_.getControlService().options().enabled)
        editor_.demoTourSettled_ = true;
}

ControlEditorBridge::~ControlEditorBridge()
{
    auto& service = editor_.processorRef_.getControlService();
    if (service.uiBridge() == this)
        service.setUiBridge(nullptr);
}

std::optional<ControlUiBridge::ProjectInfo> ControlEditorBridge::projectInfo() const
{
    // The editor's ProjectSession owns the project file and the unsaved-changes flag.
    const auto& session = editor_.projectSession_;
    ProjectInfo info;
    if (session.hasProjectPath())
        info.projectPath = session.getCurrentProjectFile().getFullPathName().toStdString();
    info.dirty = session.isDirty();
    return info;
}

void ControlEditorBridge::markDirty()
{
    // ProjectSession::markDirty(), as the editor's own edits do. The title is not refreshed here, as after
    // the editor's own edits.
    editor_.projectSession_.markDirty();
}

std::optional<ControlUiBridge::BusyState> ControlEditorBridge::queryBusy() const
{
    BusyState busy;
    // A mouse button or pen held down anywhere in the application: a gesture in progress.
    busy.pointerDown = juce::Desktop::getInstance().getNumDraggingMouseSources() > 0;
    // The editor's dialogs (DialogWindow, ConfirmDialogContent) enter the modal state.
    busy.modalOpen = juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0;
    // The editor's own flags of its project save/open, its import queue and its export worker.
    busy.operationRunning = editor_.projectOperationBusy_;
    busy.importRunning = editor_.isImportInProgress_ || !editor_.importQueue_.empty();
    busy.exportRunning = editor_.exportInProgress_.load();
    return busy;
}

void ControlEditorBridge::onStructureChanged()
{
    editor_.arrangementView_.requestContentRedraw();
}

void ControlEditorBridge::onUndoRedoApplied()
{
    // The editor's own steps after its undo and redo (performUndoRedoAction): mark the project as changed and
    // redraw the arrangement. The piano roll follows the new revisions on the editor's next heartbeat, as after
    // the UI's own undo.
    editor_.afterUndoRedo();
}

void ControlEditorBridge::onTimeGridChanged()
{
    // PianoRollComponent::onTimeGridRevisionChanged(): what the piano roll's own commit of a time-grid edit does
    // (its commitTimeGrid takes the new grid revision of the content it shows and redraws); it does nothing when
    // the piano roll shows another content. The arrangement draws clips through their time grids, so it is
    // redrawn as after an undo of a time-grid edit (afterUndoRedo).
    editor_.pianoRoll_.onTimeGridRevisionChanged();
    editor_.arrangementView_.requestContentRedraw();
}

bool ControlEditorBridge::focusPlacement(int trackId, std::uint64_t placementId)
{
    // The editor's steps after its own import commit (startPendingImport): redraw the arrangement and
    // make the clip the selected clip of the active track, which also shows it in the piano roll.
    editor_.arrangementView_.requestContentRedraw();
    editor_.applyPlacementSelectionContext(trackId, placementId);
    return true;
}

bool ControlEditorBridge::revealTracks(int count)
{
    // The editor's steps when an import into separate tracks needs more tracks than it shows
    // (importAudioRequested): the track panel and the arrangement show the same count. The panel's change also reaches
    // visibleTrackCountChanged(), which saves the count in the arrangement (project state) and marks the project.
    count = std::clamp(count, 1, OpenTuneAudioProcessor::MAX_TRACKS);
    if (editor_.trackPanel_.getVisibleTrackCount() >= count)
        return true;
    editor_.trackPanel_.setVisibleTrackCount(count);
    editor_.arrangementView_.setVisibleTrackCount(count);
    return true;
}

bool ControlEditorBridge::currentNoteGenerationParams(NoteGeneratorParams& out) const
{
    // PianoRollComponent::getCurrentAutoTuneParams(): what the editor passes to the note generation of
    // its own import in the openDyne scheme.
    out = editor_.pianoRoll_.getCurrentAutoTuneParams();
    return true;
}

std::uint64_t ControlEditorBridge::activeSelection() const
{
    const auto* arrangement = editor_.processorRef_.getStandaloneArrangement();
    if (arrangement == nullptr)
        return 0;
    const int activeTrack = arrangement->getActiveTrackId();
    return activeTrack >= 0 ? arrangement->getSelectedPlacementId(activeTrack) : 0;
}

std::optional<ControlUiBridge::ViewState> ControlEditorBridge::viewState() const
{
    ViewState view;
    view.workspace = editor_.isWorkspaceView_ ? "arrangement" : "pianoRoll";
    // The piano roll shows the selected clip of the active track once the editor synced it.
    view.pianoRollPlacementId = editor_.lastPianoRollContentKey_.isValid() ? activeSelection() : 0;
    return view;
}

std::optional<std::vector<std::uint64_t>> ControlEditorBridge::selectedPlacementIds() const
{
    // The selection the processor's arrangement holds: the selected clip of the active track. The extra
    // clips of a multi-selection live only inside the arrangement view and are not reported.
    std::vector<std::uint64_t> ids;
    if (const auto selected = activeSelection(); selected != 0)
        ids.push_back(selected);
    return ids;
}

std::optional<int> ControlEditorBridge::visibleTrackCount() const
{
    return editor_.trackPanel_.getVisibleTrackCount();
}

ProjectSession* ControlEditorBridge::projectSession()
{
    return &editor_.projectSession_;
}

juce::ThreadPool* ControlEditorBridge::projectWorkerPool()
{
    return &editor_.projectWorkerPool_;
}

juce::AudioDeviceManager* ControlEditorBridge::audioDevice()
{
    // The Standalone application's device manager, as the editor reaches it.
    auto* holder = juce::StandalonePluginHolder::getInstance();
    return holder != nullptr ? &holder->deviceManager : nullptr;
}

namespace {

/** The Standalone window that hosts the editor and its plug-in holder, or nullptr when the editor is not (yet, or any
    more) in one: the only place from which the application can be ended as the close button does. */
juce::StandaloneFilterWindow* standaloneWindowOf(juce::Component& editor)
{
    if (juce::StandalonePluginHolder::getInstance() == nullptr)
        return nullptr;
    return dynamic_cast<juce::StandaloneFilterWindow*>(editor.getTopLevelComponent());
}

} // namespace

bool ControlEditorBridge::canQuit() const
{
    return standaloneWindowOf(editor_) != nullptr;
}

bool ControlEditorBridge::requestQuit()
{
    // DocumentWindow::closeButtonPressed(), virtual: the window's own close button handler. The stock
    // StandaloneFilterWindow saves the plug-in state (StandalonePluginHolder::savePluginState()) and ends the
    // application (JUCEApplicationBase::quit()), which runs the normal shutdown. Calling the window's handler keeps
    // the behaviour of whatever the window does when its close button is pressed, without a copy of it here.
    auto* window = standaloneWindowOf(editor_);
    if (window == nullptr)
        return false;
    static_cast<juce::DocumentWindow*>(window)->closeButtonPressed();
    return true;
}

} // namespace OpenTune
