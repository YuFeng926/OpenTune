#if JucePlugin_Build_VST3

#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

#include "Editor/Preferences/SharedPreferencePages.h"
#include "Editor/Preferences/TabbedPreferencesDialog.h"
#include "Plugin/Capture/CaptureSession.h"
#include "Utils/AppLogger.h"
#include "Utils/ParameterPanelSync.h"
#include "Utils/Note.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/TimeCoordinate.h"

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

struct AudioDiffRange {
    bool changed{false};
    int64_t startSample{0};
    int64_t endSampleExclusive{0};
};

float getAudioSampleOrZero(const juce::AudioBuffer<float>& buffer, int channel, int64_t sampleIndex)
{
    if (channel < 0 || channel >= buffer.getNumChannels()) {
        return 0.0f;
    }
    if (sampleIndex < 0 || sampleIndex >= static_cast<int64_t>(buffer.getNumSamples())) {
        return 0.0f;
    }

    return buffer.getSample(channel, static_cast<int>(sampleIndex));
}

bool nearlyEqualAudioSample(float a, float b)
{
    return std::abs(a - b) <= 1.0e-5f;
}

bool nearlyEqualAudioFrameAtIndices(const juce::AudioBuffer<float>& lhs,
                                    int64_t lhsSampleIndex,
                                    const juce::AudioBuffer<float>& rhs,
                                    int64_t rhsSampleIndex)
{
    const int maxChannels = std::max(lhs.getNumChannels(), rhs.getNumChannels());
    for (int channel = 0; channel < maxChannels; ++channel) {
        const float lhsSample = getAudioSampleOrZero(lhs, channel, lhsSampleIndex);
        const float rhsSample = getAudioSampleOrZero(rhs, channel, rhsSampleIndex);
        if (!nearlyEqualAudioSample(lhsSample, rhsSample)) {
            return false;
        }
    }

    return true;
}

