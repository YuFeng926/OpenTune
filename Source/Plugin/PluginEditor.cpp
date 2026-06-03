#if JucePlugin_Build_VST3

#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

#include "Editor/Preferences/SharedPreferencePages.h"
#include "Editor/Preferences/StandalonePreferencePages.h"
#include "Editor/Preferences/TabbedPreferencesDialog.h"
#include "Plugin/Capture/CaptureSession.h"
#include "Utils/AppLogger.h"
#include "Utils/KeyShortcutConfig.h"
#include "Utils/ParameterPanelSync.h"
#include "Utils/Note.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/PitchShiftSettings.h"
#include "Utils/PitchShiftEditAction.h"
#include "Editor/PitchShiftDialogContent.h"
#include "Utils/TimeCoordinate.h"
#include "UI/UiAssets.h"
#include "UI/FrameScheduler.h"

#if JucePlugin_Enable_ARA
#include "ARA/OpenTuneDocumentController.h"
#endif

namespace OpenTune::PluginUI {

namespace {

void showHostManagedMessage(const juce::String& title, const juce::String& detail)
{
    AppLogger::log("VST3Editor: " + title + " requested, delegated to host DAW");
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                           title,
                                           "In VST3 mode this action is managed by your DAW.\n\n"
                                               + detail);
}

bool nearlyEqualSeconds(double a, double b)
{
    return std::abs(a - b) <= (1.0 / TimeCoordinate::kRenderSampleRate);
}

MaterializationTimelineProjection makeCaptureSegmentProjection(const Capture::SegmentInfo& segment)
{
    MaterializationTimelineProjection projection;
    projection.timelineStartSeconds = segment.T_start;
    projection.timelineDurationSeconds = segment.durationSeconds;
    projection.materializationDurationSeconds = segment.durationSeconds;
    return projection;
}

TimelineMaterializationPlacement makePlacement(uint64_t materializationId,
                                               const MaterializationTimelineProjection& projection)
{
    TimelineMaterializationPlacement placement;
    placement.materializationId = materializationId;
    placement.projection = projection;
    return placement;
}

uint64_t chooseActiveCaptureMaterialization(Capture::CaptureSession& session,
                                            double hostTimeSeconds)
{
    Capture::SegmentInfo activeSegment;
    if (session.resolveDisplaySegment(hostTimeSeconds, activeSegment))
        return activeSegment.materializationId;

    return 0;
}

juce::String buildRenderingOverlayTitle(int completedTasks, int totalTasks, float progress)
{
    if (totalTasks <= 0)
        return juce::String::fromUTF8("\xe6\xad\xa3\xe5\x9c\xa8\xe6\xb8\xb2\xe6\x9f\x93\xe4\xb8\xad");
    const int pct = static_cast<int>(std::round(progress * 100.0f));
    return juce::String::fromUTF8("\xe6\xb8\xb2\xe6\x9f\x93\xe4\xb8\xad ")
        + juce::String(pct) + "% ("
        + juce::String(completedTasks) + "/"
        + juce::String(totalTasks) + ")";
}

#if JucePlugin_Enable_ARA
MaterializationTimelineProjection makePianoRollLocalProjection(
    const OpenTuneDocumentController::PlaybackRegionProjection& region)
{
    MaterializationTimelineProjection projection;
    projection.timelineStartSeconds = region.startInPlaybackTime;
    projection.timelineDurationSeconds = region.durationInPlaybackTime;
    projection.materializationDurationSeconds = region.materializationDurationSeconds;
    return projection;
}
#endif

} // namespace

OpenTuneAudioProcessorEditor::OpenTuneAudioProcessorEditor(OpenTuneAudioProcessor& processor)
    : AudioProcessorEditor(&processor)
#if JucePlugin_Enable_ARA
    , juce::AudioProcessorEditorARAExtension(&processor)
#endif
    , processorRef_(processor)
    , languageState_(std::make_shared<LocalizationManager::LanguageState>(
          LocalizationManager::LanguageState{ appPreferences_.getState().shared.language }))
    , languageBinding_(languageState_)
    , menuBar_(processor, MenuBarComponent::Profile::Plugin)
    , topBar_(menuBar_, transportBar_)
{
    menuBar_.setVisible(false);

    addAndMakeVisible(topBar_);
    addAndMakeVisible(parameterPanel_);
    addAndMakeVisible(pianoRoll_);
    addAndMakeVisible(autoRenderOverlay_);
    autoRenderOverlay_.setVisible(false);
    addAndMakeVisible(renderBadge_);
    renderBadge_.setVisible(false);

    LocalizationManager::getInstance().addListener(this);
    menuBar_.addListener(this);
    transportBar_.addListener(this);
    parameterPanel_.addListener(this);
    pianoRoll_.addListener(this);
    transportBar_.setLayoutProfile(TransportBarComponent::LayoutProfile::VST3AraSingleClip);

    pianoRoll_.setProcessor(&processorRef_);
    pianoRoll_.setPianoKeyAudition(&processorRef_.getPianoKeyAudition());
    pianoRoll_.setPlayheadPositionSource(processorRef_.getPositionAtomic());

    transportBar_.setPlaying(processorRef_.isPlaying());
    transportBar_.setLoopEnabled(processorRef_.isLoopEnabled());
    transportBar_.setBpm(processorRef_.getBpm());
    pianoRoll_.setBpm(processorRef_.getBpm());
    pianoRoll_.setTimeSignature(processorRef_.getTimeSigNumerator(), processorRef_.getTimeSigDenominator());

    transportBar_.onFileMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        auto menu = menuBar_.getMenuForIndex(0, menuNames.isEmpty() ? juce::String() : menuNames[0]);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&transportBar_.getFileButton()).withParentComponent(this),
                           [this](int result) {
                               if (result != 0) {
                                   menuBar_.menuItemSelected(result, 0);
                               }
                           });
    };

    transportBar_.onEditMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        auto menu = menuBar_.getMenuForIndex(1, menuNames.size() > 1 ? menuNames[1] : juce::String());
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&transportBar_.getEditButton()).withParentComponent(this),
                           [this](int result) {
                               if (result != 0) {
                                   menuBar_.menuItemSelected(result, 1);
                               }
                           });
    };

    transportBar_.onViewMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        auto menu = menuBar_.getMenuForIndex(2, menuNames.size() > 2 ? menuNames[2] : juce::String());
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&transportBar_.getViewButton()).withParentComponent(this),
                           [this](int result) {
                               if (result != 0) {
                                   menuBar_.menuItemSelected(result, 2);
                               }
                           });
    };

    topBar_.onToggleTrackPanel = nullptr;
    topBar_.setTrackPanelToggleVisible(false);
    topBar_.onToggleParameterPanel = [this]() {
        parameterPanel_.setVisible(!parameterPanel_.isVisible());
        resized();
    };
    topBar_.setSidePanelsVisible(false, true);

    applyThemeToEditor(appPreferences_.getState().shared.theme);

    setWantsKeyboardFocus(true);
    setResizable(true, true);
    setResizeLimits(960, 640, 3000, 2000);
    setSize(1280, 820);

    syncMaterializationProjectionToPianoRoll();
    syncSharedAppPreferences();
    updateRegularCaptureSessionCallback();

    startTimerHz(kHeartbeatHz);

    // 启动时应用持久化声码器权重偏�?
    const auto weight = appPreferences_.getState().shared.vocoderModelWeight;
    processorRef_.setVocoderModelWeight(weight);
    // 幂等：weight==Community �?setVocoderModelWeight �?return early
}

