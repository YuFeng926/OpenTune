// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Control/ControlUiBridge.h"
#include "Standalone/StandaloneMediaOperations.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace OpenTune {

class OpenTuneAudioProcessorEditor;
class ControlTrackColourWatch;

/**
 * The Standalone editor's side of ControlUiBridge: a thin adapter that reads the editor's own state and
 * calls the editor's own functions, and adds no behaviour of its own. It is a friend of
 * OpenTuneAudioProcessorEditor, so that the editor keeps its members private.
 *
 * Lifecycle: the editor creates it at the end of its constructor, which registers it with the processor's
 * ControlService, and destroys it first in its destructor, which unregisters it; the control API therefore
 * never reaches an editor that is being built or torn down. With the control API not enabled at run time
 * the service never starts and never calls the bridge.
 *
 * ControlEditorBridge.cpp covers the general part (project information, busy state, structure changes, the steps
 * after an undo or redo, the refresh after a time-grid edit, the view, the import focus, objects the editor owns, the
 * close of the application's window);
 * ControlEditorBridgeProject.cpp the project flows (the steps after a save or an open, the recent projects, the flag
 * of a running project operation); dialogs, tracks and preferences follow in files of their own
 * (ControlEditorBridge<Domain>.cpp).
 *
 * Threading: message thread, like the editor.
 */
class ControlEditorBridge final : public ControlUiBridge, private juce::MouseListener
{
public:
    explicit ControlEditorBridge(OpenTuneAudioProcessorEditor& editor);
    ~ControlEditorBridge() override;

    ControlEditorBridge(const ControlEditorBridge&) = delete;
    ControlEditorBridge& operator=(const ControlEditorBridge&) = delete;

    std::optional<ProjectInfo> projectInfo() const override;
    std::optional<BusyState> queryBusy() const override;
    void onStructureChanged() override;
    UndoAction* performUndoRedo(bool undo, const std::function<bool(UndoAction*)>& validateTop) override;
    void onTimeGridChanged() override;
    void onProjectReloaded() override;
    void onRecentProjectsChanged() override;
    void setProjectOperationRunning(bool running) override;
    std::optional<ProjectSession::SaveTask> prepareProjectSave(
        const juce::File& file, ProjectSession::SaveTask::Mode mode) override;
    std::optional<ProjectSession::OpenCommitPolicy> prepareProjectOpen(bool discardUnsaved) override;
    void commitProjectSave(const juce::File& file, ProjectSession::SaveTask::Mode mode,
                           uint64_t snapshotGeneration) override;
    bool focusPlacement(int trackId, std::uint64_t placementId) override;
    bool revealTracks(int count) override;
    bool currentNoteGenerationParams(NoteGeneratorParams& out) const override;
    StandaloneMediaOperations::ImportCommitResult commitImportedAudio(
        OpenTuneAudioProcessor::PreparedImport&& prepared,
        const StandaloneMediaOperations::ImportPlacementRequest& request,
        bool focus,
        bool generateNotes,
        std::function<void()> onNotesGenerated = {}) override;
    std::optional<ViewState> viewState() const override;
    std::optional<std::vector<std::uint64_t>> selectedPlacementIds() const override;
    std::optional<int> visibleTrackCount() const override;
    ProjectSession* projectSession() override;
    juce::ThreadPool* projectWorkerPool() override;
    juce::AudioDeviceManager* audioDevice() override;
    bool canQuit() const override;
    bool requestQuit(bool discardUnsaved) override;
    void busyStateChanged();

    // ---- Tracks and transport (ControlEditorBridgeTracks.cpp) ----
    ControlTrackCommandResult addTracks(int count) override;
    ControlTrackCommandResult duplicateTrack(int trackId, bool shareContent) override;
    ControlTrackCommandResult deleteTrack(int trackId) override;
    bool setTrackMix(int trackId, std::optional<bool> muted, std::optional<bool> solo,
                     std::optional<double> volumeLinear, std::optional<std::uint32_t> colourRgb) override;
    bool transportPlay() override;
    bool transportPause() override;
    bool transportStop() override;
    bool transportSeek(double positionSeconds) override;
    bool transportPlayFromLastStart() override;
    bool transportPauseAtLastStart() override;
    bool setLoopEnabled(bool enabled) override;
    bool setBpm(double bpm) override;
    bool setTimeSignature(int numerator, int denominator) override;
    // ---- Dialogs and onboarding (ControlEditorBridgeModals.cpp) ----
    std::optional<std::vector<DialogInfo>> openDialogs() const override;
    bool closeTopDialog() override;
    std::optional<bool> onboardingShowing() const override;
    bool dismissOnboarding() override;
    std::optional<UiScale> uiScale() const override;

    // ---- Selection (ControlEditorBridgeSelection.cpp) ----
    std::optional<SelectionPlan> planSelection(int trackId, std::uint64_t placementId,
                                               bool openEditor) const override;
    bool select(SelectionPlan plan) override;

    // ---- Workspace (ControlEditorBridgeWorkspace.cpp) ----
    bool setWorkspace(bool arrangement) override;

    // ---- Preferences and audio device (ControlEditorBridgePrefs.cpp) ----
    void onPreferencesChanged() override;
    std::optional<bool> editingSchemeApplied(bool notesPrimary) const override;
    void watchBusyState(std::function<void()> wake) override;
    std::optional<AsioRateRestoreFailure> prepareAudioDeviceSwitch(juce::AudioDeviceManager& manager,
                                                                   const std::string& reason) override;

private:
    void mouseUp(const juce::MouseEvent& event) override;

    /** The placement the arrangement selects on its active track, or 0. */
    std::uint64_t activeSelection() const;

    OpenTuneAudioProcessorEditor& editor_;

    /** Tells which track the colour picker that is open is for (ControlEditorBridgeColour.h). */
    std::unique_ptr<ControlTrackColourWatch> colourWatch_;
};

} // namespace OpenTune
