#include "PluginEditor.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "UI/UIColors.h"
#include "UI/FrameScheduler.h"
#include "Editor/Preferences/SharedPreferencePages.h"
#include "Editor/Preferences/StandalonePreferencePages.h"
#include "Editor/Preferences/TabbedPreferencesDialog.h"
#include "Audio/AudioFormatRegistry.h"
#include "Audio/AsyncAudioLoader.h"
#include "Utils/PresetManager.h"
#include "Utils/PitchCurve.h"
#include "Utils/NoteGenerator.h"
#include "Utils/PitchControlConfig.h"
#include "Utils/AppLogger.h"
#include "Utils/ParameterPanelSync.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/TimeCoordinate.h"
#include "Utils/KeyShortcutConfig.h"
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <set>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <future>
#include <chrono>

namespace OpenTune {

namespace {

constexpr int kHeartbeatHzIdle = 30;
constexpr int kHeartbeatHzInferenceActive = 10;

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

int getStandaloneActiveTrack(OpenTuneAudioProcessor& processor)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->getActiveTrackId();
}

bool setStandaloneActiveTrack(OpenTuneAudioProcessor& processor, int trackId)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->setActiveTrack(trackId);
}

int getStandaloneSelectedPlacementIndex(OpenTuneAudioProcessor& processor, int trackId)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->getSelectedPlacementIndex(trackId);
}

int getStandalonePlacementCount(OpenTuneAudioProcessor& processor, int trackId)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->getNumPlacements(trackId);
}

bool getStandaloneTrackMuted(OpenTuneAudioProcessor& processor, int trackId)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->isTrackMuted(trackId);
}

bool getStandaloneTrackSolo(OpenTuneAudioProcessor& processor, int trackId)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->isTrackSolo(trackId);
}

float getStandaloneTrackVolume(OpenTuneAudioProcessor& processor, int trackId)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->getTrackVolume(trackId);
}

float getStandaloneTrackRms(OpenTuneAudioProcessor& processor, int trackId)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->getTrackRmsDb(trackId);
}

void setStandaloneTrackMuted(OpenTuneAudioProcessor& processor, int trackId, bool muted)
{
    if (auto* arrangement = processor.getStandaloneArrangement()) {
        arrangement->setTrackMuted(trackId, muted);
    }
}

void setStandaloneTrackSolo(OpenTuneAudioProcessor& processor, int trackId, bool solo)
{
    if (auto* arrangement = processor.getStandaloneArrangement()) {
        arrangement->setTrackSolo(trackId, solo);
    }
}

void setStandaloneTrackVolume(OpenTuneAudioProcessor& processor, int trackId, float volume)
{
    if (auto* arrangement = processor.getStandaloneArrangement()) {
        arrangement->setTrackVolume(trackId, volume);
    }
}

void setStandaloneSelectedPlacementIndex(OpenTuneAudioProcessor& processor, int trackId, int placementIndex)
{
    if (auto* arrangement = processor.getStandaloneArrangement()) {
        arrangement->setSelectedPlacementIndex(trackId, placementIndex);
    }
}

MaterializationTimelineProjection makePianoRollProjection(const StandaloneArrangement::Placement& placement,
                                                         OpenTuneAudioProcessor& processor)
{
    MaterializationTimelineProjection projection;
    projection.timelineStartSeconds = placement.timelineStartSeconds;
    projection.timelineDurationSeconds = placement.durationSeconds;
    projection.materializationDurationSeconds =
        processor.getMaterializationAudioDurationById(placement.materializationId);
    return projection;
}

bool getStandalonePlacementByIndex(OpenTuneAudioProcessor& processor,
                                   int trackId,
                                   int placementIndex,
                                   StandaloneArrangement::Placement& out)
{
    return processor.getPlacementByIndex(trackId, placementIndex, out);
}

uint64_t getStandaloneMaterializationId(OpenTuneAudioProcessor& processor,
                                        int trackId,
                                        int placementIndex)
{
    StandaloneArrangement::Placement placement;
    return getStandalonePlacementByIndex(processor, trackId, placementIndex, placement)
        ? placement.materializationId
        : 0;
}

static juce::String getImportWildcardFilter()
{
    return AudioFormatRegistry::getImportWildcardFilter();
}

static juce::String getImportExtensionSpec()
{
    const auto wildcard = getImportWildcardFilter();
    juce::StringArray tokens;
    tokens.addTokens(wildcard, ";", "\"");

    juce::StringArray extensions;
    for (auto token : tokens)
    {
        token = token.trim();
        if (token.startsWith("*."))
            token = token.fromFirstOccurrenceOf("*.", false, false);
        token = token.toLowerCase();
        if (token.isNotEmpty())
            extensions.addIfNotAlreadyThere(token);
    }

    return extensions.joinIntoString(";");
}


juce::String renderStatusToString(RenderStatus status)
{
    switch (status) {
        case RenderStatus::Idle: return "idle";
        case RenderStatus::Rendering: return "rendering";
        case RenderStatus::Ready: return "ready";
    }

    return "unknown";
}

} // namespace

#if JUCE_DEBUG
static bool runDebugSelfTests() {
    if (!ArrangementViewComponent::runDebugSelfTest()) {
        return false;
    }

    {
        PitchCurve curve;
        curve.setHopSize(160);
        curve.setSampleRate(16000);
        std::vector<float> f0(200, 440.0f);
        std::vector<float> energy(200, 1.0f);
        curve.setOriginalF0(f0);
        curve.setOriginalEnergy(energy);
        constexpr int kHopSize = 160;
        constexpr double kF0SampleRate = 16000.0;
        constexpr double kHostSampleRate = 96000.0;
        NoteGeneratorParams params;
        params.policy.transitionThresholdCents = 512.0f;
        params.policy.minDurationMs = 100.0f;
        auto notes = NoteGenerator::generate(f0, energy, kHopSize, kF0SampleRate, kHostSampleRate, params);
        if (notes.empty()) {
            return false;
        }
        // NoteGenerator extends the tail by whole-frame steps derived from tailExtendMs.
        const double hopSecs = 160.0 / 16000.0;
        const double baseEndSeconds = static_cast<double>(f0.size()) * hopSecs;
        const double tailExtendSeconds = (std::ceil(params.policy.tailExtendMs / 1000.0 / hopSecs)) * hopSecs;
        const double expectedEndSeconds = baseEndSeconds + tailExtendSeconds;
        if (std::abs(notes[0].endTime - expectedEndSeconds) > 480.0 / 44100.0) {
            return false;
        }
    }

    return true;
}
#endif

int OpenTuneAudioProcessorEditor::scaleToUiScaleType(Scale scale)
{
    switch (scale) {
        case Scale::Major:          return 1;
        case Scale::Minor:          return 2;
        case Scale::Chromatic:      return 3;
        case Scale::HarmonicMinor:  return 4;
        case Scale::Dorian:         return 5;
        case Scale::Mixolydian:     return 6;
        case Scale::PentatonicMajor:return 7;
        case Scale::PentatonicMinor:return 8;
        default:                    return 1;
    }
}

Scale OpenTuneAudioProcessorEditor::uiScaleTypeToScale(int scaleType)
{
    switch (scaleType) {
        case 1: return Scale::Major;
        case 2: return Scale::Minor;
        case 3: return Scale::Chromatic;
        case 4: return Scale::HarmonicMinor;
        case 5: return Scale::Dorian;
        case 6: return Scale::Mixolydian;
        case 7: return Scale::PentatonicMajor;
        case 8: return Scale::PentatonicMinor;
        default: return Scale::Major;
    }
}

DetectedKey OpenTuneAudioProcessorEditor::makeDetectedKeyFromUi(int rootNote, int scaleType, float confidence)
{
    DetectedKey key;
    key.root = static_cast<Key>(juce::jlimit(0, 11, rootNote));
    key.scale = uiScaleTypeToScale(scaleType);
    key.confidence = confidence;
    return key;
}

DetectedKey OpenTuneAudioProcessorEditor::resolveScaleForPlacementMaterialization(int trackId,
                                                                                  int placementIndex,
                                                                                  juce::String* sourceOut) const
{
    const auto defaultKey = []() {
        DetectedKey k;
        k.root = Key::C;
        k.scale = Scale::Major;
        k.confidence = 1.0f;
        return k;
    };

    const bool hasPlacement = (trackId >= 0
                            && trackId < OpenTuneAudioProcessor::MAX_TRACKS
                            && placementIndex >= 0
                            && placementIndex < getStandalonePlacementCount(processorRef_, trackId));

    const uint64_t materializationId = hasPlacement ? getStandaloneMaterializationId(processorRef_, trackId, placementIndex) : 0;
    if (materializationId != 0) {
        const DetectedKey materializationKey = processorRef_.getMaterializationDetectedKeyById(materializationId);
        if (materializationKey.confidence > 0.0f) {
            if (sourceOut) *sourceOut = "materialization";
            return materializationKey;
        }
    }

    if (sourceOut) *sourceOut = "default";
    return defaultKey();
}

void OpenTuneAudioProcessorEditor::applyScaleToUi(int rootNote, int scaleType)
{
    const int clampedRoot = juce::jlimit(0, 11, rootNote);
    const int clampedType = juce::jlimit(1, 8, scaleType);

    suppressScaleChangedCallback_ = true;
    transportBar_.setScale(clampedRoot, clampedType);
    suppressScaleChangedCallback_ = false;

    pianoRoll_.setScale(clampedRoot, clampedType);
    lastScaleRootNote_ = clampedRoot;
    lastScaleType_ = clampedType;
}

void OpenTuneAudioProcessorEditor::applyResolvedScaleForPlacementMaterialization(int trackId, int placementIndex)
{
    juce::String source;
    const DetectedKey key = resolveScaleForPlacementMaterialization(trackId, placementIndex, &source);
    const int rootNote = static_cast<int>(key.root);
    const int scaleType = scaleToUiScaleType(key.scale);
    applyScaleToUi(rootNote, scaleType);

    const uint64_t materializationId = (trackId >= 0 && placementIndex >= 0)
        ? getStandaloneMaterializationId(processorRef_, trackId, placementIndex)
        : 0;
    juce::ignoreUnused(materializationId);
    DBG("ScaleSyncTrace: source=" + source
        + " trackId=" + juce::String(trackId)
        + " placementIndex=" + juce::String(placementIndex)
        + " materializationId=" + juce::String(static_cast<juce::int64>(materializationId))
        + " root=" + juce::String(rootNote)
        + " scale=" + juce::String(scaleType));
}

