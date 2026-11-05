// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// A ControlUiBridge for the headless processor tests: reports what a test injects and counts the
// notifications it receives. The tests of each group of methods extend it with what they need.

#include "Control/ControlUiBridge.h"
#include "PluginProcessor.h"
#include "Utils/ProjectSession.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace OpenTune {
namespace ControlTest {

/** Message thread only, like a real bridge. */
class TestUiBridge : public ControlUiBridge
{
public:
    std::optional<ProjectInfo> projectInfo() const override
    {
        if (session == nullptr)
            return project;
        // With a project session, the session is the truth, as in the editor.
        ProjectInfo info;
        if (session->hasProjectPath())
            info.projectPath = session->getCurrentProjectFile().getFullPathName().toStdString();
        info.dirty = session->isDirty();
        return info;
    }
    void markDirty() override
    {
        ++dirtyMarks;
        project.dirty = true;
        if (session != nullptr)
            session->markDirty();
    }

    std::optional<BusyState> queryBusy() const override { return busy; }

    void onStructureChanged() override { ++structureChanges; }
    void onUndoRedoApplied() override { ++undoRedoNotifications; }
    void onProjectReloaded() override
    {
        ++projectReloads;
        if (trackCountArrangement == nullptr)
            return;
        // refreshAllUIFromProject(): the panel takes the count of the restored arrangement while
        // applyingLoadedProjectUi_ is set, so that the change does not mark the project.
        applyingLoadedProjectUi = true;
        panelShowsTracks(trackCountArrangement->getVisibleTrackCount());
        applyingLoadedProjectUi = false;
    }
    void onProjectSaved() override { ++projectSaves; }
    void onRecentProjectsChanged() override { ++recentProjectChanges; }
    void setProjectOperationRunning(bool running) override
    {
        busy.operationRunning = running;
        operationFlagChanges.push_back(running);
    }
    ProjectSession* projectSession() override { return session; }
    juce::ThreadPool* projectWorkerPool() override { return pool; }
    void onTimeGridChanged() override { ++timeGridNotifications; }

    /** The editor's applyPlacementSelectionContext(): the clip becomes the selected clip of the active track, which
        loads it into the piano roll without switching the view (a selection shows the clip in the piano roll even
        while the arrangement is shown). */
    bool focusPlacement(int trackId, std::uint64_t placementId) override
    {
        focused.emplace_back(trackId, placementId);
        view.pianoRollPlacementId = placementId;
        return true;
    }
    bool revealTracks(int count) override
    {
        ++revealCalls;
        if (trackCountArrangement == nullptr) {
            visibleTracks = std::max(visibleTracks, count);
            return true;
        }
        count = std::clamp(count, 1, OpenTuneAudioProcessor::MAX_TRACKS);
        if (visibleTracks < count)
            panelShowsTracks(count);
        return true;
    }

    std::optional<ViewState> viewState() const override { return view; }
    std::optional<std::vector<std::uint64_t>> selectedPlacementIds() const override { return selection; }
    std::optional<int> visibleTrackCount() const override { return visibleTracks; }

    /** Injected state. */
    ProjectInfo project;
    BusyState busy;
    ViewState view;
    std::vector<std::uint64_t> selection;
    int visibleTracks = 1;
    /** When set, the bridge models what the editor does with the track count: the track panel's count is saved in the
        arrangement (a StandaloneArrangement::setVisibleTrackCount() on every change of the panel, which marks the
        project unless the editor is applying a loaded project: OpenTuneAudioProcessorEditor::visibleTrackCountChanged),
        and a reloaded project sets the panel to the arrangement's count (refreshAllUIFromProject). Left null, the
        count is only a number the test sets. */
    StandaloneArrangement* trackCountArrangement = nullptr;
    /** The project session and its worker pool, as the editor owns them; null for a test without a project. */
    ProjectSession* session = nullptr;
    juce::ThreadPool* pool = nullptr;

    /** Counters of the calls received. */
    int dirtyMarks = 0;
    int structureChanges = 0;
    int undoRedoNotifications = 0;
    int projectReloads = 0;
    int projectSaves = 0;
    int recentProjectChanges = 0;
    /** The values setProjectOperationRunning() was called with, in order. */
    std::vector<bool> operationFlagChanges;
    int timeGridNotifications = 0;
    int revealCalls = 0;
    /** applyingLoadedProjectUi_ of the editor; see trackCountArrangement. */
    bool applyingLoadedProjectUi = false;
    /** The placements focusPlacement() was called with, in order. */
    std::vector<std::pair<int, std::uint64_t>> focused;

