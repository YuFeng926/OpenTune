// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Content/ContentEditCommands.h"
#include "Utils/ProjectSession.h"

#if JucePlugin_Build_Standalone
#include "Standalone/StandaloneMediaOperations.h"
#endif

namespace juce {
class AudioDeviceManager;
class ThreadPool;
} // namespace juce

namespace OpenTune {

class ProjectSession;
class UndoAction;
struct NoteGeneratorParams;

enum class ControlTrackCommandResult
{
    Applied,
    InvalidTrack,
    HiddenTrack,
    TrackLimit,
    TargetOccupied,
    LastTrack,
    Failed
};

/**
 * Narrow interface through which the control API reaches what only the Standalone editor owns: the
 * project session and its worker pool, the view, the busy state of the UI, the audio device manager.
 * The processor does not take over any of this (docs/code-architecture-review.md: no new data model in
 * PluginProcessor); the editor implements the interface in a thin adapter and registers it with
 * ControlService::setUiBridge(). Without an editor the pointer is null.
 *
 * Every function has a default that reports "not supported" (an empty optional or nullptr) or does
 * nothing, so that each group of methods overrides only what it needs, and a missing editor
 * reduces results instead of breaking them. The sections follow the domains of the API.
 *
 * Threading: message thread only, like the editor itself. The owner unregisters the bridge
 * (ControlService::setUiBridge(nullptr)) before destroying it.
 */
class ControlUiBridge
{
public:
    virtual ~ControlUiBridge() = default;

    // ---- Project information ------------------------------------------------------------------

    /** What the editor knows about the open project. */
    struct ProjectInfo
    {
        /** Absolute path of the project file (UTF-8); empty for a project that was never saved. */
        std::string projectPath;
        /** True when the editor shows unsaved changes. */
        bool dirty = false;
    };

    /** The open project as the editor sees it; nullopt when not supported. */
    virtual std::optional<ProjectInfo> projectInfo() const { return std::nullopt; }

    // ---- Busy state ---------------------------------------------------------------------------

    /** Why the UI must not be changed under the user's hands right now: each member is one reason of BUSY. */
    struct BusyState
    {
        /** A mouse or pen gesture is in progress in the editor (pointer_down). */
        bool pointerDown = false;
        /** A modal dialog is open (modal_open). */
        bool modalOpen = false;
        /** The editor runs a project save or open of its own (project_operation). */
        bool operationRunning = false;
        /** An import started in the editor is running or queued (import_in_progress). */
        bool importRunning = false;
        /** An export started in the editor is running (export_in_progress). */
        bool exportRunning = false;
    };

    /** The busy state; nullopt when not supported (the control API then cannot tell). */
    virtual std::optional<BusyState> queryBusy() const { return std::nullopt; }

    // ---- Notifications after changes made through the control API -----------------------------

    /** Tracks or placements were added, removed or moved. Default: nothing. */
    virtual void onStructureChanged() {}

    /** Settles pending editor edits, validates the resulting history top, then runs the editor's complete command. */
    virtual UndoAction* performUndoRedo(bool /*undo*/, const std::function<bool(UndoAction*)>& /*validateTop*/)
    {
        return nullptr;
    }

    /** A project was opened or the project was reloaded: the editor does what it does after its own open (updates the
        recent-projects menu and the title, refreshes the track panel, the arrangement and the piano roll from the
        new project). Default: nothing. */
    virtual void onProjectReloaded() {}

    /** The list of recent projects changed through the control API (project.clearRecent): the editor refreshes its
        recent-projects menu. Default: nothing. */
    virtual void onRecentProjectsChanged() {}

    // ---- Project operations ---------------------------------------------------------------------

    /** Sets or clears the editor's own flag of a running project save or open, which the editor's file commands check
        before they start (and answer with their "another project operation is running" message) and which
        queryBusy() reports as BusyState::operationRunning. A project job of the control API sets it for its
        whole run. Default: nothing. */
    virtual void setProjectOperationRunning(bool /*running*/) {}