void OpenTuneAudioProcessorEditor::setInferenceActive(bool active)
{
    if (inferenceActive_ == active)
        return;

    inferenceActive_ = active;
    inferenceActiveTickCounter_ = 0;

    startTimerHz(inferenceActive_ ? kHeartbeatHzInferenceActive : kHeartbeatHzIdle);
    arrangementView_.setInferenceActive(inferenceActive_);
    pianoRoll_.setInferenceActive(inferenceActive_);
    trackPanel_.setInferenceActive(inferenceActive_);
}

OpenTuneAudioProcessorEditor::OpenTuneAudioProcessorEditor(OpenTuneAudioProcessor& p)
    : AudioProcessorEditor(&p)
    , processorRef_(p)
    , languageState_(std::make_shared<LocalizationManager::LanguageState>(
          LocalizationManager::LanguageState{ appPreferences_.getState().shared.language }))
    , languageBinding_(languageState_)
    , menuBar_(p, MenuBarComponent::Profile::Standalone)
    , topBar_(menuBar_, transportBar_)
    , arrangementView_(p)
{
    // Initialize track volumes array
    lastTrackVolumes_.fill(1.0f);
    
    // Hide original menu bar as we moved it to TransportBar
    menuBar_.setVisible(false);

    // Set larger default size for the complete UI (increased height for menu bar)
    setResizable(true, true);
    setResizeLimits(1000, 700, 3000, 2000);
    setSize(1200, 900);

    UIColors::applyTheme(appPreferences_.getState().shared.theme);

    // Create Tech Cursor
    juce::Image cursorImg(juce::Image::ARGB, 32, 32, true);
    juce::Graphics g(cursorImg);
    g.setColour(UIColors::accent);
    g.drawLine(16.0f, 4.0f, 16.0f, 28.0f, 2.0f);
    g.drawLine(4.0f, 16.0f, 28.0f, 16.0f, 2.0f);
    g.drawEllipse(10.0f, 10.0f, 12.0f, 12.0f, 2.0f);
    g.fillEllipse(14.0f, 14.0f, 4.0f, 4.0f);
    techCursor_ = juce::MouseCursor(cursorImg, 16, 16);
    
    // Setup Menu Bar
    menuBar_.addListener(this);

#if JUCE_MAC
    // Populate the macOS system menu bar with File/Edit/View menus.
    // JUCE automatically adds "About OpenTune" and "Quit OpenTune" to the app menu.
    juce::MenuBarModel::setMacMainMenu(&menuBar_);
#endif

    // Register language change listener
    LocalizationManager::getInstance().addListener(this);

    // Setup Transport Bar Menu Callbacks
    // menuName 从运行时获取（语言切换后自动反映当前语言），与 getMenuForIndex 的索引匹配
    transportBar_.onFileMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        juce::PopupMenu menu = menuBar_.getMenuForIndex(0, menuNames.isEmpty() ? juce::String() : menuNames[0]);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&transportBar_.getFileButton())
                                                     .withParentComponent(this),
                           [this](int result) {
                               if (result != 0) menuBar_.menuItemSelected(result, 0);
                           });
    };
    transportBar_.onEditMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        juce::PopupMenu menu = menuBar_.getMenuForIndex(1, menuNames.size() > 1 ? menuNames[1] : juce::String());
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&transportBar_.getEditButton())
                                                     .withParentComponent(this),
                           [this](int result) {
                               if (result != 0) menuBar_.menuItemSelected(result, 1);
                           });
    };
    transportBar_.onViewMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        juce::PopupMenu menu = menuBar_.getMenuForIndex(2, menuNames.size() > 2 ? menuNames[2] : juce::String());
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&transportBar_.getViewButton())
                                                     .withParentComponent(this),
                           [this](int result) {
                               if (result != 0) menuBar_.menuItemSelected(result, 2);
                           });
    };

#if JUCE_DEBUG
    static std::atomic<bool> ran{ false };
    if (!ran.exchange(true)) {
        const bool ok = runDebugSelfTests();
        if (!ok) {
            AppLogger::log("Debug self-tests failed");
            jassertfalse;
        }
        const auto selfTestEnv = juce::SystemStats::getEnvironmentVariable("OPENTUNE_SELFTEST", {});
        if (selfTestEnv == "1") {
            std::exit(ok ? 0 : 1);
        }
    }
#endif

    // Setup Transport Bar (includes Scale controls)
    transportBar_.addListener(this);
    transportBar_.setPlaying(processorRef_.isPlaying());
    transportBar_.setLoopEnabled(processorRef_.isLoopEnabled());
    transportBar_.setBpm(processorRef_.getBpm());

    // Initialize Scale (materialization > recent > default)
    {
        const int initTrack = getStandaloneActiveTrack(processorRef_);
        const int initPlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, initTrack);
        applyResolvedScaleForPlacementMaterialization(initTrack, initPlacementIndex);
    }

    addAndMakeVisible(topBar_);

    // 顶部条：侧边栏折叠开关
    topBar_.onToggleTrackPanel = [this]() {
        isTrackPanelVisible_ = !isTrackPanelVisible_;
        trackPanel_.setVisible(isTrackPanelVisible_);
        topBar_.setSidePanelsVisible(isTrackPanelVisible_, isParameterPanelVisible_);
        resized();
        repaint();
    };

    topBar_.onToggleParameterPanel = [this]() {
        isParameterPanelVisible_ = !isParameterPanelVisible_;
        parameterPanel_.setVisible(isParameterPanelVisible_);
        topBar_.setSidePanelsVisible(isTrackPanelVisible_, isParameterPanelVisible_);
        resized();
        repaint();
    };

    topBar_.setSidePanelsVisible(isTrackPanelVisible_, isParameterPanelVisible_);

    trackPanel_.addListener(this);
    trackPanel_.setActiveTrack(getStandaloneActiveTrack(processorRef_));
    // 初始化轨道高度（与ArrangementView同步）
    trackPanel_.setTrackHeight(processorRef_.getTrackHeight());
    // 初始化所有12条轨道的状态
    for (int i = 0; i < MAX_TRACKS; ++i)
    {
        trackPanel_.setTrackMuted(i, getStandaloneTrackMuted(processorRef_, i));
        trackPanel_.setTrackSolo(i, getStandaloneTrackSolo(processorRef_, i));
        trackPanel_.setTrackVolume(i, getStandaloneTrackVolume(processorRef_, i));
    }
    addAndMakeVisible(trackPanel_);

    // Setup Parameter Panel
    parameterPanel_.addListener(this);
    // parameterPanel_.setRetuneSpeed(processorRef_.getRetuneSpeed());
    parameterPanel_.setRetuneSpeed(PitchControlConfig::kDefaultRetuneSpeedPercent);
    pianoRoll_.setRetuneSpeed(PitchControlConfig::kDefaultRetuneSpeedNormalized);
    parameterPanel_.setNoteSplit(PitchControlConfig::kDefaultNoteSplitCents);
    pianoRoll_.setNoteSplit(PitchControlConfig::kDefaultNoteSplitCents);
    
    parameterPanel_.setF0Min(30.0f);
    parameterPanel_.setF0Max(2000.0f);
    
    addAndMakeVisible(parameterPanel_);

    arrangementView_.addListener(this);
    arrangementView_.setZoomLevel(processorRef_.getZoomLevel());
    addAndMakeVisible(arrangementView_);

    // Setup Piano Roll (main editor area)
    pianoRoll_.addListener(this);
    pianoRoll_.setProcessor(&processorRef_);
    pianoRoll_.setPianoKeyAudition(&processorRef_.getPianoKeyAudition());
    {
        const int activeTrack = getStandaloneActiveTrack(processorRef_);
        const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
        const uint64_t placementId = (placementIndex >= 0) ? processorRef_.getPlacementId(activeTrack, placementIndex) : 0;
        applyPlacementSelectionContext(activeTrack, placementId);
    }
    pianoRoll_.setBpm(processorRef_.getBpm());
    pianoRoll_.setTimeSignature(processorRef_.getTimeSigNumerator(), processorRef_.getTimeSigDenominator());
    pianoRoll_.setShowWaveform(processorRef_.getShowWaveform());
    pianoRoll_.setShowLanes(processorRef_.getShowLanes());
    pianoRoll_.setZoomLevel(processorRef_.getZoomLevel());
    
    // 设置高性能播放头位置源 - 直接从 Processor 读取，绕过 60Hz Timer 瓶颈
    pianoRoll_.setPlayheadPositionSource(processorRef_.getPositionAtomic());
    arrangementView_.setPlayheadPositionSource(processorRef_.getPositionAtomic());
    
    addAndMakeVisible(pianoRoll_);
    pianoRoll_.setVisible(!isWorkspaceView_);
    arrangementView_.setVisible(isWorkspaceView_);

    // Add AutoRenderOverlay (initially hidden, covers PianoRoll during AUTO)
    addAndMakeVisible(autoRenderOverlay_);
    autoRenderOverlay_.setVisible(false);

    addAndMakeVisible(renderBadge_);
    renderBadge_.setVisible(false);

    // Ensure initial focus
    if (isWorkspaceView_)
        arrangementView_.grabKeyboardFocus();
    else
        pianoRoll_.grabKeyboardFocus();

    // Add Ripple Overlay (Topmost)
    addAndMakeVisible(rippleOverlay_);
    // No need for setAlwaysOnTop on component level, we handle z-order in resized

    applyThemeToEditor(appPreferences_.getState().shared.theme);

    // Apply the purple theme to the window
    getLookAndFeel().setColour(juce::ResizableWindow::backgroundColourId, UIColors::backgroundDark);

    // 启用原生标题栏（系统风格的最大化/最小化/关闭按钮）
    juce::Timer::callAfterDelay(60, [safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this)]
    {
        if (safeThis == nullptr) return;
        if (auto* window = safeThis->findParentComponentOfClass<juce::DocumentWindow>())
        {
            // 使用原生标题栏，让用户可以使用系统标准的最大化按钮
            window->setUsingNativeTitleBar(true);
            window->setColour(juce::DocumentWindow::backgroundColourId, UIColors::backgroundMedium);
            window->repaint();
        }
    });

    // 播放头渲染走 VBlank 覆盖层，主编辑器同步心跳降到 30Hz 减轻消息线程压力
    startTimerHz(kHeartbeatHzIdle);

    // VocoderRenderScheduler queue depth is polled via getVocoderScheduler()->getQueueDepth()

    // Hide the standalone "Options" button and Mute Warning if running in standalone mode
    juce::Timer::callAfterDelay(50, [safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this)]() {
        if (safeThis == nullptr) return;
        if (auto* topLevel = safeThis->getTopLevelComponent())
        {
            // 2. Hide Options Button & Notification
            for (auto* child : topLevel->getChildren())
            {
                if (auto* button = dynamic_cast<juce::Button*>(child))
                {
                    if (button->getButtonText().trim().equalsIgnoreCase("Options"))
                    {
                        button->setVisible(false);
                    }
                }
                
                // Try to find the Notification Component
                if (auto* label = dynamic_cast<juce::Label*>(child))
                {
                     if (label->getText().containsIgnoreCase("Audio input is muted"))
                         label->getParentComponent()->setVisible(false);
                }
            }
            
            topLevel->repaint();
        }
    });

    syncSharedAppPreferences();
}