OpenTuneAudioProcessorEditor::~OpenTuneAudioProcessorEditor()
{
    stopTimer();
    clearRegularCaptureSessionCallback();
    LocalizationManager::getInstance().removeListener(this);
    pianoRoll_.removeListener(this);
    parameterPanel_.removeListener(this);
    transportBar_.removeListener(this);
    menuBar_.removeListener(this);
    setLookAndFeel(nullptr);
}

void OpenTuneAudioProcessorEditor::paint(juce::Graphics& g)
{
    if (UIColors::currentThemeId() == ThemeId::Overdose)
    {
        UiAssets::drawAssetCover(g, UiAssetId::BackgroundMain, getLocalBounds().toFloat());
        return;
    }

    g.fillAll(UIColors::backgroundDark);
}

void OpenTuneAudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
    // 确保点击 Editor 背景时焦点转�?PianoRoll（按键可达）
    if (!pianoRoll_.hasKeyboardFocus(true))
        pianoRoll_.grabKeyboardFocus();
    juce::AudioProcessorEditor::mouseDown(e);
}

void OpenTuneAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    topBar_.setBounds(bounds.removeFromTop(TOP_BAR_HEIGHT));

    if (parameterPanel_.isVisible()) {
        parameterPanel_.setBounds(bounds.removeFromRight(PARAMETER_PANEL_WIDTH));
    } else {
        parameterPanel_.setBounds({});
    }

    pianoRoll_.setBounds(bounds);
    autoRenderOverlay_.setBounds(bounds);
    autoRenderOverlay_.toFront(false);
    renderBadge_.setBounds(bounds.getRight() - 148, bounds.getY() + 8, 140, 28);
    renderBadge_.toFront(false);
}

void OpenTuneAudioProcessorEditor::syncParameterPanelFromSelection()
{
    ParameterPanelSyncContext context;
    context.clipRetuneSpeedPercent = pianoRoll_.getCurrentRetuneSpeed() * 100.0f;
    context.clipVibratoDepth = pianoRoll_.getCurrentVibratoDepth();
    context.clipVibratoRate = pianoRoll_.getCurrentVibratoRate();
    context.wasShowingSelectionParameters = showingSingleNoteParams_;

    context.hasSelectedNoteParameters = pianoRoll_.getSingleSelectedNoteParameters(
        context.selectedNoteRetuneSpeedPercent,
        context.selectedNoteVibratoDepth,
        context.selectedNoteVibratoRate);

    const auto scheme = appPreferences_.getState().shared.audioEditingScheme;
    const auto decision = resolveParameterPanelSyncDecision(scheme, context);
    if (decision.shouldSetRetuneSpeed) {
        parameterPanel_.setRetuneSpeed(decision.retuneSpeedPercent);
    }
    if (decision.shouldSetVibratoDepth) {
        parameterPanel_.setVibratoDepth(decision.vibratoDepth);
    }
    if (decision.shouldSetVibratoRate) {
        parameterPanel_.setVibratoRate(decision.vibratoRate);
    }

    showingSingleNoteParams_ = decision.nextShowingSelectionParameters;
}