bool nearlyEqualSeconds(double a, double b)
{
    return std::abs(a - b) <= (1.0 / TimeCoordinate::kRenderSampleRate);
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

MaterializationTimelineProjection makePianoRollLocalProjection(const VST3AraSession::PublishedRegionView& region)
{
    MaterializationTimelineProjection projection;
    projection.timelineStartSeconds = region.playbackStartSeconds;
    projection.timelineDurationSeconds = region.materializationDurationSeconds;
    projection.materializationDurationSeconds = region.materializationDurationSeconds;
    return projection;
}

AudioDiffRange detectAudioDiffRange(const juce::AudioBuffer<float>& oldBuffer,
                                    const juce::AudioBuffer<float>& newBuffer)
{
    const int64_t oldSamples = static_cast<int64_t>(oldBuffer.getNumSamples());
    const int64_t newSamples = static_cast<int64_t>(newBuffer.getNumSamples());
    const int64_t maxSamples = std::max(oldSamples, newSamples);

    AudioDiffRange out;
    if (maxSamples <= 0) {
        return out;
    }

    int64_t prefix = 0;
    while (prefix < maxSamples) {
        if (!nearlyEqualAudioFrameAtIndices(oldBuffer, prefix, newBuffer, prefix)) {
            break;
        }
        ++prefix;
    }

    if (prefix == maxSamples) {
        return out;
    }

    int64_t suffix = 0;
    while ((suffix + prefix) < maxSamples) {
        const int64_t oldIndex = oldSamples - 1 - suffix;
        const int64_t newIndex = newSamples - 1 - suffix;
        if (!nearlyEqualAudioFrameAtIndices(oldBuffer, oldIndex, newBuffer, newIndex)) {
            break;
        }
        ++suffix;
    }

    out.changed = true;
    out.startSample = prefix;
    out.endSampleExclusive = maxSamples - suffix;
    if (out.endSampleExclusive < out.startSample) {
        out.endSampleExclusive = out.startSample;
    }
    return out;
}

bool prepareImportFromAraRegion(OpenTuneAudioProcessor& processor,
                                const juce::AudioBuffer<float>& sourceBuffer,
                                double sourceSampleRate,
                                double sourceStartSeconds,
                                double sourceEndSeconds,
                                const juce::String& displayName,
                                OpenTuneAudioProcessor::PreparedImport& outPreparedImport)
{
    if (sourceSampleRate <= 0.0 || sourceBuffer.getNumSamples() <= 0 || sourceBuffer.getNumChannels() <= 0) {
        return false;
    }

    if (!(sourceEndSeconds > sourceStartSeconds)) {
        return false;
    }

    const int64_t sourceStartSample = juce::jmax<int64_t>(0,
        TimeCoordinate::secondsToSamples(sourceStartSeconds, sourceSampleRate));
    const int64_t sourceEndSample = juce::jmax<int64_t>(sourceStartSample,
        TimeCoordinate::secondsToSamples(sourceEndSeconds, sourceSampleRate));

    const int64_t sourceLengthSamples = static_cast<int64_t>(sourceBuffer.getNumSamples());
    if (sourceStartSample >= sourceLengthSamples) {
        return false;
    }

    const int64_t clampedEndSample = juce::jmin<int64_t>(sourceEndSample, sourceLengthSamples);
    const int64_t regionSamples = clampedEndSample - sourceStartSample;
    if (regionSamples <= 0) {
        return false;
    }

    juce::AudioBuffer<float> sliced(static_cast<int>(sourceBuffer.getNumChannels()),
                                    static_cast<int>(regionSamples));
    for (int ch = 0; ch < sliced.getNumChannels(); ++ch) {
        sliced.copyFrom(ch,
                        0,
                        sourceBuffer,
                        ch,
                        static_cast<int>(sourceStartSample),
                        static_cast<int>(regionSamples));
    }

    if (!processor.prepareImport(std::move(sliced),
                                 sourceSampleRate,
                                 displayName,
                                 outPreparedImport)) {
        return false;
    }

    outPreparedImport.sourceWindow.sourceId = 0; // sourceId filled at commit time
    outPreparedImport.sourceWindow.sourceStartSeconds = sourceStartSeconds;
    outPreparedImport.sourceWindow.sourceEndSeconds = sourceEndSeconds;
    return true;
}

#if JucePlugin_Enable_ARA
const VST3AraSession::PublishedRegionView* resolvePreferredAraRegionView(
    const VST3AraSession::PublishedSnapshot& snapshot)
{
    if (const auto* preferredRegionView = snapshot.findPreferredRegion())
        return preferredRegionView;

    if (snapshot.publishedRegions.size() == 1)
        return &snapshot.publishedRegions.front();

    return nullptr;
}
#endif

} // namespace

OpenTuneAudioProcessorEditor::OpenTuneAudioProcessorEditor(OpenTuneAudioProcessor& processor)
    : AudioProcessorEditor(&processor)
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

    // 在首次推理服务初始化前，将持久化的渲染优先级应用到检测器
    if (appPreferences_.getState().shared.renderingPriority == RenderingPriority::CpuFirst) {
        processorRef_.resetInferenceBackend(true);
    }

    syncMaterializationProjectionToPianoRoll();
    syncSharedAppPreferences();

    // Non-ARA VST3 host-mirror mode: PianoRoll display is driven entirely by host_t
    // via timerCallback → syncMaterializationProjectionToPianoRoll(). No active-segment
    // callback is registered: when the host playhead enters a segment's range, the
    // timer-tick fallback picks it up; when it leaves, PianoRoll goes blank.

    startTimerHz(kHeartbeatHz);
}

OpenTuneAudioProcessorEditor::~OpenTuneAudioProcessorEditor()
{
    stopTimer();
    LocalizationManager::getInstance().removeListener(this);
    pianoRoll_.removeListener(this);
    parameterPanel_.removeListener(this);
    transportBar_.removeListener(this);
    menuBar_.removeListener(this);
    setLookAndFeel(nullptr);
}

void OpenTuneAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(UIColors::backgroundDark);
}

void OpenTuneAudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
    // 确保点击 Editor 背景时焦点转到 PianoRoll（按键可达）
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
    context.hasSelectedSegmentRetuneSpeed = pianoRoll_.getSelectedSegmentRetuneSpeed(
        context.selectedSegmentRetuneSpeedPercent);

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
    // 首次 timer 回调时确保 PianoRoll 获取焦点（VST3 嵌入时序可能导致 visibilityChanged 中的 grab 失败）
    if (!initialFocusGrabbed_ && isShowing()) {
        initialFocusGrabbed_ = true;
        pianoRoll_.grabKeyboardFocus();
    }

    syncSharedAppPreferences();

    // Drive capture session tick (poll Capturing→Pending and Processing→Edited transitions,
    // run reclaim sweep). No-op when capture session is null (Standalone / VST3+ARA).
    if (auto* session = processorRef_.getCaptureSession()) {
        session->tick();
        // Reflect "currently recording" as a sticky toggle on the record button so the
        // user gets explicit visual feedback that capture is in progress.
        const bool isCapturingNow =
            session->getGlobalState() == OpenTune::Capture::SessionState::HasCapturing;
        transportBar_.setRecordIndicatorActive(isCapturingNow);
    }

    const double currentPositionSeconds = processorRef_.getPosition();
    const bool playing = processorRef_.isPlaying();
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
    }

    if (rmvpeOverlayLatched_) {
        const uint64_t targetMaterializationId = rmvpeOverlayTargetMaterializationId_;

        bool shouldUnlatch = false;
        if (targetMaterializationId == 0) {
            shouldUnlatch = true;
        } else {
            const auto f0State = processorRef_.getMaterializationOriginalF0StateById(targetMaterializationId);
            if (f0State == OriginalF0State::Ready || f0State == OriginalF0State::Failed) {
                shouldUnlatch = true;
            }
        }

        if (shouldUnlatch) {
            rmvpeOverlayLatched_ = false;
            rmvpeOverlayTargetMaterializationId_ = 0;
        }
    }

    // Drive PianoRoll heartbeat first so autoTuneInFlight_ is up-to-date
    if (pianoRoll_.isShowing()) {
        pianoRoll_.onHeartbeatTick();
    }

    bool shouldShowOverlay = false;

    const uint64_t activeMaterializationId = resolveCurrentMaterializationId();
    const auto chunkStats = processorRef_.getMaterializationChunkStatsById(activeMaterializationId);
    const bool isAutoProcessing = pianoRoll_.isAutoTuneProcessing();
    const bool hasActiveRender = chunkStats.hasActiveWork();

    if (isAutoProcessing) {
        const int total = chunkStats.total();
        const int done = chunkStats.idle + chunkStats.blank;
        const float progress = (total > 0) ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
        autoRenderOverlay_.setMessageText(buildRenderingOverlayTitle(done, total, progress));
        shouldShowOverlay = true;
    }

    if (rmvpeOverlayLatched_) {
        autoRenderOverlay_.setMessageText(juce::String::fromUTF8("正在分析音高"));
        shouldShowOverlay = true;
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

    // Unified materialization → PianoRoll sync (projection + curve + buffer + scale)
    syncMaterializationProjectionToPianoRoll();

    syncImportedAraClipIfNeeded();
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
    uint64_t materializationId = 0;
    MaterializationTimelineProjection projection;
    resolveCurrentMaterializationProjection(materializationId, projection);
    return materializationId;
}