OpenTuneAudioProcessorEditor::~OpenTuneAudioProcessorEditor()
{
    // Stop timer
#if JUCE_MAC
    // Clear the macOS system menu bar before menuBar_ is destroyed.
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif

    stopTimer();

    // Ensure import/deferred background tasks are fully completed
    // before tearing down UI/listeners to avoid lifetime races.
    waitForBackgroundUiTasks();

    // Safely join export worker thread if it exists
    if (exportWorker_.joinable())
    {
        exportWorker_.join();
    }

    // Remove custom LookAndFeel
    setLookAndFeel(nullptr);

    // Remove language change listener
    LocalizationManager::getInstance().removeListener(this);

    transportBar_.removeListener(this);
    trackPanel_.removeListener(this);
    arrangementView_.removeListener(this);
    parameterPanel_.removeListener(this);
    pianoRoll_.removeListener(this);
}

void OpenTuneAudioProcessorEditor::launchBackgroundUiTask(std::function<void()> task)
{
    if (!task)
        return;

    for (auto it = backgroundTasks_.begin(); it != backgroundTasks_.end();)
    {
        if (it->valid() && it->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            try { it->get(); } catch (const std::exception& e) { AppLogger::error("[PluginEditor] Background task exception: " + juce::String(e.what())); } catch (...) { AppLogger::error("[PluginEditor] Background task unknown exception"); }
            it = backgroundTasks_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    backgroundTasks_.emplace_back(
        std::async(std::launch::async, [task = std::move(task)]() mutable { task(); }));
}

void OpenTuneAudioProcessorEditor::waitForBackgroundUiTasks()
{
    std::vector<std::future<void>> pending;
    pending.swap(backgroundTasks_);

    for (auto& future : pending)
    {
        if (!future.valid())
            continue;

        try { future.get(); } catch (const std::exception& e) { AppLogger::error("[PluginEditor] Wait for background task exception: " + juce::String(e.what())); } catch (...) { AppLogger::error("[PluginEditor] Wait for background task unknown exception"); }
    }
}

bool OpenTuneAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Undo, key))
    {
        if (!shouldAcceptUndoRedoShortcut()) {
            return true;
        }
        undoRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Redo, key))
    {
        if (!shouldAcceptUndoRedoShortcut()) {
            return true;
        }
        redoRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::PlayPause, key))
    {
        playPauseToggleRequested();
        return true;
    }
    
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::PlayFromStart, key))
    {
        playFromStartToggleRequested();
        return true;
    }

    return false;
}

bool OpenTuneAudioProcessorEditor::shouldAcceptUndoRedoShortcut()
{
    const uint32_t nowMs = juce::Time::getMillisecondCounter();
    constexpr uint32_t debounceMs = 120;

    if (lastUndoRedoShortcutMs_ != 0 && (nowMs - lastUndoRedoShortcutMs_) < debounceMs) {
        return false;
    }

    lastUndoRedoShortcutMs_ = nowMs;
    return true;
}

bool OpenTuneAudioProcessorEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    static const juce::String kImportExtensionSpec = getImportExtensionSpec();
    for (const auto& path : files)
    {
        const juce::File f(path);
        if (f.hasFileExtension(kImportExtensionSpec))
            return true;
    }
    return false;
}

void OpenTuneAudioProcessorEditor::filesDropped(const juce::StringArray& files, int x, int y)
{
    juce::ignoreUnused(x, y);

    if (isImportInProgress_)
    {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                juce::String::fromUTF8(u8"导入音频"),
                juce::String::fromUTF8(u8"当前正在导入音频，请稍后再试。")
            );
        return;
    }

    if (files.isEmpty())
        return;

    const juce::File file(files[0]);
    if (!file.existsAsFile())
        return;

    static const juce::String kImportExtensionSpec = getImportExtensionSpec();
    if (!file.hasFileExtension(kImportExtensionSpec))
    {
            const auto wildcard = getImportWildcardFilter().replaceCharacters("*", "");
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                juce::String::fromUTF8(u8"导入音频"),
                juce::String::fromUTF8(u8"不支持的文件类型。\n当前版本支持：") + wildcard
            );
        return;
    }

    if (files.size() > 1)
    {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                juce::String::fromUTF8(u8"导入音频"),
                juce::String::fromUTF8(u8"检测到多个文件，本次将仅导入第一个文件。")
            );
    }

    promptTrackSelectionForDroppedFile(file);
}

void OpenTuneAudioProcessorEditor::paint(juce::Graphics& g)
{
    // Solid background (Soft Blue-Grey)
    g.fillAll(UIColors::backgroundDark);
}

void OpenTuneAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    rippleOverlay_.setBounds(bounds);
    rippleOverlay_.toFront(false);

    // 阴影边距：为各面板预留阴影渲染空间
    // 各组件 paint() 使用 reduced(shadowMargin) 绘制背景，阴影在边距内渲染
    const int shadowMargin = 12;
    const int gap = 6;  // Gap between panels (视觉间距，不含阴影)

    bounds.reduce(gap, gap); // Global padding

    // TopBar：高度 + 阴影边距（上下各12px）
    const int topBarHeight = menuBar_.isVisible() ? (MENU_BAR_HEIGHT + TRANSPORT_BAR_HEIGHT) : TRANSPORT_BAR_HEIGHT;
    const int topBarHeightWithShadow = topBarHeight + shadowMargin * 2;
    topBar_.setBounds(bounds.removeFromTop(topBarHeightWithShadow));
    // 视觉间距：gap 减去已被阴影占用的下边距
    bounds.removeFromTop(juce::jmax(0, gap - shadowMargin));

    // 左侧 Track Inspector（可折叠）
    // 宽度 + 阴影边距（左右各12px）
    if (isTrackPanelVisible_)
    {
        trackPanel_.setVisible(true);
        const int trackPanelWidthWithShadow = TRACK_PANEL_WIDTH + shadowMargin * 2;
        trackPanel_.setBounds(bounds.removeFromLeft(trackPanelWidthWithShadow));
        bounds.removeFromLeft(juce::jmax(0, gap - shadowMargin));
    }
    else
    {
        trackPanel_.setVisible(false);
        trackPanel_.setBounds({});
    }

    // 右侧 Properties Panel（可折叠）
    // 宽度 + 阴影边距（左右各12px）
    if (isParameterPanelVisible_)
    {
        parameterPanel_.setVisible(true);
        const int paramPanelWidthWithShadow = PARAMETER_PANEL_WIDTH + shadowMargin * 2;
        parameterPanel_.setBounds(bounds.removeFromRight(paramPanelWidthWithShadow));
        bounds.removeFromRight(juce::jmax(0, gap - shadowMargin));
    }
    else
    {
        parameterPanel_.setVisible(false);
        parameterPanel_.setBounds({});
    }

    // 中央区域（PianoRoll / ArrangementView）
    // PianoRoll 已经使用 reduced(12.0f) 绘制背景，bounds 保持不变
    arrangementView_.setBounds(bounds);
    pianoRoll_.setBounds(bounds);
    
    // AutoRenderOverlay 覆盖整个 PianoRoll 区域
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
    syncSharedAppPreferences();

    auto* vocoderDomain = processorRef_.getVocoderDomain();
    const bool inferenceNow = pianoRoll_.isAutoTuneProcessing();
    setInferenceActive(inferenceNow);

    syncParameterPanelFromSelection();

    if (arrangementView_.isShowing()) {
        arrangementView_.onHeartbeatTick();
    }

    if (pianoRoll_.isShowing()) {
        pianoRoll_.onHeartbeatTick();
    }

    const bool allowSecondaryRefresh = !inferenceActive_ || ((++inferenceActiveTickCounter_ % 4) == 0);

    // Sync other state if needed (e.g. from Toolbar or ParameterPanel)
    if (allowSecondaryRefresh && !f0ParamsSyncedFromInference_ && processorRef_.isInferenceReady()) {
        auto* f0Service = processorRef_.getF0Service();
        if (f0Service) {
            parameterPanel_.setF0Min(f0Service->getF0Min());
            parameterPanel_.setF0Max(f0Service->getF0Max());
            f0ParamsSyncedFromInference_ = true;
        }
    }

    // Update playhead position from processor
    double currentPositionSeconds = processorRef_.getPosition();
    double sampleRate = processorRef_.getSampleRate();

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
    
    if (allowSecondaryRefresh && sampleRate > 0.0) {
        const int sr = static_cast<int>(sampleRate);
        const int activeTrack = getStandaloneActiveTrack(processorRef_);
        const int activePlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
        const uint64_t activeMaterializationId = (activeTrack >= 0 && activePlacementIndex >= 0)
            ? getStandaloneMaterializationId(processorRef_, activeTrack, activePlacementIndex)
            : 0;
        auto curve = processorRef_.getMaterializationPitchCurveById(activeMaterializationId);
        std::shared_ptr<const juce::AudioBuffer<float>> materializationBuffer =
            processorRef_.getMaterializationAudioBufferById(activeMaterializationId);
        if (activeMaterializationId != lastPianoRollMaterializationId_
            || sr != lastPianoRollSampleRate_
            || curve != lastPianoRollCurve_
            || materializationBuffer != lastPianoRollBuffer_) {
            pianoRoll_.setEditedMaterialization(activeMaterializationId, curve, materializationBuffer, sr);
            lastPianoRollMaterializationId_ = activeMaterializationId;
            lastPianoRollSampleRate_ = sr;
            lastPianoRollCurve_ = curve;
            lastPianoRollBuffer_ = materializationBuffer;
        }
    }

    // 播放头位置由各组件通过 positionSource_ 直接从 Processor 读取
    transportBar_.setPositionSeconds(currentPositionSeconds);

    const RenderStatusSnapshot statusSnapshot = getRenderStatusSnapshot();

    // RMVPE overlay：与 vocoder 无关，独立于渲染状态
    if (rmvpeOverlayLatched_ && !isWorkspaceView_) {
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

    bool shouldShowOverlay = false;

    if (rmvpeOverlayLatched_ && !isWorkspaceView_) {
        autoRenderOverlay_.setMessageText(juce::String::fromUTF8("正在分析音高"));
        shouldShowOverlay = true;
    }

    // Sync Rendering Progress（vocoder 相关）
    if (vocoderDomain != nullptr) {
        const int activeTrack = getStandaloneActiveTrack(processorRef_);
        const int activePlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);

        const uint64_t activeMaterializationId = (activeTrack >= 0 && activePlacementIndex >= 0)
            ? getStandaloneMaterializationId(processorRef_, activeTrack, activePlacementIndex)
            : 0;
        const auto chunkStats = processorRef_.getMaterializationChunkStatsById(activeMaterializationId);
        const bool isTxnActive = chunkStats.hasActiveWork();

        const bool isAutoProcessing = pianoRoll_.isAutoTuneProcessing();

        if (isAutoProcessing) {
            const int olDone = chunkStats.idle + chunkStats.blank;
            const int olTotal = chunkStats.total();
            const float olProgress = (olTotal > 0) ? static_cast<float>(olDone) / static_cast<float>(olTotal) : 0.0f;
            autoRenderOverlay_.setMessageText(buildRenderingOverlayTitle(olDone, olTotal, olProgress));
            shouldShowOverlay = true;
        }

        const bool shouldShowBadge = isTxnActive && !isAutoProcessing;
        if (shouldShowBadge) {
            const int stDone = chunkStats.idle + chunkStats.blank;
            const int stTotal = chunkStats.total();
            renderBadge_.setMessageText(juce::String::fromUTF8(u8"\u6e32\u67d3\u4e2d (")
                + juce::String(stDone) + "/" + juce::String(stTotal) + ")");
        }
        if (renderBadge_.isVisible() != shouldShowBadge) {
            renderBadge_.setVisible(shouldShowBadge);
        }
        transportBar_.setRenderStatusText(juce::String());
    }

    if (autoRenderOverlay_.isVisible() != shouldShowOverlay) {
        autoRenderOverlay_.setVisible(shouldShowOverlay);
    }