void OpenTuneAudioProcessorEditor::timerCallback()
{
    // 首次 timer 回调时确�?PianoRoll 获取焦点（VST3 嵌入时序可能导致 visibilityChanged 中的 grab 失败�?
    if (!initialFocusGrabbed_ && isShowing()) {
        initialFocusGrabbed_ = true;
        pianoRoll_.grabKeyboardFocus();
    }

    syncSharedAppPreferences();
    updateRegularCaptureSessionCallback();

    // Drive capture session tick (poll Capturing→Pending and Processing→Edited transitions,
    // run reclaim sweep). No-op when capture session is null (Standalone / VST3+ARA).
    if (auto* session = processorRef_.getCaptureSession()) {
        session->tick();
        // Drive record button visual state from capture session state:
        //   HasCapturing �?Capturing (toggled + enabled)
        //   HasProcessing �?Processing (disabled to prevent re-trigger)
        //   Idle �?Idle (normal appearance)
        using OpenTune::Capture::SessionState;
        const auto captureState = session->getGlobalState();
        if (captureState == SessionState::HasCapturing)
            transportBar_.setRecordButtonState(OpenTune::RecordButtonState::Capturing);
        else if (captureState == SessionState::HasProcessing)
            transportBar_.setRecordButtonState(OpenTune::RecordButtonState::Processing);
        else
            transportBar_.setRecordButtonState(OpenTune::RecordButtonState::Idle);
    }

    const double currentPositionSeconds = processorRef_.getPosition();
    const bool playing = processorRef_.isPlaying();
    const bool loopEnabled = processorRef_.isLoopEnabled();
    transportBar_.setPositionSeconds(currentPositionSeconds);

    syncParameterPanelFromSelection();

    const double bpm = processorRef_.getBpm();
    if (bpm > 0.0 && std::abs(bpm - lastSyncedBpm_) > 0.001) {
        transportBar_.setBpm(bpm);
        pianoRoll_.setBpm(bpm);
        lastSyncedBpm_ = bpm;
    }

    const int timeSigNum = processorRef_.getTimeSigNumerator();
    const int timeSigDenom = processorRef_.getTimeSigDenominator();
    if (timeSigNum > 0 && timeSigDenom > 0
        && (timeSigNum != lastSyncedTimeSigNum_ || timeSigDenom != lastSyncedTimeSigDenom_)) {
        pianoRoll_.setTimeSignature(timeSigNum, timeSigDenom);
        lastSyncedTimeSigNum_ = timeSigNum;
        lastSyncedTimeSigDenom_ = timeSigDenom;
    }

    if (transportBar_.isPlaying() != playing) {
        transportBar_.setPlaying(playing);
        pianoRoll_.setIsPlaying(playing);
        FrameScheduler::instance().setTimelinePlaybackActive(playing);
    }
    transportBar_.setLoopEnabled(loopEnabled);

    // Drive PianoRoll heartbeat first so autoTuneInFlight_ is up-to-date
    if (pianoRoll_.isShowing()) {
        pianoRoll_.onHeartbeatTick();
    }

    bool shouldShowOverlay = false;

    const uint64_t activeMaterializationId = resolveCurrentMaterializationId();
    const auto chunkStats = processorRef_.getMaterializationChunkStatsById(activeMaterializationId);
    const bool isAutoProcessing = pianoRoll_.isAutoTuneProcessing();
    const bool hasActiveRender = chunkStats.hasActiveWork();

    // Pull fresh notes when an async generator (GAME) commits late.  Only
    // refresh when the same materialization advances its notesRevision �?
    // changing materializationId already triggers a refresh via
    // syncMaterializationProjectionToPianoRoll �?setEditedMaterialization.
    if (activeMaterializationId != 0) {
        const uint64_t currentNotesRevision =
            processorRef_.getMaterializationNotesSnapshotById(activeMaterializationId).notesRevision;
        if (activeMaterializationId == lastPianoRollNotesRevisionMatId_
            && currentNotesRevision != lastPianoRollNotesRevision_
            && pianoRoll_.isShowing()) {
            pianoRoll_.refreshEditedMaterializationNotes();
            pianoRoll_.requestContentRedraw();
        }
        lastPianoRollNotesRevisionMatId_ = activeMaterializationId;
        lastPianoRollNotesRevision_      = currentNotesRevision;
    } else {
        lastPianoRollNotesRevisionMatId_ = 0;
        lastPianoRollNotesRevision_      = 0;
    }

    // Pull fresh TimeGrid when external commits / undo-redo publish silently.
    // Revision increments on every setMaterializationTimeGridById() call;
    // tool-handler edits fire notifyTimeGridChanged with Interactive priority
    // for sub-frame latency; this polling guard catches the non-interactive paths.
    if (activeMaterializationId != 0) {
        const uint64_t currentTimeGridRevision =
            processorRef_.getMaterializationTimeGridRevisionById(activeMaterializationId);
        if (activeMaterializationId == lastPianoRollTimeGridRevisionMatId_
            && currentTimeGridRevision != lastPianoRollTimeGridRevision_
            && pianoRoll_.isShowing()) {
            pianoRoll_.requestContentRedraw();
        }
        lastPianoRollTimeGridRevisionMatId_ = activeMaterializationId;
        lastPianoRollTimeGridRevision_      = currentTimeGridRevision;
    } else {
        lastPianoRollTimeGridRevisionMatId_ = 0;
        lastPianoRollTimeGridRevision_      = 0;
    }

    if (isAutoProcessing) {
        const int total = chunkStats.total();
        const int done = chunkStats.idle + chunkStats.blank;
        const float progress = (total > 0) ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
        autoRenderOverlay_.setMessageText(buildRenderingOverlayTitle(done, total, progress));
        shouldShowOverlay = true;
    }

    // Waiting for ARA materialization birth (Read Audio) �?blocking overlay with spinner.
    // Auto-dismissed when the materialization is ready (detected via resolveCurrentMaterializationId).
    if (waitingForAraMaterialization_) {
        if (activeMaterializationId != 0) {
            waitingForAraMaterialization_ = false;
        } else {
            // Safety timeout: if birth takes > 60s, dismiss to avoid trapping user.
            const auto nowMs = juce::Time::getApproximateMillisecondCounter();
            if (nowMs - araWaitStartMs_ > 60000) {
                AppLogger::log("ReadAudio: ARA materialization birth timed out after 60s");
                waitingForAraMaterialization_ = false;
            } else {
                shouldShowOverlay = true;
            }
        }
    }

    if (autoRenderOverlay_.isVisible() != shouldShowOverlay) {
        autoRenderOverlay_.setVisible(shouldShowOverlay);
    }

    // Lightweight badge for non-AUTO render
    const bool shouldShowBadge = hasActiveRender && !shouldShowOverlay;
    if (shouldShowBadge) {
        const int total = chunkStats.total();
        const int done = chunkStats.idle + chunkStats.blank;
        renderBadge_.setMessageText(juce::String::fromUTF8(u8"\u6e32\u67d3\u4e2d (")
            + juce::String(done) + "/" + juce::String(total) + ")");
    }
    if (renderBadge_.isVisible() != shouldShowBadge) {
        renderBadge_.setVisible(shouldShowBadge);
    }

    // Unified materialization �?PianoRoll sync (projection + curve + buffer + scale)
    syncMaterializationProjectionToPianoRoll();

    // Consume audio-thread log events on message thread (see AudioThreadLogEvent).
    processorRef_.consumeAudioThreadLogs();
}