    /** The time grid of a content was changed through the control API (content.shiftTiming,
        content.editTimeGrid): the editor does what it does after its own time-grid edit (the piano roll takes
        the new grid of the content it shows and redraws) and redraws the arrangement, which draws clips through
        their grids. Default: nothing. */
    virtual void onTimeGridChanged() {}

    // ---- View changes that a write makes as the UI does ----------------------------------------

    /** Focuses a placement the way the editor does after its own import: it becomes the active track's
        selected placement and the piano roll loads its content. The view is not switched: the workspace that the
        window shows stays as it was, so no switch and no tempo rounding follow.
        Returns false when not supported. */
    virtual bool focusPlacement(int /*trackId*/, std::uint64_t /*placementId*/) { return false; }

    /** Makes the arrangement show at least count track slots, as the editor does when an import fills
        tracks beyond the visible ones. Returns false when not supported. */
    virtual bool revealTracks(int /*count*/) { return false; }

    /** The note generation parameters the editor would use for an AUTO or for the notes of an import
        right now. Returns false, leaving out unchanged, when not supported. */
    virtual bool currentNoteGenerationParams(NoteGeneratorParams& /*out*/) const { return false; }

#if JucePlugin_Build_Standalone
    /** Commits imported media through the Standalone editor's complete product operation. */
    virtual StandaloneMediaOperations::ImportCommitResult commitImportedAudio(
        OpenTuneAudioProcessor::PreparedImport&&,
        const StandaloneMediaOperations::ImportPlacementRequest&,
        bool /*focus*/,
        bool /*generateNotes*/,
        std::function<void()> /*onNotesGenerated*/ = {})
    {
        return {};
    }
#endif

    // ---- View ---------------------------------------------------------------------------------

    /** The parts of the view that results report. */
    struct ViewState
    {
        /** Name of the visible workspace, for example "arrangement" or "pianoRoll". */
        std::string workspace;
        /** Placement shown in the piano roll; 0 when none. */
        std::uint64_t pianoRollPlacementId = 0;
    };

    /** The view; nullopt when not supported. */
    virtual std::optional<ViewState> viewState() const { return std::nullopt; }

    /** The placements selected in the arrangement, in the editor's order; nullopt when not supported. */
    virtual std::optional<std::vector<std::uint64_t>> selectedPlacementIds() const { return std::nullopt; }

    /** Number of tracks the arrangement shows; nullopt when not supported. */
    virtual std::optional<int> visibleTrackCount() const { return std::nullopt; }

    // ---- Objects owned by the editor ----------------------------------------------------------

    /** The editor's project session; nullptr when not supported. */
    virtual ProjectSession* projectSession() { return nullptr; }

    /** The worker pool the editor uses for project operations; nullptr when not supported. */
    virtual juce::ThreadPool* projectWorkerPool() { return nullptr; }

    /** Captures a save snapshot after the editor settles its pending project edits. */
    virtual std::optional<ProjectSession::SaveTask> prepareProjectSave(
        const juce::File& /*file*/, ProjectSession::SaveTask::Mode /*mode*/)
    {
        return std::nullopt;
    }

    /** Captures the open conflict policy after the editor settles its pending project edits. */
    virtual std::optional<ProjectSession::OpenCommitPolicy> prepareProjectOpen(bool /*discardUnsaved*/)
    {
        return std::nullopt;
    }

    /** Commits the saved file and refreshes the editor's recent-project menu and title. */
    virtual void commitProjectSave(const juce::File& /*file*/, ProjectSession::SaveTask::Mode /*mode*/,
                                   std::uint64_t /*snapshotGeneration*/) {}

    // ---- Audio device -------------------------------------------------------------------------

    /** The Standalone application's audio device manager; nullptr when not supported. */
    virtual juce::AudioDeviceManager* audioDevice() { return nullptr; }

    // ---- Tracks and transport -----------------------------------------------------------------