#if JUCE_DEBUG
    if (++diagnosticHeartbeatCounter_ >= 300) {
        diagnosticHeartbeatCounter_ = 0;
        const auto diagnosticInfo = processorRef_.getDiagnosticInfo(getStandaloneActiveTrack(processorRef_), statusSnapshot.placementId);
        AppLogger::log("StandaloneEditor: render status=" + renderStatusToString(statusSnapshot.status)
            + " materializationId=" + juce::String(static_cast<juce::int64>(diagnosticInfo.materializationId))
            + " placementId=" + juce::String(static_cast<juce::int64>(diagnosticInfo.placementId))
            + " desiredRev=" + juce::String(static_cast<juce::int64>(diagnosticInfo.desiredRevision))
            + " publishedRev=" + juce::String(static_cast<juce::int64>(diagnosticInfo.publishedRevision))
            + " pending=" + juce::String(diagnosticInfo.chunkStats.pending)
            + " running=" + juce::String(diagnosticInfo.chunkStats.running)
            + " lastControl=" + diagnosticInfo.lastControlCall);
    }
#endif

    // Sync playing state (Fix for inconsistent UI state)
    if (transportBar_.isPlaying() != processorRef_.isPlaying())
    {
        transportBar_.setPlaying(processorRef_.isPlaying());
        pianoRoll_.setIsPlaying(processorRef_.isPlaying());
        arrangementView_.setIsPlaying(processorRef_.isPlaying());
    }

    if (allowSecondaryRefresh) {
        for (int i = 0; i < OpenTuneAudioProcessor::MAX_TRACKS; ++i) {
            const float rmsDb = getStandaloneTrackRms(processorRef_, i);
            trackPanel_.setTrackLevel(i, rmsDb);
        }
    }
}

void OpenTuneAudioProcessorEditor::syncSharedAppPreferences()
{
    const auto preferencesState = appPreferences_.getState();
    const auto& sharedPreferences = preferencesState.shared;
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
    arrangementView_.setZoomSensitivity(sharedPreferences.zoomSensitivity);
    menuBar_.setNoteNameMode(visualPreferences.noteNameMode);
    menuBar_.setShowChunkBoundaries(visualPreferences.showChunkBoundaries);
    menuBar_.setShowUnvoicedFrames(visualPreferences.showUnvoicedFrames);

    shortcutSettings_ = preferencesState.standalone.shortcuts;
    pianoRoll_.setShortcutSettings(shortcutSettings_);
    arrangementView_.setShortcutSettings(shortcutSettings_);
    menuBar_.setMouseTrailTheme(preferencesState.standalone.mouseTrailTheme);
    rippleOverlay_.setTrailTheme(preferencesState.standalone.mouseTrailTheme);
}

RenderStatusSnapshot OpenTuneAudioProcessorEditor::getRenderStatusSnapshot() const
{
    const int trackId = getStandaloneActiveTrack(processorRef_);
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    const uint64_t placementId = (placementIndex >= 0) ? processorRef_.getPlacementId(trackId, placementIndex) : 0;
    const uint64_t materializationId = getStandaloneMaterializationId(processorRef_, trackId, placementIndex);

    RenderStatusSnapshot snapshot;
    snapshot.materializationId = materializationId;
    snapshot.placementId = placementId;
    if (materializationId == 0) {
        return snapshot;
    }

    auto renderCache = processorRef_.getMaterializationRenderCacheById(materializationId);
    if (renderCache == nullptr) {
        snapshot.materializationId = 0;
        snapshot.placementId = 0;
        return snapshot;
    }

    return makeRenderStatusSnapshot(materializationId, placementId, renderCache->getStateSnapshot());
}

void OpenTuneAudioProcessorEditor::syncPianoRollFromPlacementSelection(int trackId, int placementIndex)
{
    StandaloneArrangement::Placement placement;
    const bool hasPlacement = (placementIndex >= 0)
        && getStandalonePlacementByIndex(processorRef_, trackId, placementIndex, placement);
    const uint64_t materializationId = hasPlacement ? placement.materializationId : 0;

    pianoRoll_.setMaterializationProjection(hasPlacement ? makePianoRollProjection(placement, processorRef_)
                                                  : MaterializationTimelineProjection{});

    const int sr = static_cast<int>(processorRef_.getSampleRate());
    std::shared_ptr<const juce::AudioBuffer<float>> materializationBuffer =
        processorRef_.getMaterializationAudioBufferById(materializationId);
    auto curve = processorRef_.getMaterializationPitchCurveById(materializationId);
    pianoRoll_.setEditedMaterialization(materializationId, curve, materializationBuffer, sr);

    lastPianoRollMaterializationId_ = materializationId;
    lastPianoRollSampleRate_ = sr;
    lastPianoRollCurve_ = curve;
    lastPianoRollBuffer_ = materializationBuffer;

    applyResolvedScaleForPlacementMaterialization(trackId, placementIndex);

}

void OpenTuneAudioProcessorEditor::applyPlacementSelectionContext(int trackId, uint64_t placementId)
{
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS)
    {
        pianoRoll_.setMaterializationProjection({});
        pianoRoll_.setEditedMaterialization(0, nullptr, nullptr, static_cast<int>(processorRef_.getSampleRate()));
        lastPianoRollMaterializationId_ = 0;
        lastPianoRollCurve_.reset();
        lastPianoRollBuffer_.reset();
        return;
    }

    setStandaloneActiveTrack(processorRef_, trackId);
    trackPanel_.setActiveTrack(trackId);

    if (placementId == 0)
    {
        setStandaloneSelectedPlacementIndex(processorRef_, trackId, -1);
        pianoRoll_.setMaterializationProjection({});
        pianoRoll_.setEditedMaterialization(0, nullptr, nullptr, static_cast<int>(processorRef_.getSampleRate()));
        lastPianoRollMaterializationId_ = 0;
        lastPianoRollCurve_.reset();
        lastPianoRollBuffer_.reset();
        return;
    }

    const int placementIndex = processorRef_.findPlacementIndexById(trackId, placementId);
    if (placementIndex < 0)
    {
        setStandaloneSelectedPlacementIndex(processorRef_, trackId, -1);
        pianoRoll_.setMaterializationProjection({});
        pianoRoll_.setEditedMaterialization(0, nullptr, nullptr, static_cast<int>(processorRef_.getSampleRate()));
        lastPianoRollMaterializationId_ = 0;
        lastPianoRollCurve_.reset();
        lastPianoRollBuffer_.reset();
        return;
    }

    setStandaloneSelectedPlacementIndex(processorRef_, trackId, placementIndex);
    syncPianoRollFromPlacementSelection(trackId, placementIndex);
}

void OpenTuneAudioProcessorEditor::toolSelected(int toolId)
{
    if (toolId < 0 || toolId > static_cast<int>(ToolId::HandDraw)) {
        return;
    }

    auto tool = static_cast<ToolId>(toolId);
    pianoRoll_.setCurrentTool(tool);
}