    // ---- Dialogs and onboarding ----

    std::optional<std::vector<DialogInfo>> openDialogs() const override
    {
        if (!reportsDialogs)
            return std::nullopt;
        return dialogs;
    }
    /** Closes the topmost dialog, as the editor's bridge does, and runs onDialogClosed for it (what closing it
        commits, which the test decides); the modal flag of busy follows the remaining dialogs. */
    bool closeTopDialog() override
    {
        if (dialogs.empty() || dialogs.back().systemFileChooser || refuseClose)
            return false;
        const auto closed = dialogs.back();
        dialogs.pop_back();
        busy.modalOpen = !dialogs.empty();
        closedDialogs.push_back(closed.kind);
        if (onDialogClosed)
            onDialogClosed(closed);
        return true;
    }
    std::optional<bool> onboardingShowing() const override { return onboarding; }
    bool closeOnboarding() override
    {
        if (!onboarding.value_or(false) || !onboardingClosable)
            return false;
        onboarding = false;
        ++onboardingCloses;
        return true;
    }
    std::optional<UiScale> uiScale() const override { return scale; }

    // ---- Workspace ----

    /** What the editor's bridge does (ControlEditorBridgeWorkspace.cpp): the view alone changes, nothing else is
        touched except the tempo that the BPM field rewrites (commitBpmFieldOnViewSwitch()), and a window that shows
        the view already does nothing. A bridge without the switch returns false. */
    bool setWorkspace(bool arrangement) override
    {
        if (commitsBpmFieldOnFailedSwitch && view.workspace != (arrangement ? "arrangement" : "pianoRoll"))
            commitBpmFieldOnViewSwitch();
        if (!switchesWorkspace)
            return false;
        if (swallowsWorkspaceSwitch)
            return true;
        const std::string target = arrangement ? "arrangement" : "pianoRoll";
        if (view.workspace == target)
            return true;
        ++workspaceSwitches;
        view.workspace = target;
        commitBpmFieldOnViewSwitch();
        return true;
    }

    /** The transport bar's BPM field, as the editor meets it when the view really changes: the switch moves the
        keyboard focus, the focus that leaves the field commits it (BpmValueField::commit shows the tempo with one
        decimal, juce::String(value, 1), and sends that value to OpenTuneAudioProcessorEditor::bpmChanged), and the
        editor sets that tempo and marks the project as changed. The editor's bridge takes the mark back when the tempo
        is what it was (settleViewSwitch), so a tempo with one decimal changes nothing and a tempo with more decimals
        is rewritten and keeps the mark. It does this only while the field holds the focus (focusTempoField()); with
        the focus elsewhere, the default, which is a window in the background, it leaves the tempo alone. A derived
        bridge that switches the view calls this when it does. */
    void commitBpmFieldOnViewSwitch()
    {
        if (bpmFieldProcessor == nullptr)
            return;
        const double before = bpmFieldProcessor->getBpm();
        bpmFieldProcessor->setBpm(juce::String(before, 1).getDoubleValue());
        if (std::abs(bpmFieldProcessor->getBpm() - before) > 1e-9)
            markDirty();
    }

    /** Where the keyboard focus of the window is, in the terms of the given.keyboardFocus of a vector
        (vectors/README.md, Preconditions). Only the tempo field of the transport bar is modelled: the focus that
        leaves it at a switch of the view is what the control API reports. */
    enum class KeyboardFocus {
        elsewhere,
        tempoField
    };

    /** Establishes given.keyboardFocus tempoField: the transport bar's BPM field holds the keyboard focus, as in a
        window that has just started in the foreground, and commits the tempo of processor when the focus leaves it
        (commitBpmFieldOnViewSwitch()). */
    void focusTempoField(OpenTuneAudioProcessor& processor) { bpmFieldProcessor = &processor; }
    /** The keyboard focus is somewhere else, as in a window in the background: a switch of the view leaves the tempo
        alone and does not mark the project. This is the default of a TestUiBridge. */
    void focusElsewhere() { bpmFieldProcessor = nullptr; }
    KeyboardFocus keyboardFocus() const
    {
        return bpmFieldProcessor != nullptr ? KeyboardFocus::tempoField : KeyboardFocus::elsewhere;
    }