OpenTuneAudioProcessorEditor::MaterializationSource
OpenTuneAudioProcessorEditor::resolveCurrentMaterializationProjection(uint64_t& materializationId,
                                                                     MaterializationTimelineProjection& projection)
{
    materializationId = 0;
    projection = {};

#if JucePlugin_Enable_ARA
    if (const auto* dc = processorRef_.getDocumentController()) {
        if (const auto* session = dc->getSession()) {
            const auto snapshot = session->loadSnapshot();
            if (snapshot != nullptr) {
                if (const auto* preferredRegion = resolvePreferredAraRegionView(*snapshot)) {
                    materializationId = preferredRegion->appliedProjection.materializationId;
                    projection = makePianoRollLocalProjection(*preferredRegion);
                    if (materializationId != 0)
                        return MaterializationSource::Ara;
                }
            }
        }
    }
#endif

    // Non-ARA VST3 fallback: pick the Edited capture segment whose host time range
    // contains the current host playhead. Used by AUTO / pen-tool / scale / any other
    // code path that goes through resolveCurrentMaterializationId so they can find
    // the active take instead of returning 0.
    if (auto* session = processorRef_.getCaptureSession()) {
        const double host_t = processorRef_.getPosition();
        const auto segments = session->listSegments();
        for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
            if (it->state != OpenTune::Capture::SegmentState::Edited
                || it->materializationId == 0)
                continue;
            const double segStart = it->T_start;
            const double segEnd = segStart + it->durationSeconds;
            if (host_t >= segStart && host_t < segEnd) {
                materializationId = it->materializationId;
                projection.timelineStartSeconds = segStart;
                projection.timelineDurationSeconds = it->durationSeconds;
                projection.materializationDurationSeconds = it->durationSeconds;
                return MaterializationSource::Capture;
            }
        }
    }

    return MaterializationSource::None;
}

bool OpenTuneAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey) {
        playPauseToggleRequested();
        return true;
    }

    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'z') {
        undoRequested();
        return true;
    }

    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'y') {
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
    if (toolId < 0 || toolId > static_cast<int>(ToolId::HandDraw)) {
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

void OpenTuneAudioProcessorEditor::savePresetRequested()
{
    showHostManagedMessage("Save Preset",
                           "Use your DAW preset/chunk save workflow to persist plugin state.");
}

void OpenTuneAudioProcessorEditor::loadPresetRequested()
{
    showHostManagedMessage("Load Preset",
                           "Use your DAW preset/chunk load workflow to restore plugin state.");
}

void OpenTuneAudioProcessorEditor::preferencesRequested()
{
    showPreferencesDialog();
}

void OpenTuneAudioProcessorEditor::showPreferencesDialog()
{
    auto pages = SharedPreferencePages::create(appPreferences_, [this] { syncSharedAppPreferences(); });

    // Insert Audio page (with rendering priority) at the beginning
    auto audioPage = SharedPreferencePages::createRenderingPriorityComponent(
        appPreferences_, [this] { syncSharedAppPreferences(); },
        [this](bool forceCpu) { processorRef_.resetInferenceBackend(forceCpu); });
    pages.insert(pages.begin(), { LOC(kAudio), std::move(audioPage) });

    auto* dialogContent = new TabbedPreferencesDialog(std::move(pages));
    dialogContent->setSize(560, 420);

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
        processorRef_.setPlayingStateOnly(true);
        transportBar_.setPlaying(true);
        pianoRoll_.setIsPlaying(true);
        return;
    }
#endif
    processorRef_.setPlaying(true);
    transportBar_.setPlaying(true);
    pianoRoll_.setIsPlaying(true);
}

void OpenTuneAudioProcessorEditor::pauseRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        docController->requestStopPlayback();
        processorRef_.setPlayingStateOnly(false);
        transportBar_.setPlaying(false);
        pianoRoll_.setIsPlaying(false);
        return;
    }
#endif
    processorRef_.setPlaying(false);
    transportBar_.setPlaying(false);
    pianoRoll_.setIsPlaying(false);
}

void OpenTuneAudioProcessorEditor::stopRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        docController->requestStopPlayback();
        docController->requestSetPlaybackPosition(0.0);
        processorRef_.setPosition(0.0);
        transportBar_.setPlaying(false);
        pianoRoll_.setIsPlaying(false);
        return;
    }
#endif
    processorRef_.setPlaying(false);
    processorRef_.setPosition(0.0);
    transportBar_.setPlaying(false);
    pianoRoll_.setIsPlaying(false);
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
#if !JucePlugin_Enable_ARA
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                           "Read Audio",
                                           "ARA is not available in current build.");
    return;