// ============================================================================
// ParameterPanel::Listener Implementation
// ============================================================================

void OpenTuneAudioProcessorEditor::retuneSpeedChanged(float speed)
{
    float normalizedSpeed = speed / 100.0f;
    pianoRoll_.setRetuneSpeed(normalizedSpeed);
    if (pianoRoll_.applyRetuneSpeedToSelection(normalizedSpeed)) {
        return;
    }

    const float vibratoDepth = parameterPanel_.getVibratoDepth();
    const float vibratoRate = parameterPanel_.getVibratoRate();
    pianoRoll_.applyCorrectionAsyncForEntireClip(normalizedSpeed, vibratoDepth, vibratoRate);
}

void OpenTuneAudioProcessorEditor::vibratoDepthChanged(float value)
{
    if (pianoRoll_.applyVibratoDepthToSelection(value)) return;
    pianoRoll_.setVibratoDepth(value);
    const float speed = parameterPanel_.getRetuneSpeed() / 100.0f;
    const float rate = parameterPanel_.getVibratoRate();
    pianoRoll_.applyCorrectionAsyncForEntireClip(speed, value, rate);
}

void OpenTuneAudioProcessorEditor::vibratoRateChanged(float value)
{
    if (pianoRoll_.applyVibratoRateToSelection(value)) return;
    pianoRoll_.setVibratoRate(value);
    const float speed = parameterPanel_.getRetuneSpeed() / 100.0f;
    const float depth = parameterPanel_.getVibratoDepth();
    pianoRoll_.applyCorrectionAsyncForEntireClip(speed, depth, value);
}

void OpenTuneAudioProcessorEditor::noteSplitChanged(float value)
{
    pianoRoll_.setNoteSplit(value);
}

void OpenTuneAudioProcessorEditor::parameterDragEnded(int paramId, float oldValue, float newValue)
{
}

// ============================================================================
// MenuBarComponent::Listener Implementation
// ============================================================================

// 导入模式枚举
enum class ImportMode
{
    SameTrack,      // 按顺序导入到同一个轨道
    SeparateTracks  // 分别导入到多个轨道（齐头）
};

void OpenTuneAudioProcessorEditor::importAudioRequested()
{
    // 新版本：直接弹出文件选择窗口，支持多选
    // 导入哪个轨道由当前选中轨道决定
    DBG("OpenTuneAudioProcessorEditor::importAudioRequested called");

    if (isImportInProgress_)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            juce::String::fromUTF8(u8"导入音频"),
            juce::String::fromUTF8(u8"当前正在导入音频，请稍后再试。")
        );
        return;
    }

    const auto wildcardFilter = getImportWildcardFilter();
    auto chooser = std::make_shared<juce::FileChooser>(
        juce::String::fromUTF8(u8"选择要导入的音频文件"),
        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        wildcardFilter
    );

    // 支持多选
    auto chooserFlags = juce::FileBrowserComponent::openMode 
                      | juce::FileBrowserComponent::canSelectFiles 
                      | juce::FileBrowserComponent::canSelectMultipleItems;

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    chooser->launchAsync(chooserFlags, [safeThis, chooser](const juce::FileChooser& fc)
    {
        if (safeThis == nullptr)
            return;

        const juce::Array<juce::File>& selectedFiles = fc.getResults();
        
        if (selectedFiles.isEmpty())
        {
            DBG("No files selected");
            return;
        }

        // 获取当前选中的轨道
        int currentTrack = getStandaloneActiveTrack(safeThis->processorRef_);
        int visibleTracks = safeThis->trackPanel_.getVisibleTrackCount();

        if (selectedFiles.size() == 1)
        {
            // 单文件：直接导入到当前选中的轨道
            safeThis->importAudioFileToTrack(currentTrack, selectedFiles[0]);
        }
        else
        {
            // 多文件：弹窗询问导入模式
            auto* alert = new juce::AlertWindow(
        juce::String::fromUTF8(u8"选择导入模式"),
        juce::String::fromUTF8(u8"您选择了 ") + juce::String(selectedFiles.size()) + juce::String::fromUTF8(u8" 个音频文件，请选择导入方式："),
        juce::AlertWindow::QuestionIcon
    );

            alert->addButton(juce::String::fromUTF8(u8"顺序导入到当前轨道"), 1);
            alert->addButton(juce::String::fromUTF8(u8"分别导入到多个轨道"), 2);
            alert->addButton(juce::String::fromUTF8(u8"取消"), 0);

            // 保存文件列表供回调使用
            auto filesPtr = std::make_shared<juce::Array<juce::File>>(selectedFiles);

            alert->enterModalState(
                true,
                juce::ModalCallbackFunction::create([safeThis, filesPtr, currentTrack, visibleTracks](int result)
                {
                    if (safeThis == nullptr)
                        return;

                    if (result == 0)
                    {
                        // 用户取消
                        return;
                    }
                    else if (result == 1)
                    {
                        // 顺序导入到同一轨道（当前选中轨道）
                        const int batchId = safeThis->nextImportBatchId_++;
                        safeThis->importBatchNextStartSeconds_[batchId] = safeThis->computeTrackAppendStartSeconds(currentTrack);
                        safeThis->importBatchRemainingItems_[batchId] = filesPtr->size();

                        for (int i = 0; i < filesPtr->size(); ++i)
                        {
                            OpenTuneAudioProcessorEditor::PendingImport pending;
                            pending.placement.trackId = currentTrack;
                            pending.file = (*filesPtr)[i];
                            pending.batchId = batchId;
                            pending.appendSequentially = true;
                            safeThis->queuePendingImport(std::move(pending));
                        }
                    }
                    else if (result == 2)
                    {
                        // 齐头导入多个轨道
                        const int remainingTrackCapacity = juce::jmax(0, OpenTuneAudioProcessor::MAX_TRACKS - currentTrack);
                        const int acceptedFileCount = juce::jmin(filesPtr->size(), remainingTrackCapacity);
                        if (acceptedFileCount <= 0)
                        {
                            juce::AlertWindow::showMessageBoxAsync(
                                juce::AlertWindow::WarningIcon,
                                juce::String::fromUTF8(u8"导入失败"),
                                juce::String::fromUTF8(u8"当前轨道之后没有剩余可用轨道。")
                            );
                            return;
                        }
                        
                        // 自动扩展可见轨道数量
                        int requiredTracks = currentTrack + acceptedFileCount;
                        if (requiredTracks > visibleTracks)
                        {
                            int newVisibleTracks = std::min(requiredTracks, OpenTuneAudioProcessor::MAX_TRACKS);
                            safeThis->trackPanel_.setVisibleTrackCount(newVisibleTracks);
                        }

                        // 从当前轨道开始，依次导入到后续轨道
                        for (int i = 0; i < acceptedFileCount; ++i)
                        {
                            OpenTuneAudioProcessorEditor::PendingImport pending;
                            pending.placement.trackId = currentTrack + i;
                            pending.placement.timelineStartSeconds = 0.0;
                            pending.file = (*filesPtr)[i];
                            safeThis->queuePendingImport(std::move(pending));
                        }

                        if (acceptedFileCount < filesPtr->size())
                        {
                            juce::AlertWindow::showMessageBoxAsync(
                                juce::AlertWindow::InfoIcon,
                                juce::String::fromUTF8(u8"导入数量已裁剪"),
                                juce::String::fromUTF8(u8"当前轨道之后只剩 ")
                                    + juce::String(acceptedFileCount)
                                    + juce::String::fromUTF8(u8" 条可用轨道，超出的文件未加入导入队列。")
                            );
                        }
                    }
                }),
                true  // 自动删除AlertWindow
            );
        }
    });
}

void OpenTuneAudioProcessorEditor::promptTrackSelectionForDroppedFile(const juce::File& file)
{
    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    auto* alert = new juce::AlertWindow(
        juce::String::fromUTF8(u8"导入音频"),
        juce::String::fromUTF8(u8"您希望将音频文件添加到："),
        juce::AlertWindow::NoIcon
    );

    int visibleTracks = trackPanel_.getVisibleTrackCount();
    for (int i = 0; i < visibleTracks; ++i)
    {
        alert->addButton(juce::String::fromUTF8(u8"轨道") + juce::String(i + 1), i + 1);
    }
    alert->addButton(juce::String::fromUTF8(u8"取消"), 0);

    alert->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, file, visibleTracks](int result)
        {
            if (safeThis == nullptr)
                return;

            if (result >= 1 && result <= visibleTracks)
                safeThis->importAudioFileToTrack(result - 1, file);
        }),
        true
    );
}

void OpenTuneAudioProcessorEditor::importAudioFileToTrack(int trackId, const juce::File& file)
{
    OpenTuneAudioProcessor::ImportPlacement placement;
    placement.trackId = trackId;
    placement.timelineStartSeconds = computeTrackAppendStartSeconds(trackId);

    PendingImport pendingImport;
    pendingImport.placement = placement;
    pendingImport.file = file;
    queuePendingImport(std::move(pendingImport));
}

void OpenTuneAudioProcessorEditor::queuePendingImport(PendingImport pendingImport)
{
    if (isImportInProgress_)
    {
        importQueue_.push_back(std::move(pendingImport));
        return;
    }

    startPendingImport(std::move(pendingImport));
}