void OpenTuneAudioProcessorEditor::syncSharedAppPreferences()
{
    const auto sharedPreferences = appPreferences_.getState().shared;
    const auto& visualPreferences = sharedPreferences.pianoRollVisualPreferences;

    if (languageState_ != nullptr) {
        languageState_->language = sharedPreferences.language;
    }

    if (appliedLanguage_ != sharedPreferences.language) {
        appliedLanguage_ = sharedPreferences.language;
        LocalizationManager::getInstance().notifyLanguageChanged(sharedPreferences.language);
    }

    if (appliedThemeId_ != sharedPreferences.theme)
        applyThemeToEditor(sharedPreferences.theme);

    pianoRoll_.setAudioEditingScheme(sharedPreferences.audioEditingScheme);
    pianoRoll_.setZoomSensitivity(sharedPreferences.zoomSensitivity);
    pianoRoll_.setNoteNameMode(visualPreferences.noteNameMode);
    pianoRoll_.setShowChunkBoundaries(visualPreferences.showChunkBoundaries);
    pianoRoll_.setShowUnvoicedFrames(visualPreferences.showUnvoicedFrames);
    menuBar_.setNoteNameMode(visualPreferences.noteNameMode);
    menuBar_.setShowChunkBoundaries(visualPreferences.showChunkBoundaries);
    menuBar_.setShowUnvoicedFrames(visualPreferences.showUnvoicedFrames);
    pianoRoll_.setShortcutSettings(appPreferences_.getState().shared.shortcuts);
}

void OpenTuneAudioProcessorEditor::languageChanged(Language newLanguage)
{
    juce::ignoreUnused(newLanguage);

    menuBar_.menuItemsChanged();
    menuBar_.repaint();
    topBar_.refreshLocalizedText();
    parameterPanel_.refreshLocalizedText();
    repaint();
}

uint64_t OpenTuneAudioProcessorEditor::resolveCurrentMaterializationId()
{
    return resolveCurrentMaterializationSync().activeMaterializationId;
}

OpenTuneAudioProcessorEditor::PianoRollMaterializationSync
OpenTuneAudioProcessorEditor::resolveCurrentMaterializationSync()
{
    PianoRollMaterializationSync sync;

#if JucePlugin_Enable_ARA
    if (const auto* dc = processorRef_.getDocumentController()) {
        const auto regions = dc->getPlaybackRegionProjections();
        for (const auto& region : regions) {
            const auto materializationId = region.materializationId;
            if (materializationId == 0)
                continue;

            sync.placements.push_back(makePlacement(materializationId,
                                                    makePianoRollLocalProjection(region)));
        }

        if (const auto focusedRegion = dc->getFocusedEditorPlaybackRegionProjection()) {
            sync.activeMaterializationId = focusedRegion->materializationId;
        }

        const bool activeBelongsToPlacements = std::any_of(sync.placements.begin(),
                                                           sync.placements.end(),
                                                           [&sync](const auto& placement) {
                                                               return placement.materializationId == sync.activeMaterializationId;
                                                           });
        if (!activeBelongsToPlacements)
            sync.activeMaterializationId = 0;

        return sync;
    }
#endif

    if (auto* session = processorRef_.getCaptureSession()) {
        double viewEndSeconds = 0.0;
        for (const auto& segment : session->listEditedSegments()) {
            const auto projection = makeCaptureSegmentProjection(segment);
            sync.placements.push_back(makePlacement(segment.materializationId, projection));
            viewEndSeconds = std::max(viewEndSeconds, projection.timelineEndSeconds());
        }

        if (!sync.placements.empty()) {
            sync.activeMaterializationId = chooseActiveCaptureMaterialization(*session, processorRef_.getPosition());
            const bool activeBelongsToPlacements = std::any_of(sync.placements.begin(),
                                                               sync.placements.end(),
                                                               [&sync](const auto& placement) {
                                                                   return placement.materializationId == sync.activeMaterializationId;
                                                               });
            if (!activeBelongsToPlacements)
                sync.activeMaterializationId = 0;

            sync.usesRegularCaptureTimelineDomain = true;
            sync.timelineViewStartSeconds = 0.0;
            sync.timelineViewEndSeconds = viewEndSeconds;
        }
    }

    return sync;
}

void OpenTuneAudioProcessorEditor::updateRegularCaptureSessionCallback()
{
    auto* session = processorRef_.getCaptureSession();
    if (session == regularCaptureCallbackSession_)
        return;

    clearRegularCaptureSessionCallback();

    if (session == nullptr)
        return;

    regularCaptureCallbackSession_ = session;
    session->setActiveSegmentChangedCallback([this](uint64_t materializationId) {
        AppLogger::log("VST3 Capture: completed materializationId="
            + juce::String(static_cast<juce::int64>(materializationId)));
        syncMaterializationProjectionToPianoRoll();
    });
}

void OpenTuneAudioProcessorEditor::clearRegularCaptureSessionCallback()
{
    if (regularCaptureCallbackSession_ != nullptr) {
        regularCaptureCallbackSession_->setActiveSegmentChangedCallback(nullptr);
        regularCaptureCallbackSession_ = nullptr;
    }
}

bool OpenTuneAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    return handleEditorShortcut(key);
}

bool OpenTuneAudioProcessorEditor::handleEditorShortcut(const juce::KeyPress& key)
{
    const auto& shortcutSettings = appPreferences_.getState().shared.shortcuts;

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::PlayPause, key)) {
        playPauseToggleRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Stop, key)) {
        stopPlaybackRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Undo, key)) {
        undoRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Redo, key)) {
        redoRequested();
        return true;
    }

    return false;
}

