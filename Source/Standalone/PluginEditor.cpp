#include "PluginEditor.h"
#include "UI/UIColors.h"
#include "UI/FrameScheduler.h"
#include "UI/OptionsDialogComponent.h"
#include "Audio/AsyncAudioLoader.h"
#include "DSP/ChromaKeyDetector.h"
#include "Utils/PresetManager.h"
#include "Utils/PitchCurve.h"
#include "Utils/NoteGenerator.h"
#include "Utils/PitchControlConfig.h"
#include "Utils/AppLogger.h"
#include "Utils/TimeCoordinate.h"
#include "Utils/UndoAction.h"
#include "Utils/KeyShortcutConfig.h"
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <cstring>
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

static void initialiseImportFormatManager(juce::AudioFormatManager& formatManager)
{
    formatManager.registerBasicFormats();
}

static juce::String getImportWildcardFilter()
{
    juce::AudioFormatManager formatManager;
    initialiseImportFormatManager(formatManager);
    const auto wildcard = formatManager.getWildcardForAllFormats();
    if (wildcard.isNotEmpty())
        return wildcard;

    // 兜底：当底层未返回 wildcard 时仍提供基础格式
    return "*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3";
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

struct RenderStatusUiState
{
    bool showRendering = false;
    int uiPendingTasks = 0;
    juce::String detailText;
};

static RenderStatusUiState buildRenderStatusUiState(bool isTxnActive)
{
    RenderStatusUiState state;
    state.showRendering = isTxnActive;
    state.uiPendingTasks = isTxnActive ? 1 : 0;

    if (state.showRendering)
    {
        state.detailText = juce::String::fromUTF8(u8"音符渲染中");
    }

    return state;
}


} // namespace