void OpenTuneAudioProcessorEditor::startPendingImport(PendingImport pendingImport)
{
    if (!pendingImport.placement.isValid())
    {
        processNextImportInQueue();
        return;
    }

    isImportInProgress_ = true;

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
    const auto sourceFile = pendingImport.file;
    const auto fileName = sourceFile.getFileName();

    asyncAudioLoader_.loadAudioFile(
        sourceFile,
        {},
        [safeThis, pendingImport = std::move(pendingImport), fileName](AsyncAudioLoader::LoadResult result) mutable
        {
            if (safeThis == nullptr)
                return;

            if (!result.success)
            {
                safeThis->isImportInProgress_ = false;
                safeThis->releaseImportBatchSlot(pendingImport.batchId);
                safeThis->processNextImportInQueue();
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    juce::String::fromUTF8(u8"导入失败"),
                    result.errorMessage
                );
                return;
            }

            OpenTuneAudioProcessor::ImportPlacement placement = pendingImport.placement;
            if (pendingImport.appendSequentially)
            {
                const auto cursorIt = safeThis->importBatchNextStartSeconds_.find(pendingImport.batchId);
                placement.timelineStartSeconds = cursorIt != safeThis->importBatchNextStartSeconds_.end()
                    ? cursorIt->second
                    : safeThis->computeTrackAppendStartSeconds(placement.trackId);
            }

            safeThis->launchBackgroundUiTask([safeThis,
                                              pendingImport,
                                              placement,
                                              fileName,
                                              sampleRate = result.sampleRate,
                                              audioBuffer = std::move(result.audioBuffer)]() mutable
            {
                if (safeThis == nullptr)
                    return;

                OpenTuneAudioProcessor::PreparedImport preparedImport;
                {
                    if (!safeThis->processorRef_.prepareImport(std::move(audioBuffer), sampleRate, fileName, preparedImport))
                    {
                        juce::MessageManager::callAsync([safeThis, batchId = pendingImport.batchId]()
                        {
                            if (safeThis == nullptr)
                                return;
                            safeThis->isImportInProgress_ = false;
                            safeThis->releaseImportBatchSlot(batchId);
                            safeThis->processNextImportInQueue();
                            juce::AlertWindow::showMessageBoxAsync(
                                juce::AlertWindow::WarningIcon,
                                juce::String::fromUTF8(u8"导入失败"),
                                juce::String::fromUTF8(u8"导入预处理失败，请重试。")
                            );
                        });
                        return;
                    }
                }

                const double preparedImportDurationSeconds = TimeCoordinate::samplesToSeconds(preparedImport.storedAudioBuffer.getNumSamples(), TimeCoordinate::kRenderSampleRate);

                juce::MessageManager::callAsync([safeThis,
                                                pendingImport,
                                                placement,
                                                preparedImportDurationSeconds,
                                                preparedImport = std::move(preparedImport)]() mutable
                {
                    if (safeThis == nullptr)
                        return;

                    const auto committedPlacement = safeThis->processorRef_.commitPreparedImportAsPlacement(std::move(preparedImport), placement);
                    if (!committedPlacement.isValid())
                    {
                        safeThis->isImportInProgress_ = false;
                        safeThis->releaseImportBatchSlot(pendingImport.batchId);
                        safeThis->processNextImportInQueue();
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon,
                            juce::String::fromUTF8(u8"导入失败"),
                            juce::String::fromUTF8(u8"导入提交失败，请重试。")
                        );
                        return;
                    }

                    if (pendingImport.appendSequentially)
                    {
                        safeThis->importBatchNextStartSeconds_[pendingImport.batchId] = placement.timelineStartSeconds + preparedImportDurationSeconds;
                    }

                    safeThis->isImportInProgress_ = false;
                    safeThis->releaseImportBatchSlot(pendingImport.batchId);

                    safeThis->arrangementView_.grabKeyboardFocus();
                    safeThis->applyPlacementSelectionContext(placement.trackId, committedPlacement.placementId);
                    safeThis->pianoRoll_.setEditedMaterialization(committedPlacement.materializationId,
                                                          nullptr,
                                                            safeThis->processorRef_.getMaterializationAudioBufferById(committedPlacement.materializationId),
                                                          static_cast<int>(safeThis->processorRef_.getSampleRate()));
                    safeThis->lastPianoRollMaterializationId_ = committedPlacement.materializationId;
                    safeThis->lastPianoRollCurve_.reset();
                    safeThis->lastPianoRollBuffer_ = safeThis->processorRef_.getMaterializationAudioBufferById(committedPlacement.materializationId);

                    OpenTuneAudioProcessor::MaterializationRefreshRequest refreshRequest;
                    refreshRequest.materializationId = committedPlacement.materializationId;
                    if (!safeThis->processorRef_.requestMaterializationRefresh(refreshRequest)) {
                        AppLogger::log("ClipDerivedRefresh: standalone request rejected materializationId="
                            + juce::String(static_cast<juce::int64>(committedPlacement.materializationId)));
                    } else {
                        safeThis->rmvpeOverlayLatched_ = true;
                        safeThis->rmvpeOverlayTargetMaterializationId_ = committedPlacement.materializationId;
                    }

                    safeThis->arrangementView_.resetUserZoomFlag();
                    safeThis->pianoRoll_.resetUserZoomFlag();

                    FrameScheduler::instance().requestInvalidate(safeThis->arrangementView_, FrameScheduler::Priority::Interactive);

                    juce::Timer::callAfterDelay(100, [safeThis]() {
                        if (safeThis != nullptr && !safeThis->arrangementView_.hasUserManuallyZoomed()) {
                            safeThis->arrangementView_.fitToContent();
                        }
                    });

                    safeThis->processNextImportInQueue();
                });
            });
        }
    );
}

// 处理导入队列中的下一个文件
void OpenTuneAudioProcessorEditor::processNextImportInQueue()
{
    if (importQueue_.empty())
        return;
    
    // 取出队列中的第一个待导入项
    auto next = importQueue_.front();
    importQueue_.erase(importQueue_.begin());

    startPendingImport(std::move(next));
}

double OpenTuneAudioProcessorEditor::computeTrackAppendStartSeconds(int trackId) const
{
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS) {
        return 0.0;
    }

    const auto* arrangement = processorRef_.getStandaloneArrangement();
    jassert(arrangement != nullptr);

    double appendStartSeconds = 0.0;
    const int placementCount = arrangement->getNumPlacements(trackId);
    for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex)
    {
        StandaloneArrangement::Placement placement;
        if (!arrangement->getPlacementByIndex(trackId, placementIndex, placement) || placement.materializationId == 0) {
            continue;
        }

        const auto buffer = processorRef_.getMaterializationAudioBufferById(placement.materializationId);
        if (buffer == nullptr) {
            continue;
        }

        appendStartSeconds = std::max(appendStartSeconds, placement.timelineEndSeconds());
    }

    return appendStartSeconds;
}

void OpenTuneAudioProcessorEditor::releaseImportBatchSlot(int batchId)
{
    if (batchId == 0) {
        return;
    }

    const auto remainingIt = importBatchRemainingItems_.find(batchId);
    if (remainingIt == importBatchRemainingItems_.end()) {
        importBatchNextStartSeconds_.erase(batchId);
        return;
    }

    remainingIt->second -= 1;
    if (remainingIt->second > 0) {
        return;
    }

    importBatchRemainingItems_.erase(remainingIt);
    importBatchNextStartSeconds_.erase(batchId);
}

void OpenTuneAudioProcessorEditor::exportAudioRequested(MenuBarComponent::ExportType exportType)
{
    using ExportType = MenuBarComponent::ExportType;
    
    // Check if export is already in progress
    if (exportInProgress_.load())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            juce::String::fromUTF8("导出音频"),
            juce::String::fromUTF8("已有导出任务正在进行中，请稍后再试。"));
        return;
    }
    
    // 根据导出类型确定默认文件名
    juce::String defaultFileName;
    switch (exportType)
    {
        case ExportType::SelectedClip:
            defaultFileName = "selected_clip.wav";
            break;
        case ExportType::Track:
            defaultFileName = "track_" + juce::String(getStandaloneActiveTrack(processorRef_) + 1) + ".wav";
            break;
        case ExportType::Bus:
            defaultFileName = "master_mix.wav";
            break;
    }

    auto chooser = std::make_shared<juce::FileChooser>(
        "Export Audio File",
        juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(defaultFileName),
        "*.wav");

    auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles;

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    chooser->launchAsync(chooserFlags, [safeThis, exportType, chooser](const juce::FileChooser& fc)
    {
        if (safeThis == nullptr)
            return;

        auto file = fc.getResult();
        if (file == juce::File{})
            return;

        struct ExportRequest final
        {
            ExportType type{ ExportType::Bus };
            int trackId{ -1 };
            int placementIndex{ -1 };
            juce::String targetName;
        };

        ExportRequest request;
        request.type = exportType;

        switch (exportType)
        {
            case ExportType::SelectedClip:
            {
                request.trackId = getStandaloneActiveTrack(safeThis->processorRef_);
                request.placementIndex = getStandaloneSelectedPlacementIndex(safeThis->processorRef_, request.trackId);

                if (request.placementIndex < 0)
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        juce::String::fromUTF8("导出失败"),
                        juce::String::fromUTF8("没有选中的音频片段。请先在轨道上选择一个Clip。"));
                    return;
                }

                request.targetName = "Selected Placement (Track "
                    + juce::String(request.trackId + 1)
                    + ", Clip " + juce::String(request.placementIndex + 1) + ")";
                break;
            }

            case ExportType::Track:
            {
                request.trackId = getStandaloneActiveTrack(safeThis->processorRef_);
                request.targetName = "Track " + juce::String(request.trackId + 1);
                break;
            }

            case ExportType::Bus:
            {
                request.targetName = "Bus (Master Mix)";
                break;
            }
        }

        auto* processor = &safeThis->processorRef_;
        const auto outFile = file;
        const auto outRequest = request;
        const juce::Component::SafePointer<OpenTuneAudioProcessorEditor> uiSafe = safeThis;

        // Join previous export thread if it exists
        if (safeThis->exportWorker_.joinable())
        {
            safeThis->exportWorker_.join();
        }

        // Set export in progress flag
        safeThis->exportInProgress_.store(true);

        // Create new controlled export thread
        safeThis->exportWorker_ = std::thread([processor, outFile, outRequest, uiSafe]()
            {
                bool ok = false;
                juce::String errorText;

                switch (outRequest.type)
                {
                    case ExportType::SelectedClip:
                        ok = processor->exportPlacementAudio(outRequest.trackId, outRequest.placementIndex, outFile);
                        break;
                    case ExportType::Track:
                        ok = processor->exportTrackAudio(outRequest.trackId, outFile);
                        break;
                    case ExportType::Bus:
                        ok = processor->exportMasterMixAudio(outFile);
                        break;
                }

                if (!ok)
                {
                    errorText = processor->getLastExportError();
                }

                juce::MessageManager::callAsync([ok, outFile, outRequest, errorText, uiSafe]()
                {
                    // Check if editor is still alive
                    if (uiSafe == nullptr)
                        return;

                    // Clear export in progress flag
                    uiSafe->exportInProgress_.store(false);

                    if (ok)
                    {
                        DBG("Successfully exported " + outRequest.targetName);
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::InfoIcon,
                            juce::String::fromUTF8("导出完成"),
                            outRequest.targetName + juce::String::fromUTF8(" 已导出到: ") + outFile.getFullPathName());
                        return;
                    }

                    juce::String failText = juce::String::fromUTF8("无法导出音频到: ") + outFile.getFullPathName();
                    if (errorText.isNotEmpty())
                    {
                        failText += juce::String::fromUTF8("\n原因: ") + errorText;
                    }

                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        juce::String::fromUTF8("导出失败"),
                        failText);
                });
            });
    });
}