#else
    araClipImportArmed_ = true;
    auto* dc = processorRef_.getDocumentController();
    if (dc == nullptr) {
        // Non-ARA VST3 context: dispatch to capture session (Melodyne Transfer-style flow)
        // instead of failing with an alert. Empty session → arm a new capture; capturing →
        // stop and submit; processing → ignore (button is disabled in tooltip update).
        if (auto* session = processorRef_.getCaptureSession()) {
            using OpenTune::Capture::SessionState;
            switch (session->getGlobalState()) {
                case SessionState::Idle:
                    session->armNewCapture();
                    AppLogger::log("VST3 Capture: armNewCapture (non-ARA path)");
                    break;
                case SessionState::HasCapturing:
                    session->stopCapture();
                    AppLogger::log("VST3 Capture: stopCapture (non-ARA path)");
                    break;
                case SessionState::HasProcessing:
                    AppLogger::log("VST3 Capture: ignored (Processing — wait for render)");
                    break;
            }
            return;
        }
        // True fallback: no capture session and no ARA — show original error.
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Read Audio",
                                               "Unable to access VST3 ARA DocumentController.");
        return;
    }
    auto* session = dc->getSession();
    if (session == nullptr) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Read Audio",
                                               "Unable to access VST3 ARA session.");
        return;
    }

    const auto snapshot = session->loadSnapshot();
    if (!snapshot) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Read Audio",
                                               "ARA snapshot is unavailable.");
        return;
    }

    const auto* preferredRegionView = resolvePreferredAraRegionView(*snapshot);
    if (preferredRegionView == nullptr || preferredRegionView->regionIdentity.audioSource == nullptr) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Read Audio",
                                               "No preferred ARA playback region is available. Select the target item in the DAW, then invoke Read Audio again.");
        return;
    }

    if (preferredRegionView->copiedAudio == nullptr || preferredRegionView->numSamples <= 0) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Read Audio",
                                               "Audio data is not yet available. Please wait for source hydration to complete.");
        return;
    }

    // Birth: ensure materialization exists for this region
    uint64_t materializationId = preferredRegionView->appliedProjection.materializationId;
    if (materializationId == 0)
    {
        const auto birthResult = processorRef_.ensureAraRegionMaterialization(
            preferredRegionView->regionIdentity.audioSource,
            preferredRegionView->sourceId,
            preferredRegionView->copiedAudio,
            preferredRegionView->sampleRate,
            preferredRegionView->sourceWindow,
            preferredRegionView->playbackStartSeconds);

        if (!birthResult.has_value() || birthResult->materializationId == 0) {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Read Audio",
                                                   "Failed to create materialization from ARA audio.");
            return;
        }

        materializationId = birthResult->materializationId;

        session->bindPlaybackRegionToMaterialization(
            preferredRegionView->regionIdentity.playbackRegion,
            materializationId,
            birthResult->materializationRevision,
            preferredRegionView->projectionRevision,
            preferredRegionView->sourceWindow,
            birthResult->materializationDurationSeconds,
            preferredRegionView->playbackStartSeconds);
    }

    // Refresh: trigger F0 extraction
    syncMaterializationProjectionToPianoRoll();

    OpenTuneAudioProcessor::MaterializationRefreshRequest refreshRequest;
    refreshRequest.materializationId = materializationId;
    if (!processorRef_.requestMaterializationRefresh(refreshRequest)) {
        AppLogger::log("RecordTrace: VST3 refresh rejected materializationId="
            + juce::String(static_cast<juce::int64>(materializationId)));
    } else {
        rmvpeOverlayLatched_ = true;
        rmvpeOverlayTargetMaterializationId_ = materializationId;
    }

    AppLogger::log("RecordTrace: VST3 recordRequested materializationId="
        + juce::String(static_cast<juce::int64>(materializationId)));
#endif
}

