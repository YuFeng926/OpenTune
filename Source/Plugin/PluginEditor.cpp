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
#include "Content/ContentKey.h"

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

ContentTimelineProjection makeCaptureSegmentProjection(const Capture::SegmentInfo& segment)
{
    ContentTimelineProjection projection;
    projection.timelineStartSeconds = segment.T_start;
    projection.timelineDurationSeconds = segment.durationSeconds;
    projection.contentDurationSeconds = segment.durationSeconds;
    return projection;
}

TimelineContentPlacement makePlacement(ContentKey contentKey,
                                                const ContentTimelineProjection& projection)
{
    TimelineContentPlacement placement;
    placement.contentKey = contentKey;
    placement.projection = projection;
    return placement;
}

ContentKey chooseActiveCaptureContentKey(Capture::CaptureSession& session,
                                      double hostTimeSeconds)
{
    Capture::SegmentInfo activeSegment;
    if (session.resolveDisplaySegment(hostTimeSeconds, activeSegment))
        return activeSegment.contentKey;
    return {};
}

#if JucePlugin_Enable_ARA
ContentTimelineProjection makePianoRollLocalProjection(
    const OpenTuneDocumentController::PlaybackRegionProjection& region)
{
    ContentTimelineProjection projection;
    projection.timelineStartSeconds = region.startInPlaybackTime;
    projection.timelineDurationSeconds = region.durationInPlaybackTime;
    projection.contentDurationSeconds = region.contentDurationSeconds;
    return projection;
}
#endif

} // anonymous namespace

ContentKey OpenTuneAudioProcessorEditor::resolveCurrentContentKey()
{
    return resolveCurrentContentSync().activeContentKey;
}