void OpenTuneAudioProcessorEditor::retuneSpeedChanged(float speed)
{
    const float normalized = speed / 100.0f;
    pianoRoll_.setRetuneSpeed(normalized);
    if (pianoRoll_.applyRetuneSpeedToSelection(normalized)) {
        return;
    }
    const float depth = parameterPanel_.getVibratoDepth();
    const float rate = parameterPanel_.getVibratoRate();
    pianoRoll_.applyCorrectionAsyncForEntireClip(normalized, depth, rate);
}

void OpenTuneAudioProcessorEditor::vibratoDepthChanged(float value)
{
    if (pianoRoll_.applyVibratoDepthToSelection(value)) {
        return;
    }
    pianoRoll_.setVibratoDepth(value);
    const float speed = parameterPanel_.getRetuneSpeed() / 100.0f;
    const float rate = parameterPanel_.getVibratoRate();
    pianoRoll_.applyCorrectionAsyncForEntireClip(speed, value, rate);
}

void OpenTuneAudioProcessorEditor::vibratoRateChanged(float value)
{
    if (pianoRoll_.applyVibratoRateToSelection(value)) {
        return;
    }
    pianoRoll_.setVibratoRate(value);
    const float speed = parameterPanel_.getRetuneSpeed() / 100.0f;
    const float depth = parameterPanel_.getVibratoDepth();
    pianoRoll_.applyCorrectionAsyncForEntireClip(speed, depth, value);
}

void OpenTuneAudioProcessorEditor::noteSplitChanged(float value)
{
    pianoRoll_.setNoteSplit(value);
}

void OpenTuneAudioProcessorEditor::toolSelected(int toolId)
{
    if (toolId < 0 || toolId > static_cast<int>(ToolId::TimeTool)) {
        return;
    }

    const auto tool = static_cast<ToolId>(toolId);
    pianoRoll_.setCurrentTool(tool);
}

void OpenTuneAudioProcessorEditor::importAudioRequested()
{
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                           "Import Audio",
                                           "Please import audio from your DAW in VST3 mode.");
}

void OpenTuneAudioProcessorEditor::exportAudioRequested(MenuBarComponent::ExportType exportType)
{
    juce::ignoreUnused(exportType);
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                           "Export Audio",
                                           "Please render/export from your DAW in VST3 mode.");
}

void OpenTuneAudioProcessorEditor::openProjectRequested()
{
    showHostManagedMessage("Open Project",
                           "Project file management is handled in the Standalone version.");
}

void OpenTuneAudioProcessorEditor::saveProjectRequested()
{
    showHostManagedMessage("Save Project",
                           "Project file management is handled in the Standalone version.");
}

void OpenTuneAudioProcessorEditor::saveProjectAsRequested()
{
    showHostManagedMessage("Save Project As...",
                           "Project file management is handled in the Standalone version.");
}

void OpenTuneAudioProcessorEditor::openRecentProjectRequested(const juce::File&)
{
    showHostManagedMessage("Open Recent Project",
                           "Project file management is handled in the Standalone version.");
}

void OpenTuneAudioProcessorEditor::clearRecentProjectsRequested()
{
    // No-op in VST3 mode
}

void OpenTuneAudioProcessorEditor::preferencesRequested()
{
    showPreferencesDialog();
}

void OpenTuneAudioProcessorEditor::showPreferencesDialog()
{
    auto pages = SharedPreferencePages::create(appPreferences_, [this] { syncSharedAppPreferences(); });

    // Insert Audio page (with rendering priority) at the beginning
    auto onVocoderModelWeightChanged = [this](VocoderModelWeight weight) {
        processorRef_.setVocoderModelWeight(weight);
    };
    auto audioPage = SharedPreferencePages::createRenderingPriorityComponent(
        appPreferences_, [this] { syncSharedAppPreferences(); },
        [this](bool forceCpu) { processorRef_.resetInferenceBackend(forceCpu); },
        std::move(onVocoderModelWeightChanged));
    pages.insert(pages.begin(), { LOC(kAudio), std::move(audioPage) });

    // Append standalone-only pages (Keyswitch, MouseTrail)
    auto standalonePages = StandalonePreferencePages::createStandaloneOnlyPages(
        appPreferences_, [this] { syncSharedAppPreferences(); });
    for (auto& page : standalonePages) {
        pages.push_back(std::move(page));
    }

    auto* dialogContent = new TabbedPreferencesDialog(std::move(pages));
    dialogContent->setSize(640, 560);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialogContent);
    options.dialogTitle = "Preferences";
    options.componentToCentreAround = this;
    options.dialogBackgroundColour = UIColors::backgroundDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.launchAsync();
}

void OpenTuneAudioProcessorEditor::helpRequested()
{
    showHostManagedMessage("Help",
                           "Open the host DAW plugin help/manual entry for VST3 usage guidance.");
}

void OpenTuneAudioProcessorEditor::showWaveformToggled(bool shouldShow)
{
    pianoRoll_.setShowWaveform(shouldShow);
}

void OpenTuneAudioProcessorEditor::showLanesToggled(bool shouldShow)
{
    pianoRoll_.setShowLanes(shouldShow);
}

void OpenTuneAudioProcessorEditor::noteNameModeChanged(NoteNameMode noteNameMode)
{
    if (appPreferences_.getState().shared.pianoRollVisualPreferences.noteNameMode != noteNameMode) {
        appPreferences_.setNoteNameMode(noteNameMode);
    }

    syncSharedAppPreferences();
    menuBar_.repaint();
}

void OpenTuneAudioProcessorEditor::showChunkBoundariesToggled(bool shouldShow)
{
    if (appPreferences_.getState().shared.pianoRollVisualPreferences.showChunkBoundaries != shouldShow) {
        appPreferences_.setShowChunkBoundaries(shouldShow);
    }

    syncSharedAppPreferences();
    menuBar_.repaint();
}

void OpenTuneAudioProcessorEditor::showUnvoicedFramesToggled(bool shouldShow)
{
    if (appPreferences_.getState().shared.pianoRollVisualPreferences.showUnvoicedFrames != shouldShow) {
        appPreferences_.setShowUnvoicedFrames(shouldShow);
    }

    syncSharedAppPreferences();
    menuBar_.repaint();
}