    /** Runs the track panel's product commands, including project state and UI synchronization. */
    virtual ControlTrackCommandResult addTracks(int /*count*/) { return ControlTrackCommandResult::Failed; }
    virtual ControlTrackCommandResult duplicateTrack(int /*trackId*/, bool /*shareContent*/)
    {
        return ControlTrackCommandResult::Failed;
    }
    virtual ControlTrackCommandResult deleteTrack(int /*trackId*/) { return ControlTrackCommandResult::Failed; }
    virtual bool setTrackMix(int /*trackId*/, std::optional<bool> /*muted*/, std::optional<bool> /*solo*/,
                             std::optional<double> /*volumeLinear*/, std::optional<std::uint32_t> /*colourRgb*/)
    {
        return false;
    }

    virtual bool transportPlay() { return false; }
    virtual bool transportPause() { return false; }
    virtual bool transportStop() { return false; }
    virtual bool transportSeek(double /*positionSeconds*/) { return false; }
    virtual bool transportPlayFromLastStart() { return false; }
    virtual bool transportPauseAtLastStart() { return false; }
    virtual bool setLoopEnabled(bool /*enabled*/) { return false; }
    virtual bool setBpm(double /*bpm*/) { return false; }
    virtual bool setTimeSignature(int /*numerator*/, int /*denominator*/) { return false; }

    // ---- Dialogs and onboarding ---------------------------------------------------------------

    /** One modal window of the editor. */
    struct DialogInfo
    {
        /** trackColour, confirm, pitchShift, preferences, message or unknown. */
        std::string kind = "unknown";
        /** The title the window shows (UTF-8); empty for a dialog that draws its title itself. */
        std::string title;
        /** Closing the dialog, also with Escape, commits the value it shows (trackColour; unknown, whose behaviour
            the bridge cannot tell). */
        bool closeCommits = true;
        /** The track of a track-colour dialog; -1 for the other kinds. */
        int trackId = -1;
        /** A file chooser of the operating system: modal, but neither listed by ui.getState nor closed by
            ui.dismiss. */
        bool systemFileChooser = false;
    };

    /** The modal windows that are open, from the bottom of the stack to the top; nullopt when not supported. */
    virtual std::optional<std::vector<DialogInfo>> openDialogs() const { return std::nullopt; }

    /** Closes the topmost modal window as Escape does, when it is a dialog and not a file chooser of the operating
        system. What the dialog does when it closes has happened when the call returns: a track-colour dialog has
        committed its colour, a confirmation dialog has cancelled its action. Returns false, closing nothing, when
        no dialog is on top or when not supported. */
    virtual bool closeTopDialog() { return false; }

    /** True while the first-run onboarding overlay is on screen; nullopt when not supported. */
    virtual std::optional<bool> onboardingShowing() const { return std::nullopt; }

    /** Dismisses onboarding and records its preference through the editor's product entry. */
    virtual bool dismissOnboarding() { return false; }

    /** The scale that the AUTO button, SNAP and the pitch tool of the UI use: the scale the transport bar shows. */
    struct UiScale
    {
        /** 0 (C) to 11 (B). */
        int rootNote = 0;
        /** The UI's scale type of Utils/ScaleUiMapping.h: 1 major to 8 minor pentatonic. */
        int scaleType = 1;
    };

    /** The scale of the UI; nullopt when not supported. */
    virtual std::optional<UiScale> uiScale() const { return std::nullopt; }

    // ---- Selection ----------------------------------------------------------------------------

    /** A selection owner plan shared by dry-run and execution. */
    struct SelectionPlan
    {
        int trackId = -1;
        std::uint64_t placementId = 0;
        bool openEditor = false;
        int activeTrackAfter = -1;
        std::vector<std::uint64_t> selectedPlacementIdsAfter;
        std::optional<UiScale> scaleAfter;
        std::string workspaceAfter;
        std::optional<ContentKey> generatedNotesKey;
        std::optional<ContentNoteRangePatch> generatedNotes;
        bool commitPendingBpm = false;
        double bpmAfter = 0.0;
    };