OpenTuneAudioProcessorEditor::PianoRollContentSync
OpenTuneAudioProcessorEditor::resolveCurrentContentSync()
{
    PianoRollContentSync sync;

#if JucePlugin_Enable_ARA
    if (const auto* dc = processorRef_.getDocumentController()) {
        const auto regions = dc->getPlaybackRegionProjections();
        for (const auto& region : regions) {
            if (!region.contentKey.isValid())
                continue;

            sync.placements.push_back(makePlacement(region.contentKey,
                                                     makePianoRollLocalProjection(region)));
        }

        // ViewSelection 优先；否则选 timeline 最早的 content-backed placement
        if (const auto focusedRegion = dc->getFocusedEditorPlaybackRegionProjection()) {
            sync.activeContentKey = focusedRegion->contentKey;
        }

        if (!sync.activeContentKey.isValid() && !sync.placements.empty()) {
            const auto earliest = std::min_element(sync.placements.begin(),
                                                    sync.placements.end(),
                                                    [](const auto& a, const auto& b) {
                                                        return a.projection.timelineStartSeconds < b.projection.timelineStartSeconds;
                                                    });
            sync.activeContentKey = earliest->contentKey;
        }

        const bool activeBelongsToPlacements = std::any_of(sync.placements.begin(),
                                                            sync.placements.end(),
                                                            [&sync](const auto& placement) {
                                                                return placement.contentKey == sync.activeContentKey;
                                                            });
        if (!activeBelongsToPlacements)
            sync.activeContentKey = {};

        return sync;
    }
#endif

    if (auto* session = processorRef_.getCaptureSession()) {
        double viewEndSeconds = 0.0;
        for (const auto& segment : session->listEditedSegments()) {
            const auto projection = makeCaptureSegmentProjection(segment);
            sync.placements.push_back(makePlacement(segment.contentKey, projection));
            viewEndSeconds = std::max(viewEndSeconds, projection.timelineEndSeconds());
        }

        if (!sync.placements.empty()) {
            sync.activeContentKey = chooseActiveCaptureContentKey(*session, processorRef_.getPosition());
            const bool activeBelongsToPlacements = std::any_of(sync.placements.begin(),
                                                               sync.placements.end(),
                                                               [&sync](const auto& placement) {
                                                                   return placement.contentKey == sync.activeContentKey;
                                                               });
            if (!activeBelongsToPlacements)
                sync.activeContentKey = {};

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
    session->setActiveSegmentChangedCallback([this](ContentKey contentKey) {
        AppLogger::log("VST3 Capture: completed contentKey="
            + juce::String(static_cast<juce::int64>(contentKey.objectId)));
        syncContentProjectionToPianoRoll();
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
    syncContentProjectionToPianoRoll();
    const auto activeKey = resolveCurrentContentKey();
    if (!activeKey.isValid()) return;

    std::shared_ptr<PitchCurve> curve;
#if JucePlugin_Enable_ARA
    if (auto* dc = processorRef_.getDocumentController())
        curve = dc->readPitchCurve(activeKey);
#endif

    if (!curve || !curve->getSnapshot()->hasRenderableCorrectedF0()) return;

    double startSec = 0.0;
    double endSec = pianoRoll_.getContentDurationSeconds();
    auto* editAction = dynamic_cast<OpenTune::PianoRollEditAction*>(action);
    if (editAction && editAction->getContentKey() == activeKey && editAction->getAffectedEndFrame() > 0) {
        const double spf = static_cast<double>(curve->getHopSize()) / curve->getSampleRate();
        startSec = static_cast<double>(editAction->getAffectedStartFrame()) * spf;
        endSec = static_cast<double>(editAction->getAffectedEndFrame()) * spf;
    }
    contentCommands_->enqueuePartialRender(activeKey, startSec, endSec);
}

void OpenTuneAudioProcessorEditor::redoRequested()
{
    auto* action = processorRef_.getUndoManager().redo();
    if (!action) return;
    syncContentProjectionToPianoRoll();
    const auto activeKey = resolveCurrentContentKey();
    if (!activeKey.isValid()) return;

    std::shared_ptr<PitchCurve> curve;
#if JucePlugin_Enable_ARA
    if (auto* dc = processorRef_.getDocumentController())
        curve = dc->readPitchCurve(activeKey);
#endif

    if (!curve || !curve->getSnapshot()->hasRenderableCorrectedF0()) return;

    double startSec = 0.0;
    double endSec = pianoRoll_.getContentDurationSeconds();
    auto* editAction = dynamic_cast<OpenTune::PianoRollEditAction*>(action);
    if (editAction && editAction->getContentKey() == activeKey && editAction->getAffectedEndFrame() > 0) {
        const double spf = static_cast<double>(curve->getHopSize()) / curve->getSampleRate();
        startSec = static_cast<double>(editAction->getAffectedStartFrame()) * spf;
        endSec = static_cast<double>(editAction->getAffectedEndFrame()) * spf;
    }
    contentCommands_->enqueuePartialRender(activeKey, startSec, endSec);
}

void OpenTuneAudioProcessorEditor::mouseTrailThemeChanged(MouseTrailConfig::TrailTheme theme)
{
    juce::ignoreUnused(theme);
}

void OpenTuneAudioProcessorEditor::playRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        if (!docController->requestStartPlayback())
            AppLogger::log("ARA: requestStartPlayback failed — host playback controller unavailable");
        return;
    }
#endif
    surfaceRegularVst3HostControlledTransport("play");
}

void OpenTuneAudioProcessorEditor::pauseRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        if (!docController->requestStopPlayback())
            AppLogger::log("ARA: requestStopPlayback failed — host playback controller unavailable");
        return;
    }
#endif
    surfaceRegularVst3HostControlledTransport("pause");
}