#if JUCE_DEBUG
static bool runDebugSelfTests() {
    if (!ArrangementViewComponent::runDebugSelfTest()) {
        return false;
    }

    {
        const auto s0 = buildRenderStatusUiState(true);
        if (!s0.showRendering || s0.uiPendingTasks != 1) {
            return false;
        }
        if (!s0.detailText.contains(juce::String::fromUTF8(u8"音符渲染中"))) {
            return false;
        }

        const auto s1 = buildRenderStatusUiState(false);
        if (s1.showRendering || s1.uiPendingTasks != 0) {
            return false;
        }
        if (s1.detailText.isNotEmpty()) {
            return false;
        }

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
        // Calculate expectedEnd using same logic as NoteGenerator.cpp
        // hopSecs = 160 / 16000 = 0.01 seconds per frame
        // samplesPerFrame = 0.01 * 96000 = 960 samples
        // tailExtendMs defaults to 15.0f in NoteGeneratorPolicy
        // tailExtendSamples = ceil(15ms / 1000ms / hopSecs) * samplesPerFrame = 2 * 960 = 1920
        const double hopSecs = 160.0 / 16000.0;
        const int64_t samplesPerFrame = static_cast<int64_t>(hopSecs * 96000.0);
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

ScaleMode OpenTuneAudioProcessorEditor::scaleToScaleMode(Scale scale)
{
    switch (scale) {
        case Scale::Major:          return ScaleMode::Major;
        case Scale::Minor:          return ScaleMode::Minor;
        case Scale::Chromatic:      return ScaleMode::Chromatic;
        case Scale::HarmonicMinor:  return ScaleMode::HarmonicMinor;
        case Scale::Dorian:         return ScaleMode::Dorian;
        case Scale::Mixolydian:     return ScaleMode::Mixolydian;
        case Scale::PentatonicMajor:return ScaleMode::PentatonicMajor;
        case Scale::PentatonicMinor:return ScaleMode::PentatonicMinor;
        default:                    return ScaleMode::Chromatic;
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

DetectedKey OpenTuneAudioProcessorEditor::resolveScaleForClip(int trackId, int clipIndex, juce::String* sourceOut) const
{
    const auto defaultKey = []() {
        DetectedKey k;
        k.root = Key::C;
        k.scale = Scale::Major;
        k.confidence = 1.0f;
        return k;
    };

    const bool hasClip = (trackId >= 0
                       && trackId < OpenTuneAudioProcessor::MAX_TRACKS
                       && clipIndex >= 0
                       && clipIndex < processorRef_.getNumClips(trackId));

    if (hasClip) {
        const DetectedKey clipKey = processorRef_.getClipDetectedKey(trackId, clipIndex);
        if (clipKey.confidence > 0.0f) {
            if (sourceOut) *sourceOut = "clip";
            return clipKey;
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

void OpenTuneAudioProcessorEditor::applyResolvedScaleForClip(int trackId, int clipIndex)
{
    juce::String source;
    const DetectedKey key = resolveScaleForClip(trackId, clipIndex, &source);
    const int rootNote = static_cast<int>(key.root);
    const int scaleType = scaleToUiScaleType(key.scale);
    applyScaleToUi(rootNote, scaleType);

    const uint64_t clipId = (trackId >= 0 && clipIndex >= 0) ? processorRef_.getClipId(trackId, clipIndex) : 0;
    (void)clipId;
    DBG("ScaleSyncTrace: source=" + source
        + " trackId=" + juce::String(trackId)
        + " clipIndex=" + juce::String(clipIndex)
        + " clipId=" + juce::String(static_cast<juce::int64>(clipId))
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
    : AudioProcessorEditor(&p),
      processorRef_(p),
      tooltipWindow_(this),
      menuBar_(p),
      topBar_(menuBar_, transportBar_),
      arrangementView_(p)
{
    // Initialize track volumes array
    lastTrackVolumes_.fill(1.0f);
    
    // Hide original menu bar as we moved it to TransportBar
    menuBar_.setVisible(false);

    // Set larger default size for the complete UI (increased height for menu bar)
    setResizable(true, true);
    setResizeLimits(1000, 700, 3000, 2000);
    setSize(1200, 900);

    // Apply custom LookAndFeel globally
    setLookAndFeel(&openTuneLookAndFeel_);
    UIColors::applyTheme(Theme::getActiveTokens());
    openTuneLookAndFeel_.setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
    openTuneLookAndFeel_.setColour(juce::TextButton::buttonOnColourId, UIColors::accent);
    openTuneLookAndFeel_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    openTuneLookAndFeel_.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);

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
        const char* selfTestEnv = std::getenv("OPENTUNE_SELFTEST");
        if (selfTestEnv != nullptr && std::strcmp(selfTestEnv, "1") == 0) {
            std::exit(ok ? 0 : 1);
        }
    }
#endif

    // Setup Transport Bar (includes Scale controls)
    transportBar_.addListener(this);
    transportBar_.setPlaying(processorRef_.isPlaying());
    transportBar_.setLooping(processorRef_.isLoopEnabled());
    transportBar_.setBpm(processorRef_.getBpm());

    // Initialize Scale (clip > recent > default)
    {
        const int initTrack = processorRef_.getActiveTrackId();
        const int initClip = processorRef_.getSelectedClip(initTrack);
        applyResolvedScaleForClip(initTrack, initClip);
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
    trackPanel_.setActiveTrack(processorRef_.getActiveTrackId());
    // 初始化轨道高度（与ArrangementView同步）
    trackPanel_.setTrackHeight(processorRef_.getTrackHeight());
    // 初始化所有12条轨道的状态
    for (int i = 0; i < MAX_TRACKS; ++i)
    {
        trackPanel_.setTrackMuted(i, processorRef_.isTrackMuted(i));
        trackPanel_.setTrackSolo(i, processorRef_.isTrackSolo(i));
        trackPanel_.setTrackVolume(i, processorRef_.getTrackVolume(i));
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
    pianoRoll_.setRenderCompleteCallback([this]() {
        autoRenderOverlay_.setVisible(false);
    });
    pianoRoll_.setGlobalUndoManager(&processorRef_.getUndoManager());
    pianoRoll_.setProcessor(&processorRef_);
    int activeTrack = processorRef_.getActiveTrackId();
    int clipIndex = processorRef_.getSelectedClip(activeTrack);
    pianoRoll_.setCurrentClipContext(activeTrack, processorRef_.getClipId(activeTrack, clipIndex));
    std::shared_ptr<const juce::AudioBuffer<float>> clipBuffer =
        processorRef_.getClipAudioBuffer(activeTrack, clipIndex);
    if (clipBuffer)
        pianoRoll_.setAudioBuffer(clipBuffer, static_cast<int>(processorRef_.getSampleRate()));
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

    // Ensure initial focus
    if (isWorkspaceView_)
        arrangementView_.grabKeyboardFocus();
    else
        pianoRoll_.grabKeyboardFocus();

    // Add Ripple Overlay (Topmost)
    addAndMakeVisible(rippleOverlay_);
    // No need for setAlwaysOnTop on component level, we handle z-order in resized

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

    themeChanged(Theme::getActiveTheme());
}

OpenTuneAudioProcessorEditor::~OpenTuneAudioProcessorEditor()
{
#if JUCE_MAC
    // Clear the macOS system menu bar before menuBar_ is destroyed.
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif

    // Stop timer
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
    if (KeyShortcutConfig::matchesShortcut(KeyShortcutConfig::ShortcutId::Undo, key))
    {
        if (!shouldAcceptUndoRedoShortcut()) {
            return true;
        }
        performUndoWithRangeTracking();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(KeyShortcutConfig::ShortcutId::Redo, key))
    {
        if (!shouldAcceptUndoRedoShortcut()) {
            return true;
        }
        performRedoWithRangeTracking();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(KeyShortcutConfig::ShortcutId::PlayPause, key))
    {
        playPauseToggleRequested();
        return true;
    }
    
    if (KeyShortcutConfig::matchesShortcut(KeyShortcutConfig::ShortcutId::PlayFromStart, key))
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

    // Draw Buffering Indicator
    if (processorRef_.isBuffering()) {
        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.setFont(24.0f);
        g.drawText("Buffering...", pianoRoll_.getBounds(), juce::Justification::centred, false);
        
        // Draw spinning circle
        int size = 40;
        juce::Rectangle<int> spinnerArea(0, 0, size, size);
        spinnerArea.setCentre(pianoRoll_.getBounds().getCentre().translated(0, 40));
        
        float angle = static_cast<float>(juce::Time::getMillisecondCounter() % 1000) / 1000.0f * juce::MathConstants<float>::twoPi;
        
        g.setColour(juce::Colours::white);
        juce::Path p;
        p.addArc((float)spinnerArea.getX(), (float)spinnerArea.getY(), (float)size, (float)size, angle, angle + 2.5f, true);
        g.strokePath(p, juce::PathStrokeType(3.0f));
    }
    
    // Draw Fallback Warning
    if (processorRef_.isDrySignalFallback()) {
        g.setColour(juce::Colours::red);
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        juce::Rectangle<int> warningArea = transportBar_.getBounds().removeFromRight(150).reduced(5);
        g.drawText("! DRY FALLBACK", warningArea, juce::Justification::centredRight, false);
    }
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

}

void OpenTuneAudioProcessorEditor::syncParameterPanelFromSelection()
{
    float selectedRetuneSpeed = 0.0f;
    float selectedVibratoDepth = 0.0f;
    float selectedVibratoRate = 0.0f;

    if (pianoRoll_.getSingleSelectedNoteParameters(selectedRetuneSpeed, selectedVibratoDepth, selectedVibratoRate)) {
        parameterPanel_.setRetuneSpeed(selectedRetuneSpeed);
        parameterPanel_.setVibratoDepth(selectedVibratoDepth);
        parameterPanel_.setVibratoRate(selectedVibratoRate);
        showingSingleNoteParams_ = true;
        return;
    }

    if (showingSingleNoteParams_) {
        parameterPanel_.setRetuneSpeed(pianoRoll_.getCurrentRetuneSpeed() * 100.0f);
        parameterPanel_.setVibratoDepth(pianoRoll_.getCurrentVibratoDepth());
        parameterPanel_.setVibratoRate(pianoRoll_.getCurrentVibratoRate());
        showingSingleNoteParams_ = false;
    }
}

void OpenTuneAudioProcessorEditor::timerCallback()
{
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

    processDeferredImportPostProcessQueue();

    const bool allowSecondaryRefresh = !inferenceActive_ || ((++inferenceActiveTickCounter_ % 4) == 0);

    // Sync other state if needed (e.g. from Toolbar or ParameterPanel)
    // if (pianoRoll_.getAlignmentOffset() != processorRef_.getAlignmentOffset()) {
    //     pianoRoll_.setAlignmentOffset(processorRef_.getAlignmentOffset());
    // }
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
        const int activeTrack = processorRef_.getActiveTrackId();
        const int clipIndex = processorRef_.getSelectedClip(activeTrack);
        std::shared_ptr<const juce::AudioBuffer<float>> clipBuffer =
            processorRef_.getClipAudioBuffer(activeTrack, clipIndex);
        if (sr != lastPianoRollSampleRate_ || clipBuffer != lastPianoRollBuffer_) {
            pianoRoll_.setAudioBuffer(clipBuffer, sr);
            lastPianoRollSampleRate_ = sr;
            lastPianoRollBuffer_ = clipBuffer;
        }

        const bool hasUserAudio = (clipBuffer != nullptr);
        if (hasUserAudio != lastPianoRollHasUserAudio_) {
            pianoRoll_.setHasUserAudio(hasUserAudio);
            lastPianoRollHasUserAudio_ = hasUserAudio;
        }
    }

    // 播放头位置由各组件通过 positionSource_ 直接从 Processor 读取
    transportBar_.setPositionSeconds(currentPositionSeconds);

    // Sync Rendering Progress
    if (vocoderDomain != nullptr) {
        // [AUTO overlay completion] Use snapshot target (trackId+clipId) when latched, otherwise current selection
        int activeTrack = processorRef_.getActiveTrackId();
        int activeClip = processorRef_.getSelectedClip(activeTrack);
        bool autoOverlayTargetExists = true;

        if (autoOverlayLatched_) {
            activeTrack = autoOverlayTargetTrackId_;
            activeClip = (activeTrack >= 0 && autoOverlayTargetClipId_ != 0)
                ? processorRef_.findClipIndexById(activeTrack, autoOverlayTargetClipId_)
                : -1;
            autoOverlayTargetExists = (activeClip >= 0);
        }

        // 从目标 Clip 的 RenderCache 获取 Chunk 状态
        const auto chunkStats = processorRef_.getClipChunkStats(activeTrack, activeClip);
        const bool isTxnActive = chunkStats.hasActiveWork();
        const auto renderState = buildRenderStatusUiState(isTxnActive);
        const float progress = renderState.showRendering ? 1.0f : 0.0f;
        
        const bool isAutoProcessing = pianoRoll_.isAutoTuneProcessing();

        // RMVPE overlay 释放判定：F0 Ready + 上下文匹配 + F0可见
        if (rmvpeOverlayLatched_) {
            const int targetTrackId = rmvpeOverlayTargetTrackId_;
            const uint64_t targetClipId = rmvpeOverlayTargetClipId_;
            const int resolvedClipIndex = (targetTrackId >= 0 && targetClipId != 0)
                ? processorRef_.findClipIndexById(targetTrackId, targetClipId)
                : -1;

            bool shouldUnlatch = false;
            if (resolvedClipIndex < 0) {
                shouldUnlatch = true;
            } else {
                const auto f0State = processorRef_.getClipOriginalF0State(targetTrackId, resolvedClipIndex);
                if (f0State == OriginalF0State::Failed) {
                    shouldUnlatch = true;
                } else if (f0State == OriginalF0State::Ready) {
                    const bool contextMatches =
                        pianoRoll_.getCurrentTrackId() == targetTrackId
                        && pianoRoll_.getCurrentClipId() == targetClipId;
                    if (contextMatches && pianoRoll_.isCurrentClipOriginalF0Visible()) {
                        shouldUnlatch = true;
                    }
                }
            }

            if (shouldUnlatch) {
                rmvpeOverlayLatched_ = false;
                rmvpeOverlayTargetTrackId_ = -1;
                rmvpeOverlayTargetClipId_ = 0;
            }
        }

        // AUTO overlay 释放判定：渲染完成
        if (autoOverlayLatched_) {
            bool shouldUnlatch = false;
            if (!autoOverlayTargetExists) {
                shouldUnlatch = true;
            } else {
                // 检查 Chunk 是否全部完成（无 Pending/Running）
                const bool txnFinished = !chunkStats.hasActiveWork() && chunkStats.total() > 0;
                
                if (!isAutoProcessing && txnFinished) {
                    shouldUnlatch = true;
                    AppLogger::log("AUTO_OVERLAY_SUCCESS"
                        + juce::String(" track=") + juce::String(activeTrack)
                        + juce::String(" clip=") + juce::String(activeClip)
                        + juce::String(" chunks=") + juce::String(chunkStats.total()));
                }
            }

            if (shouldUnlatch) {
                autoOverlayLatched_ = false;
                autoOverlayTargetTrackId_ = -1;
                autoOverlayTargetClipId_ = 0;
            }
        }

        // 统一更新 overlay 可见性
        if (autoOverlayLatched_) {
            autoRenderOverlay_.setMessageText(juce::String::fromUTF8("正在渲染中"));
            autoRenderOverlay_.setVisible(true);
        } else if (rmvpeOverlayLatched_ && !isWorkspaceView_) {
            const bool isCurrentClipExtracting = 
                pianoRoll_.getCurrentTrackId() == rmvpeOverlayTargetTrackId_
                && pianoRoll_.getCurrentClipId() == rmvpeOverlayTargetClipId_;
            
            if (isCurrentClipExtracting) {
                autoRenderOverlay_.setMessageText(
                    juce::String::fromUTF8("调式检测中......"),
                    juce::String::fromUTF8("正在提取音高曲线...")
                );
                autoRenderOverlay_.setVisible(true);
            } else {
                autoRenderOverlay_.setVisible(false);
            }
        } else {
            autoRenderOverlay_.setVisible(false);
        }
        
        // [渲染弹窗] 仅在 AUTO overlay 未锁定时更新右上角状态栏。
        if (!autoOverlayLatched_) {
            pianoRoll_.setRenderingProgress(progress, renderState.uiPendingTasks);
        }

        juce::String status;
        if (processorRef_.isBuffering()) status << "Buffering";
        if (processorRef_.isDrySignalFallback()) {
            if (status.isNotEmpty()) status << " | ";
            status << "Dry";
        }
        if (renderState.showRendering && !autoOverlayLatched_) {
            if (status.isNotEmpty()) status << " | ";
            status << renderState.detailText;
        }
        transportBar_.setRenderStatusText(status);
    }

    // Sync playing state (Fix for inconsistent UI state)
    if (transportBar_.isPlaying() != processorRef_.isPlaying())
    {
        transportBar_.setPlaying(processorRef_.isPlaying());
        pianoRoll_.setIsPlaying(processorRef_.isPlaying());
        arrangementView_.setIsPlaying(processorRef_.isPlaying());
    }

    if (allowSecondaryRefresh) {
        for (int i = 0; i < OpenTuneAudioProcessor::MAX_TRACKS; ++i) {
            float rmsDb = processorRef_.getTrackRMS(i);
            trackPanel_.setTrackLevel(i, rmsDb);
        }
    }
}

void OpenTuneAudioProcessorEditor::syncPianoRollFromClipSelection(int trackId, int clipIndex)
{

    if (clipIndex >= 0) {
        pianoRoll_.setCurrentClipContext(trackId, processorRef_.getClipId(trackId, clipIndex));
    } else {
        pianoRoll_.clearClipContext();
    }

    pianoRoll_.setTrackTimeOffset(processorRef_.getClipStartSeconds(trackId, clipIndex));

    const int sr = static_cast<int>(processorRef_.getSampleRate());
    std::shared_ptr<const juce::AudioBuffer<float>> clipBuffer =
        processorRef_.getClipAudioBuffer(trackId, clipIndex);
    const bool hasUserAudio = (clipBuffer != nullptr);

    pianoRoll_.setAudioBuffer(clipBuffer, sr);
    pianoRoll_.setHasUserAudio(hasUserAudio);

    lastPianoRollSampleRate_ = sr;
    lastPianoRollBuffer_ = clipBuffer;
    lastPianoRollHasUserAudio_ = hasUserAudio;

    if (!hasUserAudio) {
        pianoRoll_.setPitchCurve(nullptr);
        pianoRoll_.setNotes({});
    }

    // Restore Pitch Curve
    auto curve = processorRef_.getClipPitchCurve(trackId, clipIndex);
    pianoRoll_.setPitchCurve(curve);

    // clip 切换只应用该 clip 的生效调式（无跨 clip 回退）
    applyResolvedScaleForClip(trackId, clipIndex);

    // Restore Notes
    pianoRoll_.setNotes(processorRef_.getClipNotes(trackId, clipIndex));
}

void OpenTuneAudioProcessorEditor::toolSelected(int toolId)
{
    if (toolId < 0 || toolId > static_cast<int>(ToolId::HandDraw)) {
        return;
    }

    auto tool = static_cast<ToolId>(toolId);
    if (tool == ToolId::AutoTune)
    {
        // 合并重复逻辑：调用统一 helper
        startAutoTuneAsUnifiedEdit();
        pianoRoll_.setCurrentTool(ToolId::Select);
        parameterPanel_.setActiveTool(1);
        return;
    }

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
        int currentTrack = safeThis->processorRef_.getActiveTrackId();
        int visibleTracks = safeThis->trackPanel_.getVisibleTrackCount();

        if (selectedFiles.size() == 1)
        {
            // 单文件：直接导入到当前选中的轨道
            safeThis->importAudioFileToTrack(currentTrack, selectedFiles[0]);
        }
        else
        {
            // 多文件：检查数量限制（最多12个）
            constexpr int MAX_IMPORT_FILES = 12;
            if (selectedFiles.size() > MAX_IMPORT_FILES)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    juce::String::fromUTF8(u8"导入数量超限"),
                    juce::String::fromUTF8(u8"最多同时导入12个音频文件。\n") +
                    juce::String::fromUTF8(u8"您选择了 ") + juce::String(selectedFiles.size()) + 
                    juce::String::fromUTF8(u8" 个文件。")
                );
                return;
            }

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
                        // 每个文件作为独立的Clip，按顺序排列
                        for (int i = 0; i < filesPtr->size(); ++i)
                        {
                            safeThis->importAudioFileToTrack(currentTrack, (*filesPtr)[i]);
                        }
                    }
                    else if (result == 2)
                    {
                        // 齐头导入多个轨道
                        int numFiles = filesPtr->size();
                        
                        // 自动扩展可见轨道数量
                        int requiredTracks = currentTrack + numFiles;
                        if (requiredTracks > visibleTracks)
                        {
                            int newVisibleTracks = std::min(requiredTracks, OpenTuneAudioProcessor::MAX_TRACKS);
                            safeThis->trackPanel_.setVisibleTrackCount(newVisibleTracks);
                        }

                        // 从当前轨道开始，依次导入到后续轨道
                        for (int i = 0; i < numFiles; ++i)
                        {
                            int targetTrack = currentTrack + i;
                            safeThis->importAudioFileToTrack(targetTrack, (*filesPtr)[i]);
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
    // 如果正在导入，将请求加入队列
    if (isImportInProgress_)
    {
        importQueue_.push_back({trackId, file});
        return;
    }

    isImportInProgress_ = true;

    juce::ignoreUnused(trackId);

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    asyncAudioLoader_.loadAudioFile(
        file,
        {},
        [safeThis, trackId, fileName = file.getFileName()](AsyncAudioLoader::LoadResult result)
        {
            if (safeThis == nullptr)
                return;

            if (!result.success)
            {
                safeThis->isImportInProgress_ = false;
                safeThis->processNextImportInQueue();
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    juce::String::fromUTF8(u8"导入失败"),
                    result.errorMessage
                );
                return;
            }

            // 关键修复：prepare 阶段必须在后台线程执行，不能阻塞消息线程
            safeThis->launchBackgroundUiTask([safeThis,
                                              trackId,
                                              fileName,
                                              sampleRate = result.sampleRate,
                                              audioBuffer = std::move(result.audioBuffer)]() mutable
            {
                if (safeThis == nullptr)
                    return;

                OpenTuneAudioProcessor::PreparedImportClip prepared;
                {
                    PerfTimer perfPrepare("import_prepare_phase_background");
                    if (!safeThis->processorRef_.prepareImportClip(trackId, std::move(audioBuffer), sampleRate, fileName, prepared))
                    {
                        juce::MessageManager::callAsync([safeThis]()
                        {
                            if (safeThis == nullptr)
                                return;
                            safeThis->isImportInProgress_ = false;
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

                juce::MessageManager::callAsync([safeThis, trackId, prepared = std::move(prepared)]() mutable
                {
                    if (safeThis == nullptr)
                        return;

                    PerfTimer perfCommit("import_commit_phase");

                    if (!safeThis->processorRef_.commitPreparedImportClip(std::move(prepared)))
                    {
                        safeThis->isImportInProgress_ = false;
                        safeThis->processNextImportInQueue();
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon,
                            juce::String::fromUTF8(u8"导入失败"),
                            juce::String::fromUTF8(u8"导入提交失败，请重试。")
                        );
                        return;
                    }

                    // 获取新创建的 clip ID 并创建 Undo Action
                    int newClipIndex = safeThis->processorRef_.getNumClips(trackId) - 1;
                    uint64_t newClipId = 0;
                    if (newClipIndex >= 0) {
                        newClipId = safeThis->processorRef_.getClipId(trackId, newClipIndex);
                        safeThis->processorRef_.getUndoManager().addAction(
                            std::make_unique<ClipCreateAction>(safeThis->processorRef_, trackId, newClipId)
                        );
                    }

                    // 标记当前导入完成
                    safeThis->isImportInProgress_ = false;

                    // 设置键盘焦点到 ArrangementView，确保 Ctrl+Z/Y 快捷键能正常工作
                    safeThis->arrangementView_.grabKeyboardFocus();

                    safeThis->processorRef_.setActiveTrack(trackId);
                    safeThis->trackPanel_.setActiveTrack(trackId);

                    int clipIndex = safeThis->processorRef_.getNumClips(trackId) - 1;
                    if (clipIndex < 0) {
                        clipIndex = 0;
                    }
                    safeThis->processorRef_.setSelectedClip(trackId, clipIndex);
                    clipIndex = safeThis->processorRef_.getSelectedClip(trackId);
                    int numClips = safeThis->processorRef_.getNumClips(trackId);
                    if (numClips > 0) {
                        jassert(clipIndex == numClips - 1);
                    }


                    // 统一使用 syncPianoRollFromClipSelection 设置 PianoRoll 状态
                    // 包括 setActiveTrackId、setCurrentClipContext、setAudioBuffer 等
                    safeThis->syncPianoRollFromClipSelection(trackId, clipIndex);

                    // 导入刚完成时 F0 数据尚不存在，覆盖 sync 中可能恢复的旧数据
                    safeThis->pianoRoll_.setPitchCurve(nullptr);
                    safeThis->pianoRoll_.setNotes({});

                    if (newClipId != 0) {
                        safeThis->arrangementView_.prioritizeWaveformBuildForClip(trackId, newClipId);
                        safeThis->deferredImportPostProcessQueue_.push_back({trackId, newClipId});
                    }

                    // 导入完成 - 单次 UI 刷新
                    safeThis->arrangementView_.resetUserZoomFlag();
                    safeThis->pianoRoll_.resetUserZoomFlag();

                    FrameScheduler::instance().requestInvalidate(safeThis->arrangementView_, FrameScheduler::Priority::Interactive);
                    FrameScheduler::instance().requestInvalidate(safeThis->pianoRoll_, FrameScheduler::Priority::Interactive);

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
    
    // 递归调用导入函数（此时 isImportInProgress_ 已经是 false）
    importAudioFileToTrack(next.trackId, next.file);
}

void OpenTuneAudioProcessorEditor::processDeferredImportPostProcessQueue()
{
    if (deferredImportPostProcessQueue_.empty()) {
        return;
    }

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    for (int i = static_cast<int>(deferredImportPostProcessQueue_.size()) - 1; i >= 0; --i)
    {
        const auto request = deferredImportPostProcessQueue_[static_cast<std::size_t>(i)];
        if (request.trackId < 0 || request.clipId == 0) {
            deferredImportPostProcessQueue_.erase(deferredImportPostProcessQueue_.begin() + i);
            continue;
        }

        deferredImportPostProcessQueue_.erase(deferredImportPostProcessQueue_.begin() + i);

        launchBackgroundUiTask([safeThis, request]() mutable
        {
            if (safeThis == nullptr) {
                return;
            }

            OpenTuneAudioProcessor::PreparedClipPostProcess prepared;
            if (!safeThis->processorRef_.prepareDeferredClipPostProcess(request.trackId, request.clipId, prepared)) {
                return;
            }

            juce::MessageManager::callAsync([safeThis, request, prepared = std::move(prepared)]() mutable
            {
                if (safeThis == nullptr) {
                    return;
                }

                if (!safeThis->processorRef_.commitDeferredClipPostProcess(request.trackId, request.clipId, std::move(prepared))) {
                    return;
                }

                const int clipIndex = safeThis->processorRef_.findClipIndexById(request.trackId, request.clipId);
                if (clipIndex >= 0) {
                    safeThis->requestOriginalF0ExtractionForImport(request.trackId, clipIndex);
                }

                // Refresh chunk boundaries now that silentGaps are available
                safeThis->pianoRoll_.updateChunkBoundaries();
            });
        });
    }
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
            defaultFileName = "track_" + juce::String(processorRef_.getActiveTrackId() + 1) + ".wav";
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
            int clipIndex{ -1 };
            juce::String targetName;
        };

        ExportRequest request;
        request.type = exportType;

        switch (exportType)
        {
            case ExportType::SelectedClip:
            {
                request.trackId = safeThis->processorRef_.getActiveTrackId();
                request.clipIndex = safeThis->processorRef_.getSelectedClip(request.trackId);

                if (request.clipIndex < 0)
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        juce::String::fromUTF8("导出失败"),
                        juce::String::fromUTF8("没有选中的音频片段。请先在轨道上选择一个Clip。"));
                    return;
                }

                request.targetName = "Selected Clip (Track "
                    + juce::String(request.trackId + 1)
                    + ", Clip " + juce::String(request.clipIndex + 1) + ")";
                break;
            }

            case ExportType::Track:
            {
                request.trackId = safeThis->processorRef_.getActiveTrackId();
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
                        ok = processor->exportClipAudio(outRequest.trackId, outRequest.clipIndex, outFile);
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

    chooser->launchAsync(chooserFlags, [this, chooser](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file != juce::File{})
        {
            if (!file.hasFileExtension(".otpreset"))
            {
                file = file.withFileExtension(".otpreset");
            }

            PresetData preset = presetManager_.captureCurrentState(processorRef_);
            preset.name = file.getFileNameWithoutExtension();

            if (presetManager_.savePreset(preset, file))
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

    chooser->launchAsync(chooserFlags, [this, chooser](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file != juce::File{})
        {
            PresetData preset = presetManager_.loadPreset(file);

            if (preset.name.isNotEmpty())
            {
                presetManager_.applyPreset(preset, processorRef_);

                // Update UI to reflect loaded preset
                parameterPanel_.setRetuneSpeed(preset.retuneSpeed);

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
    auto* dialogContent = new OptionsDialogComponent();
    dialogContent->setSize(520, 540);
    
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialogContent);
    options.dialogTitle = "Options";
    options.dialogBackgroundColour = UIColors::backgroundDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    
    options.launchAsync();
}

void OpenTuneAudioProcessorEditor::helpRequested()
{
    auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    auto exeDir = exeFile.getParentDirectory();
#if JUCE_MAC
    // macOS: docs are in Contents/Resources/docs/ (executable is in Contents/MacOS/)
    auto helpFile = exeDir.getParentDirectory().getChildFile("Resources").getChildFile("docs").getChildFile("UserGuide.html");
#else
    // Windows: docs are alongside the executable
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

void OpenTuneAudioProcessorEditor::noteNameModeChanged(int mode)
{
    pianoRoll_.setNoteNameMode(mode);
}

void OpenTuneAudioProcessorEditor::showChunkBoundariesToggled(bool show)
{
    pianoRoll_.setShowChunkBoundaries(show);
}

void OpenTuneAudioProcessorEditor::showUnvoicedFramesToggled(bool show)
{
    pianoRoll_.setShowUnvoicedFrames(show);
}

void OpenTuneAudioProcessorEditor::themeChanged(ThemeId themeId)
{
    Theme::setActiveTheme(themeId);
    UIColors::applyTheme(Theme::getActiveTokens());

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

    menuBar_.repaint();
    topBar_.repaint();
    trackPanel_.repaint();
    parameterPanel_.repaint();
    arrangementView_.repaint();
    pianoRoll_.repaint();

    sendLookAndFeelChange();
    repaint();
}

void OpenTuneAudioProcessorEditor::mouseTrailThemeChanged(MouseTrailConfig::TrailTheme theme)
{
    MouseTrailConfig::setTheme(theme);
    rippleOverlay_.repaint();
}

void OpenTuneAudioProcessorEditor::undoRequested()
{
    performUndoWithRangeTracking();
}

void OpenTuneAudioProcessorEditor::redoRequested()
{
    performRedoWithRangeTracking();
}

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

void OpenTuneAudioProcessorEditor::refreshAfterUndoRedo()
{
    pianoRoll_.refreshAfterUndoRedo();
    arrangementView_.repaint();
    trackPanel_.repaint();
}

void OpenTuneAudioProcessorEditor::performUndoWithRangeTracking()
{
    CorrectedSegmentsChangeAction::resetLastAffectedRange();
    processorRef_.performUndo();
    
    int start = CorrectedSegmentsChangeAction::getLastAffectedStartFrame();
    int end = CorrectedSegmentsChangeAction::getLastAffectedEndFrame();
    
    AppLogger::log("performUndoWithRangeTracking: diffRange=[" + juce::String(start) + "," + juce::String(end) + "]");
    
    pianoRoll_.refreshAfterUndoRedoWithRange(start, end);
    arrangementView_.repaint();
    trackPanel_.repaint();
}

void OpenTuneAudioProcessorEditor::performRedoWithRangeTracking()
{
    CorrectedSegmentsChangeAction::resetLastAffectedRange();
    processorRef_.performRedo();
    
    int start = CorrectedSegmentsChangeAction::getLastAffectedStartFrame();
    int end = CorrectedSegmentsChangeAction::getLastAffectedEndFrame();
    
    AppLogger::log("performRedoWithRangeTracking: diffRange=[" + juce::String(start) + "," + juce::String(end) + "]");
    
    pianoRoll_.refreshAfterUndoRedoWithRange(start, end);
    arrangementView_.repaint();
    trackPanel_.repaint();
}

// ============================================================================
// TransportBarComponent::Listener Implementation
// ============================================================================

void OpenTuneAudioProcessorEditor::playRequested()
{
    processorRef_.setPlaying(true);
    transportBar_.setPlaying(true);
    pianoRoll_.setIsPlaying(true);  // Notify PianoRoll for auto-scroll
    arrangementView_.setIsPlaying(true);  // Notify ArrangementView for overlay sync
}

void OpenTuneAudioProcessorEditor::pauseRequested()
{
    processorRef_.setPlaying(false);
    transportBar_.setPlaying(false);
    pianoRoll_.setIsPlaying(false);  // Notify PianoRoll to stop auto-scroll
    arrangementView_.setIsPlaying(false);  // Notify ArrangementView to stop overlay updates
}

void OpenTuneAudioProcessorEditor::stopRequested()
{
    processorRef_.setPlaying(false);
    processorRef_.setPosition(0);
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

    const int activeTrack = processorRef_.getActiveTrackId();
    const int activeClip = processorRef_.getSelectedClip(activeTrack);
    const uint64_t activeClipId = processorRef_.getClipId(activeTrack, activeClip);

    const int newRoot = juce::jlimit(0, 11, rootNote);
    const int newScaleType = juce::jlimit(1, 8, scaleType);

    const DetectedKey oldResolved = resolveScaleForClip(activeTrack, activeClip, nullptr);
    const int oldRootNote = static_cast<int>(oldResolved.root);
    const int oldScaleType = scaleToUiScaleType(oldResolved.scale);

    if (oldRootNote == newRoot && oldScaleType == newScaleType) {
        applyScaleToUi(newRoot, newScaleType);
        return;
    }

    const DetectedKey newKey = makeDetectedKeyFromUi(newRoot, newScaleType, 1.0f);

    if (activeClip >= 0) {
        processorRef_.setClipDetectedKey(activeTrack, activeClip, newKey);
    }
    applyScaleToUi(newRoot, newScaleType);

    DBG("ScaleSyncTrace: source=manual trackId=" + juce::String(activeTrack)
        + " clipIndex=" + juce::String(activeClip)
        + " clipId=" + juce::String(static_cast<juce::int64>(activeClipId))
        + " root=" + juce::String(newRoot)
        + " scale=" + juce::String(newScaleType));

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
    processorRef_.getUndoManager().addAction(
        std::make_unique<ClipScaleKeyChangeAction>(
            activeTrack,
            activeClipId,
            oldRootNote,
            oldScaleType,
            newRoot,
            newScaleType,
            [safeThis](int trackId, uint64_t clipId, int r, int s) {
                if (safeThis == nullptr) return;

                const int resolvedClipIndex = (clipId != 0)
                    ? safeThis->processorRef_.findClipIndexById(trackId, clipId)
                    : safeThis->processorRef_.getSelectedClip(trackId);

                const DetectedKey dk = OpenTuneAudioProcessorEditor::makeDetectedKeyFromUi(r, s, 1.0f);
                if (resolvedClipIndex >= 0) {
                    safeThis->processorRef_.setClipDetectedKey(trackId, resolvedClipIndex, dk);
                }

                const int activeTrackNow = safeThis->processorRef_.getActiveTrackId();
                const int activeClipNow = safeThis->processorRef_.getSelectedClip(activeTrackNow);
                const uint64_t activeClipIdNow = safeThis->processorRef_.getClipId(activeTrackNow, activeClipNow);
                const bool sameVisibleClip = (clipId != 0)
                    ? (activeTrackNow == trackId && activeClipIdNow == clipId)
                    : (activeTrackNow == trackId && activeClipNow == resolvedClipIndex);

                if (sameVisibleClip) {
                    safeThis->applyScaleToUi(r, s);
                }

                DBG("UndoTrace: ClipScaleKeyChangeAction trackId=" + juce::String(trackId)
                    + " clipId=" + juce::String(static_cast<juce::int64>(clipId))
                    + " root=" + juce::String(r)
                    + " scale=" + juce::String(s));
            })
    );
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
    processorRef_.setActiveTrack(trackId);
    processorRef_.setSelectedClip(trackId, processorRef_.getSelectedClip(trackId));
    int clipIndex = processorRef_.getSelectedClip(trackId);
    syncPianoRollFromClipSelection(trackId, clipIndex);
    
    pianoRoll_.repaint();
}

void OpenTuneAudioProcessorEditor::trackMuteToggled(int trackId, bool muted)
{
    bool oldMuted = processorRef_.isTrackMuted(trackId);
    processorRef_.setTrackMuted(trackId, muted);
    
    // 创建 Undo Action
    if (oldMuted != muted) {
        juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
        processorRef_.getUndoManager().addAction(
            std::make_unique<TrackMuteAction>(
                processorRef_, trackId, oldMuted, muted,
                [safeThis](int tid, bool m) {
                    if (safeThis) safeThis->trackPanel_.setTrackMuted(tid, m);
                }
            )
        );
    }
}

void OpenTuneAudioProcessorEditor::trackSoloToggled(int trackId, bool solo)
{
    bool oldSolo = processorRef_.isTrackSolo(trackId);
    processorRef_.setTrackSolo(trackId, solo);
    
    // 创建 Undo Action
    if (oldSolo != solo) {
        juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
        processorRef_.getUndoManager().addAction(
            std::make_unique<TrackSoloAction>(
                processorRef_, trackId, oldSolo, solo,
                [safeThis](int tid, bool s) {
                    if (safeThis) safeThis->trackPanel_.setTrackSolo(tid, s);
                }
            )
        );
    }
}

void OpenTuneAudioProcessorEditor::trackVolumeChanged(int trackId, float volume)
{
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS) return;
    
    float oldVolume = lastTrackVolumes_[static_cast<size_t>(trackId)];
    processorRef_.setTrackVolume(trackId, volume);
    lastTrackVolumes_[static_cast<size_t>(trackId)] = volume;
    
    // 只有值变化超过阈值时才创建 Undo Action（避免拖动时产生过多 Action）
    // 使用 0.01 作为阈值，约等于 0.1dB
    if (std::abs(oldVolume - volume) > 0.01f) {
        juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
        processorRef_.getUndoManager().addAction(
            std::make_unique<TrackVolumeAction>(
                processorRef_, trackId, oldVolume, volume,
                [safeThis](int tid, float v) {
                    if (safeThis) {
                        safeThis->trackPanel_.setTrackVolume(tid, v);
                        safeThis->lastTrackVolumes_[static_cast<size_t>(tid)] = v;
                    }
                }
            )
        );
    }
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

void OpenTuneAudioProcessorEditor::clipSelectionChanged(int trackId, int clipIndex)
{
    processorRef_.setActiveTrack(trackId);
    processorRef_.setSelectedClip(trackId, clipIndex);
    trackPanel_.setActiveTrack(trackId);

    syncPianoRollFromClipSelection(trackId, clipIndex);

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

void OpenTuneAudioProcessorEditor::clipTimingChanged(int trackId, int clipIndex)
{
    // Update PianoRoll if this clip is active
    if (processorRef_.getActiveTrackId() == trackId && processorRef_.getSelectedClip(trackId) == clipIndex)
    {
        pianoRoll_.setTrackTimeOffset(processorRef_.getClipStartSeconds(trackId, clipIndex));
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

void OpenTuneAudioProcessorEditor::clipDoubleClicked(int trackId, int clipIndex)
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

    // 2. Select the clip
    clipSelectionChanged(trackId, clipIndex);

}

void OpenTuneAudioProcessorEditor::performKeyDetectionForClip(int trackId, int clipIndex)
{
    const uint64_t clipId = processorRef_.getClipId(trackId, clipIndex);

    // 已有检测结果则跳过
    const auto existingKey = processorRef_.getClipDetectedKey(trackId, clipIndex);
    if (existingKey.confidence > 0.0f) {
        return;
    }

    // 从 clip 获取音频 buffer（Chroma 方案直接分析音频 PCM，不依赖 F0）
    auto audioBuffer = processorRef_.getClipAudioBuffer(trackId, clipIndex);
    if (!audioBuffer || audioBuffer->getNumSamples() <= 0) {
        DBG("ChromaKeyDetection: skip=no_audio trackId=" + juce::String(trackId)
            + " clipIndex=" + juce::String(clipIndex)
            + " clipId=" + juce::String(static_cast<juce::int64>(clipId)));
        return;
    }

    DBG("Performing Chroma Key Detection for Track " + juce::String(trackId) + " Clip " + juce::String(clipIndex));

    // 使用第一个声道（mono）进行调式检测
    const float* audioData = audioBuffer->getReadPointer(0);
    const int numSamples = audioBuffer->getNumSamples();
    const int sampleRate = 44100;  // 项目标准采样率

    ChromaKeyDetector detector;
    DetectedKey key = detector.detect(audioData, numSamples, sampleRate);

    DBG("ChromaKeyDetection: trackId=" + juce::String(trackId)
        + " clipIndex=" + juce::String(clipIndex)
        + " clipId=" + juce::String(static_cast<juce::int64>(clipId))
        + " samples=" + juce::String(numSamples)
        + " confidence=" + juce::String(key.confidence, 4));

    // 更新目标 clip 的检测结果
    processorRef_.setClipDetectedKey(trackId, clipIndex, key);

    const int scaleType = scaleToUiScaleType(key.scale);

    // 只在目标 clip 仍是当前可见 clip 时更新 UI
    const int activeTrack = processorRef_.getActiveTrackId();
    const int activeClip = processorRef_.getSelectedClip(activeTrack);
    const uint64_t activeClipId = processorRef_.getClipId(activeTrack, activeClip);
    const uint64_t targetClipId = clipId;
    const bool sameVisibleClip = (targetClipId != 0)
        ? (activeTrack == trackId && activeClipId == targetClipId)
        : (activeTrack == trackId && activeClip == clipIndex);

    if (sameVisibleClip) {
        applyScaleToUi(static_cast<int>(key.root), scaleType);
    }

    DBG("ScaleSyncTrace: source=chroma trackId=" + juce::String(trackId)
        + " clipIndex=" + juce::String(clipIndex)
        + " clipId=" + juce::String(static_cast<juce::int64>(targetClipId))
        + " root=" + juce::String(static_cast<int>(key.root))
        + " scale=" + juce::String(scaleType)
        + " sameVisible=" + juce::String(sameVisibleClip ? 1 : 0));
    
    DBG("Detected Key: " + DetectedKey::keyToString(key.root) + " " + DetectedKey::scaleToString(key.scale));
}

// Eager-first：仅导入完成时触发一次 OriginalF0 预提取
void OpenTuneAudioProcessorEditor::requestOriginalF0ExtractionForImport(int trackId, int clipIndex)
{
    const uint64_t clipId = processorRef_.getClipId(trackId, clipIndex);
    const uint64_t requestKey = F0ExtractionService::makeRequestKey(clipId, trackId, clipIndex);
    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    if (clipId != 0) {
        processorRef_.setClipOriginalF0StateById(trackId, clipId, OriginalF0State::Extracting);
    } else {
        processorRef_.setClipOriginalF0State(trackId, clipIndex, OriginalF0State::Extracting);
    }

    auto submitResult = f0ExtractionService_.submit(
        requestKey,
        [safeThis, trackId, clipIndex, clipId, requestKey]() -> F0ExtractionService::Result {
            F0ExtractionService::Result out;
            out.trackId = trackId;
            out.clipIndexHint = clipIndex;
            out.clipId = clipId;
            out.requestKey = requestKey;

            if (safeThis == nullptr) {
                out.errorMessage = "editor_destroyed";
                return out;
            }

            auto& procRef = safeThis->processorRef_;

            if (!procRef.initializeInferenceIfNeeded()) {
                out.errorMessage = "inference_not_ready";
                return out;
            }

            OpenTuneAudioProcessor::ClipSnapshot snap;
            if (clipId != 0) {
                if (!procRef.getClipSnapshot(trackId, clipId, snap)) {
                    out.errorMessage = "clip_snapshot_failed";
                    return out;
                }
            } else {
                std::shared_ptr<const juce::AudioBuffer<float>> clipBuffer =
                    procRef.getClipAudioBuffer(trackId, clipIndex);
                if (clipBuffer == nullptr || clipBuffer->getNumSamples() <= 0) {
                    out.errorMessage = "empty_clip_audio";
                    return out;
                }
                snap.audioBuffer = clipBuffer;
            }

            const int numSamples = snap.audioBuffer->getNumSamples();
            const int numChannels = snap.audioBuffer->getNumChannels();
            if (numSamples <= 0 || numChannels <= 0) {
                out.errorMessage = "invalid_audio_buffer";
                return out;
            }

            std::vector<float> monoAudio(static_cast<size_t>(numSamples), 0.0f);
            const float invChannels = 1.0f / static_cast<float>(numChannels);
            for (int ch = 0; ch < numChannels; ++ch) {
                const float* src = snap.audioBuffer->getReadPointer(ch);
                for (int i = 0; i < numSamples; ++i) {
                    monoAudio[static_cast<size_t>(i)] += src[i] * invChannels;
                }
            }

            // Use render sample rate (44.1kHz) for all calculations
            constexpr double internalSampleRate = TimeCoordinate::kRenderSampleRate;

            auto* f0Service = procRef.getF0Service();
            const int hopSize = f0Service ? f0Service->getF0HopSize() : 160;
            const int f0SampleRate = f0Service ? f0Service->getF0SampleRate() : 16000;

            const auto& silentGaps = snap.silentGaps;

            struct VoicedSegment {
                int64_t startSample;
                int64_t endSample;
            };
            std::vector<VoicedSegment> voicedSegments;

            if (silentGaps.empty()) {
                voicedSegments.push_back({0, static_cast<int64_t>(numSamples)});
            } else {
                int64_t prevEnd = 0;
                for (const auto& gap : silentGaps) {
                    const int64_t gapStart = static_cast<int64_t>(gap.startSeconds * internalSampleRate);
                    const int64_t gapEnd = static_cast<int64_t>(gap.endSeconds * internalSampleRate);
                    if (gapStart > prevEnd) {
                        voicedSegments.push_back({prevEnd, gapStart});
                    }
                    prevEnd = gapEnd;
                }
                if (prevEnd < numSamples) {
                    voicedSegments.push_back({prevEnd, numSamples});
                }
            }

            if (voicedSegments.empty()) {
                out.errorMessage = "no_voiced_segments";
                return out;
            }

            const size_t totalFrames = static_cast<size_t>(
                std::ceil(static_cast<double>(numSamples) * f0SampleRate / internalSampleRate / hopSize));
            std::vector<float> fullF0(totalFrames, 0.0f);
            std::vector<float> fullEnergy(totalFrames, 0.0f);

            for (const auto& seg : voicedSegments) {
                const size_t segStart = static_cast<size_t>(seg.startSample);
                const size_t segLength = static_cast<size_t>(seg.endSample - seg.startSample);
                if (segLength == 0) continue;

                std::vector<float> segAudio(segLength);
                std::copy(monoAudio.begin() + segStart, monoAudio.begin() + segStart + segLength, segAudio.begin());

                auto* segF0Service = procRef.getF0Service();
                if (!segF0Service) continue;
                
                auto f0Result = segF0Service->extractF0(segAudio.data(), segLength, static_cast<int>(internalSampleRate));
                if (!f0Result.ok()) continue;
                
                const auto& segF0 = f0Result.value();
                if (segF0.empty()) continue;
                
                std::vector<float> segEnergy(segF0.size(), 1.0f);

                const size_t segFrameOffset = static_cast<size_t>(
                    static_cast<double>(seg.startSample) * f0SampleRate / internalSampleRate / hopSize);

                for (size_t i = 0; i < segF0.size() && segFrameOffset + i < totalFrames; ++i) {
                    fullF0[segFrameOffset + i] = segF0[i];
                    if (i < segEnergy.size()) {
                        fullEnergy[segFrameOffset + i] = segEnergy[i];
                    }
                }
            }

            out.f0 = std::move(fullF0);
            out.energy = std::move(fullEnergy);
            out.hopSize = hopSize;
            out.f0SampleRate = f0SampleRate;
            out.modelName = "RMVPE";

            int voicedFrames = 0;
            for (float v : out.f0) {
                if (std::isfinite(v) && v > 0.0f) {
                    ++voicedFrames;
                }
            }
            const float voicedRatio = out.f0.empty() ? 0.0f : static_cast<float>(voicedFrames) / static_cast<float>(out.f0.size());
            if (out.f0.empty() || voicedRatio <= 0.001f) {
                out.errorMessage = "f0_empty_or_unvoiced";
                return out;
            }

            out.success = true;
            return out;
        },
        [safeThis](F0ExtractionService::Result&& result) {
            if (safeThis == nullptr) {
                return;
            }

            const auto resolveClipIndexFromRequest = [&result]() -> int {
                if (result.clipIndexHint >= 0) {
                    return result.clipIndexHint;
                }
                if (result.clipId == 0 && result.requestKey != 0) {
                    return static_cast<int>(static_cast<uint32_t>(result.requestKey));
                }
                return -1;
            };

            const auto applyTerminalState = [&result, safeThis, &resolveClipIndexFromRequest](OriginalF0State targetState) -> int {
                if (result.clipId != 0) {
                    if (safeThis->processorRef_.setClipOriginalF0StateById(result.trackId, result.clipId, targetState)) {
                        return safeThis->processorRef_.findClipIndexById(result.trackId, result.clipId);
                    }
                }

                const int fallbackIndex = resolveClipIndexFromRequest();
                if (fallbackIndex >= 0) {
                    safeThis->processorRef_.setClipOriginalF0State(result.trackId, fallbackIndex, targetState);
                }
                return fallbackIndex;
            };

            if (!result.success) {
                applyTerminalState(OriginalF0State::Failed);
                DBG("F0 extraction failed: key=" + juce::String(static_cast<juce::int64>(result.requestKey))
                    + " reason=" + juce::String(result.errorMessage));
                return;
            }

            const int resolvedClipIndex = applyTerminalState(OriginalF0State::Ready);
            if (resolvedClipIndex < 0) {
                DBG("F0 extraction completed without resolvable clip index: key="
                    + juce::String(static_cast<juce::int64>(result.requestKey))
                    + " track=" + juce::String(result.trackId)
                    + " clipId=" + juce::String(static_cast<juce::int64>(result.clipId)));
                return;
            }

            auto pitchCurve = std::make_shared<PitchCurve>();
            pitchCurve->setHopSize(result.hopSize);
            pitchCurve->setSampleRate(static_cast<double>(result.f0SampleRate));
            pitchCurve->setOriginalF0(result.f0);
            if (!result.energy.empty()) {
                pitchCurve->setOriginalEnergy(result.energy);
            }

            safeThis->processorRef_.setClipPitchCurve(result.trackId, resolvedClipIndex, pitchCurve);

            const int activeTrack = safeThis->processorRef_.getActiveTrackId();
            const int activeClip = safeThis->processorRef_.getSelectedClip(activeTrack);
            const uint64_t activeClipId = safeThis->processorRef_.getClipId(activeTrack, activeClip);
            const bool sameClip = (result.clipId != 0)
                ? (activeClipId == result.clipId)
                : (activeTrack == result.trackId && activeClip == resolvedClipIndex);

            if (sameClip) {
                safeThis->pianoRoll_.setPitchCurve(pitchCurve);
                safeThis->pianoRoll_.repaint();
            }

            safeThis->performKeyDetectionForClip(result.trackId, resolvedClipIndex);

            // Import-time note generation: generate notes from F0 curve
            {
                NoteGeneratorParams genParams;
                auto generatedNotes = NoteGenerator::generate(
                    result.f0,
                    result.energy,
                    result.hopSize,
                    static_cast<double>(result.f0SampleRate),
                    44100.0,
                    genParams);

                if (!generatedNotes.empty()) {
                    safeThis->processorRef_.setClipNotes(result.trackId, resolvedClipIndex, generatedNotes);
                    if (sameClip) {
                        safeThis->pianoRoll_.setNotes(generatedNotes);
                        safeThis->pianoRoll_.repaint();
                    }
                    AppLogger::log("Import note generation: track=" + juce::String(result.trackId)
                        + " clip=" + juce::String(resolvedClipIndex)
                        + " notes=" + juce::String(static_cast<int>(generatedNotes.size())));
                }
            }

            int voicedFrames = 0;
            for (float v : result.f0) {
                if (std::isfinite(v) && v > 0.0f) ++voicedFrames;
            }
            const float voicedRatio = result.f0.empty() ? 0.0f
                : static_cast<float>(voicedFrames) / static_cast<float>(result.f0.size());

            DBG("F0 extraction completed model=" + juce::String(result.modelName)
                + " track=" + juce::String(result.trackId)
                + " clip=" + juce::String(resolvedClipIndex)
                + " frames=" + juce::String(static_cast<int>(result.f0.size()))
                + " voicedRatio=" + juce::String(voicedRatio, 3));
            (void)voicedRatio;
        }
    );

    if (submitResult == F0ExtractionService::SubmitResult::AlreadyInProgress) {
        if (clipId != 0) {
            processorRef_.setClipOriginalF0StateById(trackId, clipId, OriginalF0State::Extracting);
        } else {
            processorRef_.setClipOriginalF0State(trackId, clipIndex, OriginalF0State::Extracting);
        }
        return;
    }

    if (submitResult != F0ExtractionService::SubmitResult::Accepted) {
        if (clipId != 0) {
            processorRef_.setClipOriginalF0StateById(trackId, clipId, OriginalF0State::Failed);
        } else {
            processorRef_.setClipOriginalF0State(trackId, clipIndex, OriginalF0State::Failed);
        }
        DBG("F0 extraction request rejected key=" + juce::String(static_cast<juce::int64>(requestKey)));
        return;
    }

    // RMVPE 提取 overlay latch（仅 Accepted 才进入）
    if (clipId != 0) {
        rmvpeOverlayLatched_ = true;
        rmvpeOverlayTargetTrackId_ = trackId;
        rmvpeOverlayTargetClipId_ = clipId;
    }
}

// ============================================================================
// PianoRollComponent::Listener Implementation
// ============================================================================

void OpenTuneAudioProcessorEditor::playheadPositionChangeRequested(double timeSeconds)
{
    processorRef_.setPosition(timeSeconds);
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

void OpenTuneAudioProcessorEditor::playFromPositionRequested(double timeSeconds)
{
    processorRef_.setPosition(timeSeconds);
    playRequested();
}

void OpenTuneAudioProcessorEditor::playFromStartToggleRequested()
{
    if (processorRef_.isPlaying()) {
        processorRef_.setPlaying(false);
        double startPos = processorRef_.getPlayStartPosition();
        processorRef_.setPosition(startPos);
        transportBar_.setPlaying(false);
        pianoRoll_.setIsPlaying(false);
        arrangementView_.setIsPlaying(false);
    } else {
        double startPos = processorRef_.getPlayStartPosition();
        processorRef_.setPosition(startPos);
        processorRef_.setPlaying(true);
        transportBar_.setPlaying(true);
        pianoRoll_.setIsPlaying(true);
        arrangementView_.setIsPlaying(true);
    }
}

void OpenTuneAudioProcessorEditor::autoTuneRequested()
{
    // 合并重复逻辑：调用统一 helper
    startAutoTuneAsUnifiedEdit();
    pianoRoll_.setCurrentTool(ToolId::Select);
    parameterPanel_.setActiveTool(1);
}

void OpenTuneAudioProcessorEditor::pitchCurveEdited(int startFrame, int endFrame)
{
    DBG("Editor: Pitch curve edited frames " + juce::String(startFrame) + " to " + juce::String(endFrame));
    
    int trackId = processorRef_.getActiveTrackId();
    int clipIndex = processorRef_.getSelectedClip(trackId);
    
    if (clipIndex < 0) return;

    auto curve = processorRef_.getClipPitchCurve(trackId, clipIndex);
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
        + " clip=" + juce::String(clipIndex)
        + " frameRange=[" + juce::String(startFrame) + "," + juce::String(endFrame) + "]"
        + " secRange=[" + juce::String(editStartSec, 3) + "," + juce::String(editEndSec, 3) + "]");

    processorRef_.enqueuePartialRender(trackId, clipIndex, editStartSec, editEndSec);
}

void OpenTuneAudioProcessorEditor::trackTimeOffsetChanged(int trackId, double newOffset)
{
    juce::ignoreUnused(trackId, newOffset);
    // Update track offset in processor
    // NOTE: For now we just update it in UI, processor logic to be added
    // processorRef_.setTrackTimeOffset(trackId, newOffset);
}

void OpenTuneAudioProcessorEditor::escapeKeyPressed()
{
    // ESC 等价于点击视图切换键：左右视图互切并同步按钮状态
    const bool targetWorkspaceView = !isWorkspaceView_;
    transportBar_.setWorkspaceView(targetWorkspaceView);
    viewToggled(targetWorkspaceView);
}

void OpenTuneAudioProcessorEditor::audioSettingsRequested()
{
    processorRef_.showAudioSettingsDialog(*this);
}

// AUTO 启动统一 helper：合并 toolSelected(AutoTune) 与 autoTuneRequested() 的重复逻辑
void OpenTuneAudioProcessorEditor::startAutoTuneAsUnifiedEdit()
{
    const int trackId = processorRef_.getActiveTrackId();
    const int clipIndex = processorRef_.getSelectedClip(trackId);
    if (trackId < 0 || clipIndex < 0) {
        return;
    }

    const auto f0State = processorRef_.getClipOriginalF0State(trackId, clipIndex);

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

    // 尝试启动 AUTO 处理
    bool success = pianoRoll_.applyAutoTuneToSelection();
    if (success) {
        autoOverlayLatched_ = true;
        autoOverlayTargetTrackId_ = trackId;
        autoOverlayTargetClipId_ = processorRef_.getClipId(trackId, clipIndex);
        autoRenderOverlay_.setMessageText(juce::String::fromUTF8("正在渲染中"));
        autoRenderOverlay_.setVisible(true);
    }
}

} // namespace OpenTune