void OpenTuneAudioProcessorEditor::savePresetRequested()
{
    auto chooser = std::make_shared<juce::FileChooser>("Save Preset",
                                                         presetManager_.getDefaultPresetDirectory(),
                                                         "*.otpreset");

    auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles;

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    chooser->launchAsync(chooserFlags, [safeThis, chooser](const juce::FileChooser& fc)
    {
        if (safeThis == nullptr) {
            return;
        }

        auto file = fc.getResult();
        if (file != juce::File{})
        {
            if (!file.hasFileExtension(".otpreset"))
            {
                file = file.withFileExtension(".otpreset");
            }

            PresetData preset = safeThis->presetManager_.captureCurrentState(safeThis->processorRef_);
            preset.name = file.getFileNameWithoutExtension();

            if (safeThis->presetManager_.savePreset(preset, file))
            {
                DBG("Preset saved: " + file.getFullPathName());
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Preset Saved",
                    "Preset saved to: " + file.getFullPathName());
            }
            else
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Save Failed",
                    "Failed to save preset to: " + file.getFullPathName());
            }
        }
    });
}

void OpenTuneAudioProcessorEditor::loadPresetRequested()
{
    auto chooser = std::make_shared<juce::FileChooser>("Load Preset",
                                                         presetManager_.getDefaultPresetDirectory(),
                                                         "*.otpreset");

    auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    chooser->launchAsync(chooserFlags, [safeThis, chooser](const juce::FileChooser& fc)
    {
        if (safeThis == nullptr) {
            return;
        }

        auto file = fc.getResult();
        if (file != juce::File{})
        {
            PresetData preset = safeThis->presetManager_.loadPreset(file);

            if (preset.name.isNotEmpty())
            {
                safeThis->presetManager_.applyPreset(preset, safeThis->processorRef_);

                // Update UI to reflect loaded preset
                safeThis->parameterPanel_.setRetuneSpeed(preset.retuneSpeed);

                DBG("Preset loaded: " + preset.name);
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Preset Loaded",
                    "Loaded preset: " + preset.name);
            }
            else
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Load Failed",
                    "Failed to load preset from: " + file.getFullPathName());
            }
        }
    });
}

void OpenTuneAudioProcessorEditor::preferencesRequested()
{
    showPreferencesDialog();
}

void OpenTuneAudioProcessorEditor::showPreferencesDialog()
{
    auto* holder = juce::StandalonePluginHolder::getInstance();
    auto pages = StandalonePreferencePages::createAudioPages(
        holder != nullptr ? &holder->deviceManager : nullptr,
        appPreferences_,
        [this] { syncSharedAppPreferences(); },
        [this](bool forceCpu) { processorRef_.resetInferenceBackend(forceCpu); });

    auto sharedPages = SharedPreferencePages::create(appPreferences_, [this] { syncSharedAppPreferences(); });
    pages.insert(pages.end(),
                 std::make_move_iterator(sharedPages.begin()),
                 std::make_move_iterator(sharedPages.end()));

    auto standalonePages = StandalonePreferencePages::createStandaloneOnlyPages(appPreferences_, [this] {
        syncSharedAppPreferences();
    });
    pages.insert(pages.end(),
                 std::make_move_iterator(standalonePages.begin()),
                 std::make_move_iterator(standalonePages.end()));

    auto* dialogContent = new TabbedPreferencesDialog(std::move(pages));
    dialogContent->setSize(640, 560);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialogContent);
    options.dialogTitle = "Preferences";
    options.dialogBackgroundColour = UIColors::backgroundDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void OpenTuneAudioProcessorEditor::helpRequested()
{
#if JUCE_MAC
    // macOS: docs are in Contents/Resources/docs/ (executable is in Contents/MacOS/)
    auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    auto exeDir = exeFile.getParentDirectory();
    auto helpFile = exeDir.getParentDirectory().getChildFile("Resources").getChildFile("docs").getChildFile("UserGuide.html");
#else
    // Windows: docs are alongside the executable
    auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    auto exeDir = exeFile.getParentDirectory();
    auto helpFile = exeDir.getChildFile("docs").getChildFile("UserGuide.html");
#endif
    
    if (helpFile.exists())
    {
        helpFile.startAsProcess();
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            LOC(kClose),
            juce::String::fromUTF8(u8"无法找到帮助文档：") + helpFile.getFullPathName()
        );
    }
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

    if (themeId == ThemeId::Aurora)
    {
        setLookAndFeel(&auroraLookAndFeel_);
    }
    else
    {
        setLookAndFeel(&openTuneLookAndFeel_);
        openTuneLookAndFeel_.setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
        openTuneLookAndFeel_.setColour(juce::TextButton::buttonOnColourId, UIColors::accent);
        openTuneLookAndFeel_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        openTuneLookAndFeel_.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
    }

    getLookAndFeel().setColour(juce::ResizableWindow::backgroundColourId, UIColors::backgroundDark);

    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
    {
        window->setColour(juce::DocumentWindow::backgroundColourId, UIColors::backgroundMedium);
        window->repaint();
    }

    topBar_.applyTheme();
    topBar_.setSidePanelsVisible(isTrackPanelVisible_, isParameterPanelVisible_);
    trackPanel_.applyTheme();
    parameterPanel_.applyTheme();

    // 同步播放头颜色到高性能播放头覆盖层
    pianoRoll_.setPlayheadColour(UIColors::playhead);
    arrangementView_.setPlayheadColour(UIColors::playhead);

    sendLookAndFeelChange();
    repaint();

    PianoRollVisualInvalidationRequest pianoRollRefresh;
    pianoRollRefresh.reasonsMask = static_cast<uint32_t>(PianoRollVisualInvalidationReason::Viewport)
        | static_cast<uint32_t>(PianoRollVisualInvalidationReason::Content)
        | static_cast<uint32_t>(PianoRollVisualInvalidationReason::Decoration)
        | static_cast<uint32_t>(PianoRollVisualInvalidationReason::Interaction);
    pianoRollRefresh.fullRepaint = true;
    pianoRollRefresh.priority = PianoRollVisualInvalidationPriority::Interactive;
    pianoRoll_.invalidateVisual(pianoRollRefresh);

    repaint();
}

void OpenTuneAudioProcessorEditor::mouseTrailThemeChanged(MouseTrailConfig::TrailTheme theme)
{
    if (appPreferences_.getState().standalone.mouseTrailTheme != theme) {
        appPreferences_.setMouseTrailTheme(theme);
    }

    syncSharedAppPreferences();
    menuBar_.repaint();
    rippleOverlay_.repaint();
}

void OpenTuneAudioProcessorEditor::performUndoRedoAction(bool isUndo)
{
    auto* action = isUndo ? processorRef_.getUndoManager().undo()
                          : processorRef_.getUndoManager().redo();
    if (!action) return;

    const int activeTrack = getStandaloneActiveTrack(processorRef_);
    const int activePlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
    const uint64_t matId = (activeTrack >= 0 && activePlacementIndex >= 0)
        ? getStandaloneMaterializationId(processorRef_, activeTrack, activePlacementIndex) : 0;
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

void OpenTuneAudioProcessorEditor::undoRequested() { performUndoRedoAction(true); }

void OpenTuneAudioProcessorEditor::redoRequested() { performUndoRedoAction(false); }

void OpenTuneAudioProcessorEditor::languageChanged(Language newLanguage)
{
    juce::ignoreUnused(newLanguage);
    
    // 刷新菜单栏 - JUCE 需要调用 menuItemsChanged() 重建菜单
    menuBar_.menuItemsChanged();
    menuBar_.repaint();
    
    // 刷新顶部工具栏
    transportBar_.refreshLocalizedText();
    topBar_.refreshLocalizedText();
    
    // 刷新参数面板
    parameterPanel_.refreshLocalizedText();
    
    // 刷新整个界面
    repaint();
}

// ============================================================================
// TransportBarComponent::Listener Implementation
// ============================================================================

void OpenTuneAudioProcessorEditor::playRequested()
{
    processorRef_.setPlaying(true);
    processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Play);
    transportBar_.setPlaying(true);
    pianoRoll_.setIsPlaying(true);  // Notify PianoRoll for auto-scroll
    arrangementView_.setIsPlaying(true);  // Notify ArrangementView for overlay sync
}

void OpenTuneAudioProcessorEditor::pauseRequested()
{
    processorRef_.setPlaying(false);
    processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Pause);
    transportBar_.setPlaying(false);
    pianoRoll_.setIsPlaying(false);  // Notify PianoRoll to stop auto-scroll
    arrangementView_.setIsPlaying(false);  // Notify ArrangementView to stop overlay updates
}

void OpenTuneAudioProcessorEditor::stopRequested()
{
    processorRef_.setPlaying(false);
    processorRef_.setPosition(0);
    processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Stop);
    transportBar_.setPlaying(false);
    pianoRoll_.setIsPlaying(false);  // Notify PianoRoll to stop auto-scroll
    arrangementView_.setIsPlaying(false);  // Notify ArrangementView to stop overlay updates
}

void OpenTuneAudioProcessorEditor::loopToggled(bool enabled)
{
    processorRef_.setLoopEnabled(enabled);
}

void OpenTuneAudioProcessorEditor::bpmChanged(double newBpm)
{
    processorRef_.setBpm(newBpm);
    pianoRoll_.setBpm(newBpm);  // Update piano roll to redraw time grid
}