void OpenTuneAudioProcessorEditor::stopRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        bool ok = docController->requestStopPlayback();
        ok = docController->requestSetPlaybackPosition(0.0) && ok;
        if (!ok)
            AppLogger::log("ARA: stop/seek request failed — host playback controller unavailable");
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

    const auto activeKey = resolveCurrentContentKey();
    if (activeKey.isValid()) {
        DetectedKey key;
        key.root = static_cast<Key>(clampedRoot);
        key.scale = (clampedType == 2) ? Scale::Minor : ((clampedType == 3) ? Scale::Chromatic : Scale::Major);
        key.confidence = 1.0f;
        contentCommands_->setDetectedKey(activeKey, key);
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

    const auto allRegions = dc->getPlaybackRegionProjections();
    if (allRegions.empty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Read Audio",
                                               "No audio region is available on this track.");
        return;
    }

    const int refreshed = dc->refreshAllAudioModifications();
    if (refreshed == 0) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Read Audio",
                                               "Audio regions could not be processed.");
        return;
    }

    AppLogger::log("ReadAudio: refreshed " + juce::String(refreshed)
        + " AudioModification(s) from " + juce::String(static_cast<int>(allRegions.size()))
        + " playback region(s)");

    waitingForAraContent_ = true;
    araWaitStartMs_ = juce::Time::getApproximateMillisecondCounter();
    autoRenderOverlay_.setMessageText(
        juce::String::fromUTF8("\xe9\x9f\xb3\xe9\xa2\x91\xe5\xa4\x84\xe7\x90\x86\xe4\xb8\xad"),
        "Audio data is being processed. The region will appear shortly.");
    autoRenderOverlay_.setVisible(true);
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
    const auto activeKey = resolveCurrentContentKey();
    AppLogger::log("AutoTune: vst3 request contentId=" + juce::String(static_cast<juce::int64>(activeKey.objectId)));
    if (!activeKey.isValid()) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "AUTO",
            "AUTO needs an active ARA audio modification.");
        return;
    }

    OriginalF0State f0State = OriginalF0State::NotRequested;
#if JucePlugin_Enable_ARA
    if (auto* dc = processorRef_.getDocumentController())
        f0State = dc->readOriginalF0State(activeKey);
#endif

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

    const auto result = pianoRoll_.applyAutoTuneToSelection();
    AppLogger::log("AutoTune: vst3 apply result=" + juce::String(result.applied() ? "true" : "false")
        + " message=" + result.message());
    if (!result.applied()) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "AUTO",
            result.message());
        return;
    }

}