void OpenTuneAudioProcessorEditor::themeChanged(ThemeId themeId)
{
    if (appPreferences_.getState().shared.theme != themeId) {
        appPreferences_.setTheme(themeId);
    }

    applyThemeToEditor(themeId);
}

void OpenTuneAudioProcessorEditor::applyThemeToEditor(ThemeId themeId)
{

    appliedThemeId_ = themeId;
    UIColors::applyTheme(themeId);

    if (themeId == ThemeId::Aurora) {
        setLookAndFeel(&auroraLookAndFeel_);
    } else {
        setLookAndFeel(&openTuneLookAndFeel_);
        openTuneLookAndFeel_.setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
        openTuneLookAndFeel_.setColour(juce::TextButton::buttonOnColourId, UIColors::accent);
        openTuneLookAndFeel_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        openTuneLookAndFeel_.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
    }

    topBar_.applyTheme();
    parameterPanel_.applyTheme();
    pianoRoll_.setPlayheadColour(UIColors::playhead);
    sendLookAndFeelChange();
    repaint();
}

void OpenTuneAudioProcessorEditor::undoRequested()
{
    auto* action = processorRef_.getUndoManager().undo();
    if (!action) return;
    syncMaterializationProjectionToPianoRoll();
    const uint64_t matId = resolveCurrentMaterializationId();
    if (matId == 0) return;

    auto curve = processorRef_.getMaterializationPitchCurveById(matId);
    if (!curve || !curve->getSnapshot()->hasRenderableCorrectedF0()) return;

    double startSec = 0.0;
    double endSec = pianoRoll_.getMaterializationDurationSeconds();
    auto* editAction = dynamic_cast<OpenTune::PianoRollEditAction*>(action);
    if (editAction && editAction->getMaterializationId() == matId && editAction->getAffectedEndFrame() > 0) {
        const double spf = static_cast<double>(curve->getHopSize()) / curve->getSampleRate();
        startSec = static_cast<double>(editAction->getAffectedStartFrame()) * spf;
        endSec = static_cast<double>(editAction->getAffectedEndFrame()) * spf;
    }
    processorRef_.enqueueMaterializationPartialRenderById(matId, startSec, endSec);
}

void OpenTuneAudioProcessorEditor::redoRequested()
{
    auto* action = processorRef_.getUndoManager().redo();
    if (!action) return;
    syncMaterializationProjectionToPianoRoll();
    const uint64_t matId = resolveCurrentMaterializationId();
    if (matId == 0) return;

    auto curve = processorRef_.getMaterializationPitchCurveById(matId);
    if (!curve || !curve->getSnapshot()->hasRenderableCorrectedF0()) return;

    double startSec = 0.0;
    double endSec = pianoRoll_.getMaterializationDurationSeconds();
    auto* editAction = dynamic_cast<OpenTune::PianoRollEditAction*>(action);
    if (editAction && editAction->getMaterializationId() == matId && editAction->getAffectedEndFrame() > 0) {
        const double spf = static_cast<double>(curve->getHopSize()) / curve->getSampleRate();
        startSec = static_cast<double>(editAction->getAffectedStartFrame()) * spf;
        endSec = static_cast<double>(editAction->getAffectedEndFrame()) * spf;
    }
    processorRef_.enqueueMaterializationPartialRenderById(matId, startSec, endSec);
}

void OpenTuneAudioProcessorEditor::mouseTrailThemeChanged(MouseTrailConfig::TrailTheme theme)
{
    juce::ignoreUnused(theme);
}

void OpenTuneAudioProcessorEditor::playRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        docController->requestStartPlayback();
        return;
    }
#endif
    surfaceRegularVst3HostControlledTransport("play");
}

void OpenTuneAudioProcessorEditor::pauseRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        docController->requestStopPlayback();
        return;
    }
#endif
    surfaceRegularVst3HostControlledTransport("pause");
}

void OpenTuneAudioProcessorEditor::stopRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        docController->requestStopPlayback();
        docController->requestSetPlaybackPosition(0.0);
        return;
    }
#endif
    surfaceRegularVst3HostControlledTransport("stop");
}

void OpenTuneAudioProcessorEditor::surfaceRegularVst3HostControlledTransport(const char* actionName)
{
    const juce::String action(actionName);
    const juce::String message = "Regular VST3 " + action + " requested: host-controlled transport";
    AppLogger::log("VST3Editor: " + message);
    transportBar_.setRenderStatusText("Host-controlled transport");
}

void OpenTuneAudioProcessorEditor::loopToggled(bool enabled)
{
    processorRef_.setLoopEnabled(enabled);
}

void OpenTuneAudioProcessorEditor::bpmChanged(double newBpm)
{
    processorRef_.setBpm(newBpm);
    pianoRoll_.setBpm(newBpm);
}

void OpenTuneAudioProcessorEditor::scaleChanged(int rootNote, int scaleType)
{
    if (suppressScaleChangedCallback_) {
        return;
    }

    const int clampedRoot = juce::jlimit(0, 11, rootNote);
    const int clampedType = juce::jlimit(1, 8, scaleType);
    pianoRoll_.setScale(clampedRoot, clampedType);

    const uint64_t materializationId = resolveCurrentMaterializationId();
    if (materializationId != 0) {
        DetectedKey key;
        key.root = static_cast<Key>(clampedRoot);
        key.scale = (clampedType == 2) ? Scale::Minor : ((clampedType == 3) ? Scale::Chromatic : Scale::Major);
        key.confidence = 1.0f;
        processorRef_.setMaterializationDetectedKeyById(materializationId, key);
    }
}

void OpenTuneAudioProcessorEditor::viewToggled(bool workspaceView)
{
    if (workspaceView) {
        AppLogger::log("VST3Editor: workspace view request received, enforcing single-clip piano view");
    }

    transportBar_.setWorkspaceView(false);
    pianoRoll_.setVisible(true);
    pianoRoll_.grabKeyboardFocus();
    resized();
    repaint();
}