void OpenTuneAudioProcessorEditor::scaleChanged(int rootNote, int scaleType)
{
    if (suppressScaleChangedCallback_) {
        return;
    }

    const int activeTrack = getStandaloneActiveTrack(processorRef_);
    const int activePlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
    const uint64_t activeMaterializationId = getStandaloneMaterializationId(processorRef_, activeTrack, activePlacementIndex);

    const int newRoot = juce::jlimit(0, 11, rootNote);
    const int newScaleType = juce::jlimit(1, 8, scaleType);

    const DetectedKey oldResolved = resolveScaleForPlacementMaterialization(activeTrack, activePlacementIndex, nullptr);
    const int oldRootNote = static_cast<int>(oldResolved.root);
    const int oldScaleType = scaleToUiScaleType(oldResolved.scale);

    if (oldRootNote == newRoot && oldScaleType == newScaleType) {
        applyScaleToUi(newRoot, newScaleType);
        return;
    }

    const DetectedKey newKey = makeDetectedKeyFromUi(newRoot, newScaleType, 1.0f);

    if (activeMaterializationId != 0) {
        processorRef_.setMaterializationDetectedKeyById(activeMaterializationId, newKey);
    }
    applyScaleToUi(newRoot, newScaleType);

    DBG("ScaleSyncTrace: source=manual trackId=" + juce::String(activeTrack)
        + " placementIndex=" + juce::String(activePlacementIndex)
        + " materializationId=" + juce::String(static_cast<juce::int64>(activeMaterializationId))
        + " root=" + juce::String(newRoot)
        + " scale=" + juce::String(newScaleType));
}

void OpenTuneAudioProcessorEditor::viewToggled(bool workspaceView)
{
    isWorkspaceView_ = workspaceView;
    arrangementView_.setVisible(isWorkspaceView_);
    pianoRoll_.setVisible(!isWorkspaceView_);
    
    // Explicitly grab focus for the active view to ensure keyboard shortcuts work immediately
    if (isWorkspaceView_)
        arrangementView_.grabKeyboardFocus();
    else
        pianoRoll_.grabKeyboardFocus();

    resized();
    repaint();

    // 延迟调用自动缩放，确保resized()完成后执行
    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
    juce::Timer::callAfterDelay(50, [safeThis, workspaceView]() {
        if (safeThis == nullptr) return;

        if (workspaceView) {
            // 切换到ArrangementView
            if (!safeThis->arrangementView_.hasUserManuallyZoomed()) {
                safeThis->arrangementView_.fitToContent();
            }
        } else {
            // 切换到PianoRoll
            if (!safeThis->pianoRoll_.hasUserManuallyZoomed()) {
                safeThis->pianoRoll_.fitToScreen();
            }
        }
    });

}

// ============================================================================
// TrackPanelComponent::Listener Implementation
// ============================================================================

void OpenTuneAudioProcessorEditor::trackSelected(int trackId)
{
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    const uint64_t placementId = (placementIndex >= 0) ? processorRef_.getPlacementId(trackId, placementIndex) : 0;
    applyPlacementSelectionContext(trackId, placementId);
}

void OpenTuneAudioProcessorEditor::trackMuteToggled(int trackId, bool muted)
{
    setStandaloneTrackMuted(processorRef_, trackId, muted);
}

void OpenTuneAudioProcessorEditor::trackSoloToggled(int trackId, bool solo)
{
    setStandaloneTrackSolo(processorRef_, trackId, solo);
}

void OpenTuneAudioProcessorEditor::trackVolumeChanged(int trackId, float volume)
{
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS) return;
    
    setStandaloneTrackVolume(processorRef_, trackId, volume);
    lastTrackVolumes_[static_cast<size_t>(trackId)] = volume;
}

// Y轴缩放同步：当TrackPanel或ArrangementView通过Ctrl+滚轮缩放时，同步另一个组件
void OpenTuneAudioProcessorEditor::trackHeightChanged(int newHeight)
{
    // 更新processor中的轨道高度
    processorRef_.setTrackHeight(newHeight);
    
    // 同步TrackPanel（如果不是由它触发的）
    if (trackPanel_.getTrackHeight() != newHeight)
    {
        trackPanel_.setTrackHeight(newHeight);
    }
    
    // 刷新ArrangementView
    arrangementView_.repaint();
}

void OpenTuneAudioProcessorEditor::placementSelectionChanged(int trackId, uint64_t placementId)
{
    applyPlacementSelectionContext(trackId, placementId);

    // 如果当前在PianoRoll视图，且用户没有手动缩放过，自动适配新clip
    if (!isWorkspaceView_) {
        juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
        juce::Timer::callAfterDelay(100, [safeThis]() {
            if (safeThis != nullptr && !safeThis->pianoRoll_.hasUserManuallyZoomed()) {
                safeThis->pianoRoll_.fitToScreen();
            }
        });
    }
}

void OpenTuneAudioProcessorEditor::placementTimingChanged(int trackId, int placementIndex)
{
    if (getStandaloneActiveTrack(processorRef_) == trackId
        && getStandaloneSelectedPlacementIndex(processorRef_, trackId) == placementIndex)
    {
        syncPianoRollFromPlacementSelection(trackId, placementIndex);
    }
}

// Y轴滚动同步：ArrangementView或TrackPanel滚动时通知另一个组件跟随
void OpenTuneAudioProcessorEditor::verticalScrollChanged(int newOffset)
{
    // 同步TrackPanel
    trackPanel_.setVerticalScrollOffset(newOffset);
    // 同步ArrangementView
    arrangementView_.setVerticalScrollOffset(newOffset);
}

void OpenTuneAudioProcessorEditor::placementDoubleClicked(int trackId, int placementIndex)
{
    // 1. Switch to Piano Roll View
    if (isWorkspaceView_)
    {
        transportBar_.setWorkspaceView(false);
        viewToggled(false); // 会触发自动缩放
    }
    else
    {
        // 如果已经在PianoRoll视图，也需要调用fitToScreen
        juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
        juce::Timer::callAfterDelay(50, [safeThis]() {
            if (safeThis != nullptr && !safeThis->pianoRoll_.hasUserManuallyZoomed()) {
                safeThis->pianoRoll_.fitToScreen();
            }
        });
    }

    // 2. Select the placement
    placementSelectionChanged(trackId, processorRef_.getPlacementId(trackId, placementIndex));

}


// ============================================================================
// PianoRollComponent::Listener Implementation
// ============================================================================

void OpenTuneAudioProcessorEditor::playheadPositionChangeRequested(double timeSeconds)
{
    processorRef_.setPosition(timeSeconds);
    processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Seek);
}

void OpenTuneAudioProcessorEditor::playPauseToggleRequested()
{
    // Toggle play/pause
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

void OpenTuneAudioProcessorEditor::playFromStartToggleRequested()
{
    if (processorRef_.isPlaying()) {
        processorRef_.setPlaying(false);
        double startPos = processorRef_.getPlayStartPosition();
        processorRef_.setPosition(startPos);
        processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Pause);
        transportBar_.setPlaying(false);
        pianoRoll_.setIsPlaying(false);
        arrangementView_.setIsPlaying(false);
    } else {
        double startPos = processorRef_.getPlayStartPosition();
        processorRef_.setPosition(startPos);
        processorRef_.setPlaying(true);
        processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Play);
        transportBar_.setPlaying(true);
        pianoRoll_.setIsPlaying(true);
        arrangementView_.setIsPlaying(true);
    }
}

void OpenTuneAudioProcessorEditor::autoTuneRequested()
{
    pianoRoll_.applyAutoTuneToSelection();
}

void OpenTuneAudioProcessorEditor::pitchCurveEdited(int startFrame, int endFrame)
{
    DBG("Editor: Pitch curve edited frames " + juce::String(startFrame) + " to " + juce::String(endFrame));
    
    int trackId = getStandaloneActiveTrack(processorRef_);
    int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    
    if (placementIndex < 0) return;

    const uint64_t materializationId = getStandaloneMaterializationId(processorRef_, trackId, placementIndex);
    if (materializationId == 0) {
        return;
    }

    auto curve = processorRef_.getMaterializationPitchCurveById(materializationId);
    if (!curve) {
        return;
    }

    int hopSize = curve->getHopSize();
    double f0SampleRate = curve->getSampleRate();
    if (hopSize <= 0 || f0SampleRate <= 0.0) {
        auto* f0Service = processorRef_.getF0Service();
        if (f0Service) {
            hopSize = f0Service->getF0HopSize();
            f0SampleRate = static_cast<double>(f0Service->getF0SampleRate());
        }
    }
    if (hopSize <= 0 || f0SampleRate <= 0.0) {
        return;
    }

    int numFrames = (int) curve->size();
    if (numFrames <= 0) {
        return;
    }

    if (startFrame > endFrame) {
        std::swap(startFrame, endFrame);
    }
    startFrame = std::max(0, startFrame);
    endFrame = std::min(endFrame, numFrames - 1);
    if (endFrame < startFrame) {
        return;
    }

    const double secondsPerFrame = static_cast<double>(hopSize) / f0SampleRate;
    const double editStartSec = static_cast<double>(startFrame) * secondsPerFrame;
    const double editEndSec = static_cast<double>(endFrame + 1) * secondsPerFrame;

    AppLogger::log("RenderTrace: pitchCurveEdited"
        " track=" + juce::String(trackId)
        + " placementIndex=" + juce::String(placementIndex)
        + " frameRange=[" + juce::String(startFrame) + "," + juce::String(endFrame) + "]"
        + " secRange=[" + juce::String(editStartSec, 3) + "," + juce::String(editEndSec, 3) + "]");

    processorRef_.enqueueMaterializationPartialRenderById(materializationId, editStartSec, editEndSec);
}

void OpenTuneAudioProcessorEditor::escapeKeyPressed()
{
    // ESC 等价于点击视图切换键：左右视图互切并同步按钮状态
    const bool targetWorkspaceView = !isWorkspaceView_;
    transportBar_.setWorkspaceView(targetWorkspaceView);
    viewToggled(targetWorkspaceView);
}

} // namespace OpenTune