void OpenTuneAudioProcessorEditor::syncImportedAraClipIfNeeded()
{
#if !JucePlugin_Enable_ARA
    return;
#else
    auto* dc = processorRef_.getDocumentController();
    if (dc == nullptr)
        return;
    auto* session = dc->getSession();
    if (session == nullptr)
        return;

    const auto snapshot = session->loadSnapshot();
    if (!snapshot)
        return;

    const auto* preferredRegionView = resolvePreferredAraRegionView(*snapshot);
    auto markSnapshotConsumed = [&]()
    {
        lastConsumedAraSnapshotEpoch_ = snapshot->epoch;
        lastConsumedPreferredAraRegion_ = preferredRegionView != nullptr
            ? preferredRegionView->regionIdentity
            : VST3AraSession::RegionIdentity{};
    };

    if (preferredRegionView == nullptr || preferredRegionView->regionIdentity.audioSource == nullptr)
    {
        syncMaterializationProjectionToPianoRoll();
        markSnapshotConsumed();
        return;
    }

    auto* currentPlaybackRegion = preferredRegionView->regionIdentity.playbackRegion;
    const auto appliedProjection = preferredRegionView->appliedProjection;
    const bool snapshotAdvanced = snapshot->epoch != lastConsumedAraSnapshotEpoch_;
    const bool preferredRegionChanged = preferredRegionView->regionIdentity != lastConsumedPreferredAraRegion_;
    const bool appliedRegionChanged = appliedProjection.appliedRegionIdentity != preferredRegionView->regionIdentity;

    if (!appliedProjection.isValid() || appliedProjection.materializationId == 0)
    {
        syncMaterializationProjectionToPianoRoll();
        markSnapshotConsumed();
        return;
    }

    const uint64_t materializationId = appliedProjection.materializationId;
    const uint64_t materializationRevision = preferredRegionView->materializationRevision;
    const uint64_t projectionRevision = preferredRegionView->projectionRevision;
    const bool materializationChanged = materializationRevision > appliedProjection.appliedMaterializationRevision;
    const bool projectionChanged = projectionRevision > appliedProjection.appliedProjectionRevision;
    if (!snapshotAdvanced && !preferredRegionChanged && !materializationChanged && !projectionChanged && !appliedRegionChanged)
        return;

    if (preferredRegionView->copiedAudio == nullptr || preferredRegionView->numSamples <= 0)
        return;

    const double playbackStartSeconds = preferredRegionView->playbackStartSeconds;
    const SourceWindow& sourceWindow = preferredRegionView->sourceWindow;

    if (processorRef_.getMaterializationAudioBufferById(materializationId) == nullptr)
    {
        session->clearPlaybackRegionMaterialization(currentPlaybackRegion);
        syncMaterializationProjectionToPianoRoll();
        markSnapshotConsumed();
        return;
    }

    const bool playbackStartChanged = !nearlyEqualSeconds(appliedProjection.playbackStartSeconds, playbackStartSeconds);
    const bool sourceRangeChanged = !nearlyEqualSeconds(appliedProjection.appliedSourceWindow.sourceStartSeconds, sourceWindow.sourceStartSeconds)
        || !nearlyEqualSeconds(appliedProjection.appliedSourceWindow.sourceEndSeconds, sourceWindow.sourceEndSeconds);

    if (!materializationChanged && !projectionChanged && !sourceRangeChanged && !playbackStartChanged && appliedRegionChanged)
    {
        session->bindPlaybackRegionToMaterialization(
            currentPlaybackRegion,
            materializationId,
            materializationRevision,
            projectionRevision,
            sourceWindow,
            processorRef_.getMaterializationAudioDurationById(materializationId),
            playbackStartSeconds);
        markSnapshotConsumed();
        return;
    }

    if (!materializationChanged && !projectionChanged && !sourceRangeChanged && !playbackStartChanged)
    {
        markSnapshotConsumed();
        return;
    }

    if (!materializationChanged && !sourceRangeChanged && playbackStartChanged)
    {
        // STAB-01 diagnostic: mapping-only change, skip content re-rendering
        AppLogger::log("MappingTrace: materializationId=" + juce::String(static_cast<juce::int64>(materializationId))
            + " mappingOnly=true"
            + " playbackStart=" + juce::String(playbackStartSeconds, 6)
            + " materializationReRender=false");

        session->bindPlaybackRegionToMaterialization(
            currentPlaybackRegion, materializationId,
            materializationRevision, projectionRevision,
            sourceWindow, processorRef_.getMaterializationAudioDurationById(materializationId), playbackStartSeconds
        );
        markSnapshotConsumed();
        syncMaterializationProjectionToPianoRoll();
        return;
    }

    OpenTuneAudioProcessor::PreparedImport preparedImport;
    if (!prepareImportFromAraRegion(processorRef_,
                                    *preferredRegionView->copiedAudio,
                                    preferredRegionView->sampleRate,
                                    sourceWindow.sourceStartSeconds,
                                    sourceWindow.sourceEndSeconds,
                                    "ARA Audio",
                                    preparedImport)) {
        AppLogger::log("InvariantViolation: syncImportedAraClipIfNeeded - prepareImportFromAraRegion failed for active region");
        jassertfalse;
        return;
    }

    auto oldBuffer = processorRef_.getMaterializationAudioBufferById(materializationId);
    if (oldBuffer == nullptr) {
        AppLogger::log("InvariantViolation: syncImportedAraClipIfNeeded - materialization " + juce::String(static_cast<juce::int64>(materializationId)) + " has no audio buffer");
        jassertfalse;
        return;
    }

    const AudioDiffRange diff = detectAudioDiffRange(*oldBuffer, preparedImport.storedAudioBuffer);
    if (!diff.changed && !sourceRangeChanged && playbackStartChanged)
    {
        session->bindPlaybackRegionToMaterialization(
            currentPlaybackRegion, materializationId,
            materializationRevision, projectionRevision,
            sourceWindow, processorRef_.getMaterializationAudioDurationById(materializationId), playbackStartSeconds
        );
        markSnapshotConsumed();
        syncMaterializationProjectionToPianoRoll();
        return;
    }

    if (!diff.changed && !sourceRangeChanged && !playbackStartChanged)
    {
        if (appliedRegionChanged)
        {
            session->bindPlaybackRegionToMaterialization(
                currentPlaybackRegion,
                materializationId,
                materializationRevision,
                projectionRevision,
                sourceWindow,
                processorRef_.getMaterializationAudioDurationById(materializationId),
                playbackStartSeconds);
        }
        else
        {
            session->updatePlaybackRegionMaterializationRevisions(currentPlaybackRegion,
                                                                  materializationRevision,
                                                                  projectionRevision);
        }

        markSnapshotConsumed();
        return;
    }

    auto updatedBuffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(preparedImport.storedAudioBuffer));

    uint64_t activeMaterializationId = materializationId;

    if (sourceRangeChanged)
    {
        // lineage 变了（source range 改变）：原子新建 + erase 旧，调用方（此处）负责 bind 新 id
        MaterializationStore::CreateMaterializationRequest newRequest;
        newRequest.sourceId = appliedProjection.sourceId;
        newRequest.lineageParentMaterializationId = materializationId;
        newRequest.sourceWindow = sourceWindow;
        newRequest.audioBuffer = updatedBuffer;
        newRequest.silentGaps = std::move(preparedImport.silentGaps);
        newRequest.renderCache = std::make_shared<RenderCache>();
        activeMaterializationId = processorRef_.replaceMaterializationWithNewLineage(materializationId, std::move(newRequest));
        if (activeMaterializationId == 0)
            return;
    }
    else
    {
        // 同 lineage 重渲染（audio 内容变，source range 不变）：sourceWindow 不动
        if (!processorRef_.replaceMaterializationAudioById(materializationId,
                                                           updatedBuffer,
                                                           std::move(preparedImport.silentGaps)))
            return;
    }

    double changedStartSeconds = 0.0;
    double changedEndSeconds = TimeCoordinate::samplesToSeconds(updatedBuffer->getNumSamples(), TimeCoordinate::kRenderSampleRate);
    if (diff.changed)
    {
        changedStartSeconds = TimeCoordinate::samplesToSeconds(diff.startSample, TimeCoordinate::kRenderSampleRate);
        changedEndSeconds = TimeCoordinate::samplesToSeconds(diff.endSampleExclusive, TimeCoordinate::kRenderSampleRate);
    }

    // Invalidate affected chunks via partial render (STAB-01: partial invalidation, not full cache rebuild)
    // Only invalidate if there's actual content change (diff.changed or sourceRangeChanged)
    if (diff.changed || sourceRangeChanged)
    {
        processorRef_.enqueueMaterializationPartialRenderById(activeMaterializationId,
                                               changedStartSeconds,
                                               changedEndSeconds);
    }

    if (processorRef_.getMaterializationAudioBufferById(activeMaterializationId) != nullptr)
    {
        OpenTuneAudioProcessor::MaterializationRefreshRequest refreshRequest;
        refreshRequest.materializationId = activeMaterializationId;
        refreshRequest.preserveCorrectionsOutsideChangedRange = true;
        refreshRequest.changedStartSeconds = changedStartSeconds;
        refreshRequest.changedEndSeconds = changedEndSeconds;
        if (!processorRef_.requestMaterializationRefresh(refreshRequest)) {
            AppLogger::log("ClipDerivedRefresh: VST3 sync request rejected materializationId="
                + juce::String(static_cast<juce::int64>(activeMaterializationId)));
        } else {
            rmvpeOverlayLatched_ = true;
            rmvpeOverlayTargetMaterializationId_ = activeMaterializationId;
        }
        syncMaterializationProjectionToPianoRoll();
    }

        session->bindPlaybackRegionToMaterialization(
            currentPlaybackRegion, activeMaterializationId,
            materializationRevision, projectionRevision,
            sourceWindow, processorRef_.getMaterializationAudioDurationById(activeMaterializationId), playbackStartSeconds
        );
    markSnapshotConsumed();