    /** The processor whose tempo the BPM field of commitBpmFieldOnViewSwitch() commits; null: the focus is elsewhere
        (focusTempoField() and focusElsewhere() set it). */
    OpenTuneAudioProcessor* bpmFieldProcessor = nullptr;

    /** setWorkspace(): false is a bridge that cannot switch. */
    bool switchesWorkspace = true;
    /** setWorkspace() answers true and shows another view than asked for, as an editor that did not take the switch. */
    bool swallowsWorkspaceSwitch = false;
    /** setWorkspace() moves the keyboard focus before it fails (either failure above), as an editor whose view switch
        breaks off after the focus left the BPM field: the field commits (commitBpmFieldOnViewSwitch()) although the
        view stays. Only a request for the view that is not shown moves the focus. */
    bool commitsBpmFieldOnFailedSwitch = false;
    /** The switches setWorkspace() made, in the number of calls that changed the view. */
    int workspaceSwitches = 0;

    // ---- Quit ----

    bool canQuit() const override { return quits; }
    /** Records the request and does not end the process; onQuitRequest lets a test look at the instance at the moment
        the real bridge would close the window. A bridge that cannot quit does nothing and returns false, and so does a
        window that is asked and does not close (refusesClose; the request is counted in refusedQuitRequests). */
    bool requestQuit() override
    {
        if (!quits)
            return false;
        if (refusesClose) {
            ++refusedQuitRequests;
            if (onQuitRequest)
                onQuitRequest();
            return false;
        }
        ++quitRequests;
        if (onQuitRequest)
            onQuitRequest();
        return true;
    }

    /** canQuit(): false is a bridge whose window cannot be closed (no Standalone window around the editor). */
    bool quits = true;
    /** canQuit() is true, requestQuit() is asked and the window does not close (it returns false). That the editor is
        gone altogether, between the check of app.quit and the exit step, is not modelled by this switch: a test
        unregisters the bridge from the service instead (windowNotClosed(true) in ControlQuitTests.cpp). */
    bool refusesClose = false;
    /** The number of requestQuit() calls that closed the window. */
    int quitRequests = 0;
    /** The number of requestQuit() calls that found nothing to close (refusesClose). */
    int refusedQuitRequests = 0;
    /** Called inside requestQuit(), on the message thread, after the request was counted (also for a refused one). */
    std::function<void()> onQuitRequest;

    /** Injected dialogs, from the bottom of the stack to the top; reportsDialogs false is a bridge without them. */
    std::vector<DialogInfo> dialogs;
    bool reportsDialogs = true;
    /** closeTopDialog() fails, as a dialog that would not close. */
    bool refuseClose = false;
    /** What closing a dialog does in the test (a track-colour dialog: set the track's colour, mark dirty). */
    std::function<void(const DialogInfo&)> onDialogClosed;
    /** The onboarding overlay: showing, not showing, or nullopt for a bridge without it. */
    std::optional<bool> onboarding = false;
    bool onboardingClosable = true;
    std::optional<UiScale> scale = UiScale{};
    /** The kinds closeTopDialog() closed, in order, and the number of closeOnboarding() calls that closed it. */
    std::vector<std::string> closedDialogs;
    int onboardingCloses = 0;

private:
    /** TrackPanelComponent::setVisibleTrackCount() with the editor's listener, visibleTrackCountChanged(): nothing
        happens when the count does not change; otherwise the arrangement takes the count, and a changed count of the
        arrangement marks the project unless a loaded project is being applied. Only with trackCountArrangement. */
    void panelShowsTracks(int count)
    {
        if (count == visibleTracks)
            return;
        visibleTracks = count;
        const int previous = trackCountArrangement->getVisibleTrackCount();
        trackCountArrangement->setVisibleTrackCount(count);
        if (!applyingLoadedProjectUi && previous != trackCountArrangement->getVisibleTrackCount())
            markDirty();
    }
};

} // namespace ControlTest
} // namespace OpenTune