void OpenTuneAudioProcessorEditor::recordRequested()
{
    if (auto* session = processorRef_.getCaptureSession()) {
        AppLogger::log("VST3 recordRequested mode=regular-vst3 processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(&processorRef_)));
        using OpenTune::Capture::SessionState;
        switch (session->getGlobalState()) {
            case SessionState::Idle:
                session->armNewCapture();
                AppLogger::log("VST3 Capture: armNewCapture (regular-vst3 path)");
                break;
            case SessionState::HasCapturing:
                session->stopCapture();
                AppLogger::log("VST3 Capture: stopCapture (regular-vst3 path)");
                break;
            case SessionState::HasProcessing:
                AppLogger::log("VST3 Capture: ignored (Processing - wait for render)");
                break;
        }
        return;
    }

#if !JucePlugin_Enable_ARA
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                           "Read Audio",
                                           "This VST3 instance is not ready for audio capture or ARA reading.");
    return;
#else
    auto* dc = processorRef_.getDocumentController();
    if (dc == nullptr) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Read Audio",
                                               "This VST3 instance is not ready for audio capture or ARA reading.");
        return;
    }
    AppLogger::log("VST3 recordRequested mode=ara-bound processor="
        + juce::String::toHexString(reinterpret_cast<uintptr_t>(&processorRef_))
        + " dc=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(dc)));
    const auto focusedRegion = dc->getFocusedEditorPlaybackRegionProjection();
    if (!focusedRegion.has_value() || focusedRegion->audioModificationPersistentId.isEmpty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Read Audio",
                                               "No ARA editor selection playback region is available.");
        return;
    }

    // ARA path: recordRequested is the explicit materialization birth boundary.
    uint64_t materializationId = focusedRegion->materializationId;
    if (materializationId == 0)
    {
        // Explicit birth request: recordRequested is the sole entry point.
        dc->requestBirthForFocusedEditorPlaybackRegion();

        waitingForAraMaterialization_ = true;
        araWaitStartMs_ = juce::Time::getApproximateMillisecondCounter();
        autoRenderOverlay_.setMessageText(
            juce::String::fromUTF8("\xe9\x9f\xb3\xe9\xa2\x91\xe5\xa4\x84\xe7\x90\x86\xe4\xb8\xad"),
            "Audio data is being processed. The region will appear shortly.");
        autoRenderOverlay_.setVisible(true);
        return;
    }

    syncMaterializationProjectionToPianoRoll();
    AppLogger::log("RecordTrace: VST3 recordRequested materializationId="
        + juce::String(static_cast<juce::int64>(materializationId)));
#endif
}

void OpenTuneAudioProcessorEditor::playheadPositionChangeRequested(double timeSeconds)
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        docController->requestSetPlaybackPosition(timeSeconds);
        return;
    }
#endif
    // Non-ARA VST3: playhead is host-controlled only. Do NOT call setPosition() �?
    // the host would ignore it and the next processBlock would overwrite the value.
    // PianoRoll click/drag on timeline should not change plugin-internal position.
    juce::ignoreUnused(timeSeconds);
}

void OpenTuneAudioProcessorEditor::playPauseToggleRequested()
{
    if (processorRef_.isPlaying()) {
        pauseRequested();
    } else {
        playRequested();
    }
}

void OpenTuneAudioProcessorEditor::stopPlaybackRequested()
{
    stopRequested();
}

void OpenTuneAudioProcessorEditor::autoTuneRequested()
{
    const uint64_t materializationId = resolveCurrentMaterializationId();
    AppLogger::log("AutoTune: vst3 request materializationId=" + juce::String(static_cast<juce::int64>(materializationId)));
    if (materializationId == 0) {
        return;
    }

    const auto f0State = processorRef_.getMaterializationOriginalF0StateById(materializationId);
    if (f0State == OriginalF0State::Extracting) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "OriginalF0",
            "OriginalF0 is being extracted. Please retry in a moment.");
        return;
    }

    if (f0State == OriginalF0State::Failed) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "OriginalF0",
            "OriginalF0 extraction failed for this clip. Re-import the audio to regenerate OriginalF0.");
        return;
    }

    if (f0State != OriginalF0State::Ready) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "OriginalF0",
            "OriginalF0 is not ready for this clip.");
        return;
    }

    const bool success = pianoRoll_.applyAutoTuneToSelection();
    AppLogger::log("AutoTune: vst3 apply result=" + juce::String(success ? "true" : "false"));
    if (!success) {
        return;
    }

}

void OpenTuneAudioProcessorEditor::pitchShiftRequested()
{
    const uint64_t materializationId = resolveCurrentMaterializationId();
    if (materializationId == 0) return;

    const auto currentSettings = processorRef_.getMaterializationStore()->getPitchShiftSettings(materializationId);

    auto* content = new OpenTune::PitchShiftDialogContent(currentSettings);

    // Listener helper �?applies settings and closes the dialog on confirm/reset
    struct DialogHelper : public OpenTune::PitchShiftDialogContent::Listener
    {
        OpenTuneAudioProcessorEditor* owner;
        uint64_t matId;
        OpenTune::PitchShiftSettings oldSettings;
        juce::Component::SafePointer<juce::Component> contentPtr;

        DialogHelper(OpenTuneAudioProcessorEditor* o, uint64_t m,
                     const OpenTune::PitchShiftSettings& s,
                     juce::Component::SafePointer<juce::Component> c)
            : owner(o), matId(m), oldSettings(s), contentPtr(std::move(c)) {}

        void pitchShiftConfirmed(const OpenTune::PitchShiftSettings& newSettings) override
        {
            if (!owner) return;
            if (newSettings != oldSettings) {
                auto& proc = owner->processorRef_;
                auto& um = proc.getUndoManager();
                um.addAction(std::make_unique<OpenTune::PitchShiftEditAction>(
                    proc, matId, oldSettings, newSettings));
                proc.setPitchShiftSettings(matId, newSettings);
                owner->parameterPanel_.setPitchShiftIndicator(newSettings.semitone, newSettings.cents);
            }
            closeDialog();
        }

        void pitchShiftReset() override
        {
            if (!owner) return;
            const auto identity = OpenTune::PitchShiftSettings::identity();
            if (identity != oldSettings) {
                auto& proc = owner->processorRef_;
                auto& um = proc.getUndoManager();
                um.addAction(std::make_unique<OpenTune::PitchShiftEditAction>(
                    proc, matId, oldSettings, identity));
                proc.setPitchShiftSettings(matId, identity);
                owner->parameterPanel_.setPitchShiftIndicator(0, 0);
            }
            closeDialog();
        }

        void closeDialog()
        {
            if (contentPtr != nullptr) {
                if (auto* dw = contentPtr->findParentComponentOfClass<juce::DialogWindow>()) {
                    dw->exitModalState(0);
                }
            }
        }
    };

    auto* helper = new DialogHelper{this, materializationId, currentSettings,
                                    juce::Component::SafePointer<juce::Component>(content)};
    content->addListener(helper);

    auto options = juce::DialogWindow::LaunchOptions();
    options.content.setOwned(content);
    options.dialogTitle = "Pitch Shift";
    options.dialogBackgroundColour = UIColors::backgroundDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.componentToCentreAround = this;
    options.launchAsync();
}