void OpenTuneAudioProcessorEditor::pitchShiftRequested()
{
    const auto activeKey = resolveCurrentContentKey();
    if (!activeKey.isValid()) return;

    PitchShiftSettings currentSettings = PitchShiftSettings::identity();
#if JucePlugin_Enable_ARA
    if (auto* dc = processorRef_.getDocumentController())
        currentSettings = dc->readPitchShift(activeKey);
    else
#endif
        currentSettings = processorRef_.getPitchShiftSettings(activeKey);

    auto* content = new OpenTune::PitchShiftDialogContent(currentSettings);

    auto commands = getContentCommandsShared();
    struct DialogHelper : public OpenTune::PitchShiftDialogContent::Listener
    {
        OpenTuneAudioProcessorEditor* owner;
        ContentKey activeContentKey;
        OpenTune::PitchShiftSettings oldSettings;
        std::shared_ptr<ContentEditCommands> commands;
        juce::Component::SafePointer<juce::Component> contentPtr;

        DialogHelper(OpenTuneAudioProcessorEditor* o, ContentKey k,
                     const OpenTune::PitchShiftSettings& s,
                     std::shared_ptr<ContentEditCommands> cmds,
                     juce::Component::SafePointer<juce::Component> c)
            : owner(o), activeContentKey(k), oldSettings(s), commands(std::move(cmds)), contentPtr(std::move(c)) {}

        void pitchShiftConfirmed(const OpenTune::PitchShiftSettings& newSettings) override
        {
            if (!owner) return;
            if (newSettings != oldSettings) {
                auto& um = owner->processorRef_.getUndoManager();
                um.addAction(std::make_unique<OpenTune::PitchShiftEditAction>(
                    commands, activeContentKey, oldSettings, newSettings));
                if (commands)
                    commands->setPitchShiftSettings(activeContentKey, newSettings);
                owner->parameterPanel_.setPitchShiftIndicator(newSettings.semitone, newSettings.cents);
            }
            closeDialog();
        }

        void pitchShiftReset() override
        {
            if (!owner) return;
            const auto identity = OpenTune::PitchShiftSettings::identity();
            if (identity != oldSettings) {
                auto& um = owner->processorRef_.getUndoManager();
                um.addAction(std::make_unique<OpenTune::PitchShiftEditAction>(
                    commands, activeContentKey, oldSettings, identity));
                if (commands)
                    commands->setPitchShiftSettings(activeContentKey, identity);
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

    auto* helper = new DialogHelper{this, activeKey, currentSettings,
                                    std::move(commands),
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
    const auto activeKey = resolveCurrentContentKey();
    if (!activeKey.isValid()) {
        AppLogger::log("InvariantViolation: pitchCurveEdited - no active content during curve edit callback");
        jassertfalse;
        return;
    }

    std::shared_ptr<PitchCurve> curve;
#if JucePlugin_Enable_ARA
    if (auto* dc = processorRef_.getDocumentController())
        curve = dc->readPitchCurve(activeKey);
#endif

    if (curve == nullptr) {
        AppLogger::log("InvariantViolation: pitchCurveEdited - content " + juce::String(static_cast<juce::int64>(activeKey.objectId)) + " has no pitch curve");
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
    contentCommands_->enqueuePartialRender(activeKey, editStartSec, editEndSec);
}

void OpenTuneAudioProcessorEditor::escapeKeyPressed()
{
    // Escape cancels selection/tool mode; not a transport command.
    // Originally called playPauseToggleRequested() here — removed.
}

void OpenTuneAudioProcessorEditor::syncContentProjectionToPianoRoll()
{
    if (!contentCommands_) {
        contentCommands_ = processorRef_.getContentCommands();
        pianoRoll_.addListener(this);
        pianoRoll_.setProcessor(&processorRef_);
        pianoRoll_.setReadContentSnapshot([this](ContentKey key) {
            return processorRef_.getContentSnapshot(key);
        });
        pianoRoll_.setContentCommands(contentCommands_);
    }

    const auto sync = resolveCurrentContentSync();

    if (!sync.hasPlacements()) {
        pianoRoll_.clearTimelineViewDomain();
        pianoRoll_.setTimelineContentPlacements({});
        pianoRoll_.setEditedContent(ContentKey{},
                                    nullptr,
                                    nullptr,
                                    static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));
        return;
    }

    std::shared_ptr<const juce::AudioBuffer<float>> syncBuffer;
    std::shared_ptr<PitchCurve> curve;
    DetectedKey detectedKey;
#if JucePlugin_Enable_ARA
    if (auto* dc = processorRef_.getDocumentController()) {
        ContentKey ck = sync.activeContentKey;
        syncBuffer = dc->readAudioBuffer(ck);
        curve = dc->readPitchCurve(ck);
        detectedKey = dc->readDetectedKey(ck);
    }
#endif

    if (!sync.hasActiveContent()
        || syncBuffer == nullptr) {
        pianoRoll_.setEditedContent(ContentKey{},
                                    nullptr,
                                    nullptr,
                                    static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));
        pianoRoll_.setTimelineContentPlacements(sync.placements);
        if (sync.timelineViewEndSeconds > sync.timelineViewStartSeconds) {
            pianoRoll_.setTimelineViewDomain(sync.timelineViewStartSeconds, sync.timelineViewEndSeconds);
        } else {
            pianoRoll_.clearTimelineViewDomain();
        }
        return;
    }

    pianoRoll_.setEditedContent(sync.activeContentKey,
                                curve,
                                syncBuffer,
                                static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));
    pianoRoll_.setTimelineContentPlacements(sync.placements);
    if (sync.timelineViewEndSeconds > sync.timelineViewStartSeconds) {
        pianoRoll_.setTimelineViewDomain(sync.timelineViewStartSeconds, sync.timelineViewEndSeconds);
    } else {
        pianoRoll_.clearTimelineViewDomain();
    }
    const int rootNote = static_cast<int>(detectedKey.root);
    const int scaleType = (detectedKey.scale == Scale::Minor) ? 2 : ((detectedKey.scale == Scale::Chromatic) ? 3 : 1);

    suppressScaleChangedCallback_ = true;
    transportBar_.setScale(rootNote, scaleType);
    suppressScaleChangedCallback_ = false;
    pianoRoll_.setScale(rootNote, scaleType);
}

} // namespace OpenTune::PluginUI

#endif // JucePlugin_Build_VST3