    /** Plans the editor's complete selection operation. */
    virtual std::optional<SelectionPlan> planSelection(int /*trackId*/, std::uint64_t /*placementId*/,
                                                        bool /*openEditor*/) const { return std::nullopt; }

    /** Executes the supplied editor plan, including its view and content side effects. */
    virtual bool select(SelectionPlan /*plan*/) { return false; }

    // ---- Workspace ----------------------------------------------------------------------------

    /** Switches the window to the arrangement view (arrangement true) or to the piano roll (false), as the two view
        buttons of the transport bar do. Only the view changes: no content, key, note, clip or track is touched and
        nothing is generated or queued. One exception, which the caller reports as it happened (the processor's tempo
        before and after the call): the switch moves the keyboard focus, and the focus that the transport bar's BPM
        field loses commits the field, which shows the tempo with one decimal, so a tempo with more decimals is
        rewritten and the project is marked as changed (upstream behaviour, not corrected). Does nothing when the
        window already shows that view. The workspace that results report is viewState(). Returns false, changing
        nothing, when not supported. Message thread. */
    virtual bool setWorkspace(bool /*arrangement*/) { return false; }

    // ---- Quit ---------------------------------------------------------------------------------

    /** True when requestQuit() ends the application as the close button of the editor's window does. Default:
        false (a bridge that cannot quit). Message thread. */
    virtual bool canQuit() const { return false; }

    /** Ends the application through its normal shutdown. discardUnsaved carries the caller's explicit authorization
        to discard a dirty project to the final close path, without another prompt. With false, the editor requires a
        clean project and uses its normal unsaved-changes flow if that state changed after the caller checked it.
        Returns true when the quit is under way, false when it was not started. Message thread. */
    virtual bool requestQuit(bool discardUnsaved) { return false; }

    // ---- Preferences and audio device ---------------------------------------------------------

    /** The shared preferences were changed through the control API (prefs.set): the editor applies them the way it
        does after its own preferences window changed one (OpenTuneAudioProcessorEditor::syncSharedAppPreferences),
        which it also does on its heartbeat. Default: nothing. */
    virtual void onPreferencesChanged() {}

    /** Whether the editor has applied the editing scheme to its views (the piano roll, the parameter panel, the
        layout): the notes-primary scheme (openDyne) when notesPrimary is true. nullopt when not supported, in
        which case the control API takes the scheme as applied once the preference is set. */
    virtual std::optional<bool> editingSchemeApplied(bool /*notesPrimary*/) const { return std::nullopt; }

    /** The sample rate restore of an ASIO device that failed (prepareAudioDeviceSwitch()): the data of the warning
        ASIO_RATE_NOT_RESTORED. */
    struct AsioRateRestoreFailure
    {
        /** The ASIO device whose sample rate was to be restored (the name its driver reports). */
        std::string deviceName;
        /** The rate in Hz that the restore tried to set: the sample rate of the Windows shared mode, which is what the
            audio settings page moves an ASIO device to. When that rate could not be read, no rate was tried, and this
            is the rate the ASIO device was running at. */
        double sampleRateHz = 0.0;
    };

    /** What the audio settings page does before it closes an ASIO device for another device type or output
        device (restoreAsioSampleRateToSystem), as best effort: returns nullopt when the restore succeeded or there was
        nothing to restore, and the device and the rate when it failed or could not be verified. The change goes on all
        the same, as on the page, which shows kAudioDeviceRestoreWarning and continues; the control API reports the
        warning ASIO_RATE_NOT_RESTORED with the returned values. manager is the one the call works on (audioDevice()),
        so that the restore acts on the device the call is about to close. Default: nullopt (nothing to restore). */
    virtual std::optional<AsioRateRestoreFailure> prepareAudioDeviceSwitch(juce::AudioDeviceManager& /*manager*/,
                                                                           const std::string& /*reason*/)
    {
        return std::nullopt;
    }

    /** Registers a one-shot wake for current busy UI state to end. */
    virtual void watchBusyState(std::function<void()> wake) { (void) wake; }
};

} // namespace OpenTune