#endif
}

void OpenTuneAudioProcessorEditor::playheadPositionChangeRequested(double timeSeconds)
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        docController->requestSetPlaybackPosition(timeSeconds);
        processorRef_.setPosition(timeSeconds);
        return;
    }
#endif
    processorRef_.setPosition(timeSeconds);
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
    uint64_t materializationId = 0;
    MaterializationTimelineProjection projection;

    const MaterializationSource source =
        resolveCurrentMaterializationProjection(materializationId, projection);

    // ARA path requires explicit user arm (Read Audio click) before we attach the
    // detected materialization to PianoRoll. Capture path bypasses this — each
    // Edited segment IS the user's explicit capture, no separate arm needed.
    if (source == MaterializationSource::Ara && !araClipImportArmed_) {
        pianoRoll_.setMaterializationProjection({});
        pianoRoll_.setEditedMaterialization(0, nullptr, nullptr,
            static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));
        return;
    }

    if (materializationId == 0) {
        pianoRoll_.setMaterializationProjection({});
        pianoRoll_.setEditedMaterialization(0,
                                    nullptr,
                                    nullptr,
                                    static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));
        return;
    }

    if (processorRef_.getMaterializationAudioBufferById(materializationId) == nullptr) {
        pianoRoll_.setMaterializationProjection({});
        pianoRoll_.setEditedMaterialization(0,
                                    nullptr,
                                    nullptr,
                                    static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));
        return;
    }

    pianoRoll_.setMaterializationProjection(projection);

    auto curve = processorRef_.getMaterializationPitchCurveById(materializationId);
    auto buffer = processorRef_.getMaterializationAudioBufferById(materializationId);

    pianoRoll_.setEditedMaterialization(materializationId,
                                curve,
                                buffer,
                                static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));

    const auto key = processorRef_.getMaterializationDetectedKeyById(materializationId);
    const int rootNote = static_cast<int>(key.root);
    const int scaleType = (key.scale == Scale::Minor) ? 2 : ((key.scale == Scale::Chromatic) ? 3 : 1);

    suppressScaleChangedCallback_ = true;
    transportBar_.setScale(rootNote, scaleType);
    suppressScaleChangedCallback_ = false;
    pianoRoll_.setScale(rootNote, scaleType);
}

} // namespace OpenTune::PluginUI

#endif // JucePlugin_Build_VST3