void OpenTuneAudioProcessorEditor::currentToolChanged(ToolId tool)
{
    parameterPanel_.setActiveTool(static_cast<int>(tool));
}

void OpenTuneAudioProcessorEditor::pitchCurveEdited(int startFrame, int endFrame)
{
    AppLogger::log("AutoTune: pitchCurveEdited startFrame=" + juce::String(startFrame) + " endFrame=" + juce::String(endFrame));
    const uint64_t materializationId = resolveCurrentMaterializationId();
    if (materializationId == 0) {
        AppLogger::log("InvariantViolation: pitchCurveEdited - no active materialization during curve edit callback");
        jassertfalse;
        return;
    }

    auto curve = processorRef_.getMaterializationPitchCurveById(materializationId);
    if (curve == nullptr) {
        AppLogger::log("InvariantViolation: pitchCurveEdited - materialization " + juce::String(static_cast<juce::int64>(materializationId)) + " has no pitch curve");
        jassertfalse;
        return;
    }

    int hopSize = curve->getHopSize();
    double f0SampleRate = curve->getSampleRate();
    if (hopSize <= 0 || f0SampleRate <= 0.0) {
        auto* f0Service = processorRef_.getF0Service();
        if (f0Service != nullptr) {
            hopSize = f0Service->getF0HopSize();
            f0SampleRate = static_cast<double>(f0Service->getF0SampleRate());
        }
    }
    if (hopSize <= 0 || f0SampleRate <= 0.0) {
        return;
    }

    const int numFrames = static_cast<int>(curve->size());
    if (numFrames <= 0) {
        return;
    }

    if (startFrame > endFrame) {
        std::swap(startFrame, endFrame);
    }
    startFrame = juce::jmax(0, startFrame);
    endFrame = juce::jmin(endFrame, numFrames - 1);
    if (endFrame < startFrame) {
        return;
    }

    const double secondsPerFrame = static_cast<double>(hopSize) / f0SampleRate;
    const double editStartSec = static_cast<double>(startFrame) * secondsPerFrame;
    const double editEndSec = static_cast<double>(endFrame + 1) * secondsPerFrame;
    processorRef_.enqueueMaterializationPartialRenderById(materializationId, editStartSec, editEndSec);
}

void OpenTuneAudioProcessorEditor::escapeKeyPressed()
{
    playPauseToggleRequested();
}

void OpenTuneAudioProcessorEditor::syncMaterializationProjectionToPianoRoll()
{
    const auto sync = resolveCurrentMaterializationSync();

    if (!sync.hasPlacements()) {
        pianoRoll_.clearTimelineViewDomain();
        pianoRoll_.setTimelineMaterializationPlacements({});
        pianoRoll_.setEditedMaterialization(0,
                                    nullptr,
                                    nullptr,
                                    static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));
        return;
    }

    if (!sync.hasActiveMaterialization()
        || processorRef_.getMaterializationAudioBufferById(sync.activeMaterializationId) == nullptr) {
        pianoRoll_.setEditedMaterialization(0,
                                    nullptr,
                                    nullptr,
                                    static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));
        pianoRoll_.setTimelineMaterializationPlacements(sync.placements);
        if (sync.usesRegularCaptureTimelineDomain) {
            pianoRoll_.setTimelineViewDomain(sync.timelineViewStartSeconds, sync.timelineViewEndSeconds);
        } else {
            pianoRoll_.clearTimelineViewDomain();
        }
        return;
    }

    auto curve = processorRef_.getMaterializationPitchCurveById(sync.activeMaterializationId);
    auto buffer = processorRef_.getMaterializationAudioBufferById(sync.activeMaterializationId);

    pianoRoll_.setEditedMaterialization(sync.activeMaterializationId,
                                curve,
                                buffer,
                                static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));
    pianoRoll_.setTimelineMaterializationPlacements(sync.placements);
    if (sync.usesRegularCaptureTimelineDomain) {
        pianoRoll_.setTimelineViewDomain(sync.timelineViewStartSeconds, sync.timelineViewEndSeconds);
    } else {
        pianoRoll_.clearTimelineViewDomain();
    }
    const auto key = processorRef_.getMaterializationDetectedKeyById(sync.activeMaterializationId);
    const int rootNote = static_cast<int>(key.root);
    const int scaleType = (key.scale == Scale::Minor) ? 2 : ((key.scale == Scale::Chromatic) ? 3 : 1);

    suppressScaleChangedCallback_ = true;
    transportBar_.setScale(rootNote, scaleType);
    suppressScaleChangedCallback_ = false;
    pianoRoll_.setScale(rootNote, scaleType);
}

} // namespace OpenTune::PluginUI

#endif // JucePlugin_Build_VST3
