#include "PluginEditor.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "UI/UIColors.h"
#include "UI/UiAssets.h"
#include "UI/FrameScheduler.h"
#include "Editor/Preferences/SharedPreferencePages.h"
#include "Editor/Preferences/StandalonePreferencePages.h"
#include "Editor/Preferences/TabbedPreferencesDialog.h"
#include "Audio/AudioFormatRegistry.h"
#include "Audio/AsyncAudioLoader.h"
#include "StandaloneArrangementHelpers.h"
#include "Utils/ProjectSession.h"
#include "Utils/PitchCurve.h"
#include "Utils/LegacyNoteGenerator.h"
#include "Utils/PitchControlConfig.h"
#include "Utils/AppLogger.h"
#include "Utils/ParameterPanelSync.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/PitchShiftSettings.h"
#include "Utils/PitchShiftEditAction.h"
#include "Editor/PitchShiftDialogContent.h"
#include "Editor/ConfirmDialogContent.h"
#include "Utils/TimeCoordinate.h"
#include "Content/StandaloneClipContent.h"
#include "Runtime/ProcessF0Runtime.h"
#include "Utils/KeyShortcutConfig.h"
#include "DSP/ReferenceFeatures.h"
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <set>
#include <algorithm>
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


ContentTimelineProjection makePianoRollProjection(const StandaloneArrangement::Placement& placement,
                                                         OpenTuneAudioProcessor& processor)
{
    ContentTimelineProjection projection;
    projection.timelineStartSeconds = placement.timelineStartSeconds;
    projection.timelineDurationSeconds = placement.durationSeconds;
    auto snap = processor.getContentSnapshot(placement.contentKey);
    projection.contentDurationSeconds =
        snap ? snap->sourceWindow.durationSeconds() : placement.durationSeconds;
    return projection;
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
        NoteGeneratorParams params;
        params.policy.transitionThresholdCents = 512.0f;
        params.policy.minDurationMs = 100.0f;
        auto notes = LegacyNoteGenerator::generate(f0, energy, kHopSize, kF0SampleRate, params);
        if (notes.empty()) {
            return false;
        }
        // LegacyNoteGenerator extends the tail by whole-frame steps derived from tailExtendMs.
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

DetectedKey OpenTuneAudioProcessorEditor::resolveScaleForPlacementContent(int trackId,
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

    const ContentKey contentKey = hasPlacement ? getStandaloneContentKey(processorRef_, trackId, placementIndex) : ContentKey{};
    if (contentKey.isValid()) {
        auto snap = processorRef_.getContentSnapshot(contentKey);
        const DetectedKey detectedKey = snap ? snap->detectedKey : DetectedKey{};
        if (detectedKey.confidence > 0.0f) {
            if (sourceOut) *sourceOut = "content";
            return detectedKey;
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

void OpenTuneAudioProcessorEditor::applyResolvedScaleForPlacementContent(int trackId, int placementIndex)
{
    juce::String source;
    const DetectedKey key = resolveScaleForPlacementContent(trackId, placementIndex, &source);
    const int rootNote = static_cast<int>(key.root);
    const int scaleType = scaleToUiScaleType(key.scale);
    applyScaleToUi(rootNote, scaleType);

    const ContentKey contentKey = (trackId >= 0 && placementIndex >= 0)
        ? getStandaloneContentKey(processorRef_, trackId, placementIndex)
        : ContentKey{};
    juce::ignoreUnused(contentKey);
    DBG("ScaleSyncTrace: source=" + source
        + " trackId=" + juce::String(trackId)
        + " placementIndex=" + juce::String(placementIndex)
        + " contentKey=" + juce::String(static_cast<juce::int64>(contentKey.objectId))
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
    , projectSession_(p, appPreferences_)
{
    // Wire AppPreferences to processor for getSnapSettings()
    processorRef_.setAppPreferences(&appPreferences_);

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
// menuName obtained at runtime (auto-reflects after language switch), matched by getMenuForIndex index
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

    // Initialize Scale (content > recent > default)
    {
        const int initTrack = getStandaloneActiveTrack(processorRef_);
        const int initPlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, initTrack);
        applyResolvedScaleForPlacementContent(initTrack, initPlacementIndex);
    }

    addAndMakeVisible(topBar_);

// Top bar: side panel collapse toggle
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
// Initialize track heights (synced with ArrangementView)
    trackPanel_.setTrackHeight(processorRef_.getTrackHeight());
// Initialize state for all 32 tracks
    for (int i = 0; i < MAX_TRACKS; ++i)
    {
        trackPanel_.setTrackMuted(i, getStandaloneTrackMuted(processorRef_, i));
        trackPanel_.setTrackSolo(i, getStandaloneTrackSolo(processorRef_, i));
        trackPanel_.setTrackVolume(i, getStandaloneTrackVolume(processorRef_, i));
    }
    // Initialize track colors
    syncTrackColorsToPanel();
    trackPanel_.setTrackColorMode(appPreferences_.getTrackColorMode());
    menuBar_.setTrackColorMode(appPreferences_.getTrackColorMode());
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
    // Initial sync: track panel visible track count 鈫?arrangement view
    arrangementView_.setVisibleTrackCount(trackPanel_.getVisibleTrackCount());

    // Setup Piano Roll (main editor area)
    pianoRoll_.addListener(this);
    pianoRoll_.setProcessor(&processorRef_);
    pianoRoll_.setReadContentSnapshot([this](ContentKey key) {
        return processorRef_.getContentSnapshot(key);
    });
    pianoRoll_.setContentCommands(processorRef_.getContentCommands());
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
    
// Set high-performance playhead position source - read directly from Processor, bypassing 60Hz Timer bottleneck
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

// Apply persisted rendering priority to detector before first inference service init
    if (appPreferences_.getState().shared.renderingPriority == RenderingPriority::CpuFirst) {
        processorRef_.resetInferenceBackend(true);
    }

// Enable native title bar (system-style maximize/minimize/close buttons)
    juce::Timer::callAfterDelay(60, [safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this)]
    {
        if (safeThis == nullptr) return;
        if (auto* window = safeThis->findParentComponentOfClass<juce::DocumentWindow>())
        {
// Use native title bar for standard maximize button
            window->setUsingNativeTitleBar(true);
            window->setColour(juce::DocumentWindow::backgroundColourId, UIColors::backgroundMedium);
            window->repaint();
        }
    });

// Playhead render via VBlank overlay; main editor heartbeat reduced to 30Hz to ease message thread pressure
    startTimerHz(kHeartbeatHzIdle);

// Playhead render via VBlank overlay; main editor heartbeat reduced to 30Hz

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

// Apply persisted vocoder model weight bias at startup
    const auto weight = appPreferences_.getState().shared.vocoderModelWeight;
    processorRef_.setVocoderModelWeight(weight);
// Apply persisted vocoder model weight bias at startup
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

    // Safely join save worker thread if it exists
    if (saveWorker_.joinable())
    {
        saveWorker_.join();
    }

    // Safely join open worker thread if it exists
    if (openWorker_.joinable())
    {
        openWorker_.join();
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
    if (isImportInProgress_)
    {
        ConfirmDialogContent::showMessage(
            this,
            juce::String("Import Audio"),
            juce::String("Audio import is already in progress. Please try again later.")
        );
        return;
    }

    clearImportDropPreview();

    if (files.isEmpty())
        return;

    const juce::File file(files[0]);
    if (!file.existsAsFile())
        return;

    static const juce::String kImportExtensionSpec = getImportExtensionSpec();
    if (!file.hasFileExtension(kImportExtensionSpec))
    {
        const auto wildcard = getImportWildcardFilter().replaceCharacters("*", "");
        ConfirmDialogContent::showMessage(
            this,
            juce::String("Import Audio"),
            juce::String("Unsupported file type.\nSupported extensions: ") + wildcard
        );
        return;
    }

    if (files.size() > 1)
    {
        ConfirmDialogContent::showMessage(
            this,
            juce::String("Import Audio"),
            juce::String("Multiple files detected. Only the first file will be imported.")
        );
    }

    // Resolve import target from drop position (x,y)
    const ImportDropTarget target = resolveImportDropTarget(x, y);
    applyImportDropTarget(target, file);
}

// ============================================================================
// File Drag Hover Preview (fileDragEnter / fileDragMove / fileDragExit)
// ============================================================================

void OpenTuneAudioProcessorEditor::fileDragEnter(const juce::StringArray& files, int x, int y)
{
    juce::ignoreUnused(files);
    updateImportDropPreview(resolveImportDropTarget(x, y));
}

void OpenTuneAudioProcessorEditor::fileDragMove(const juce::StringArray& files, int x, int y)
{
    juce::ignoreUnused(files);
    updateImportDropPreview(resolveImportDropTarget(x, y));
}

void OpenTuneAudioProcessorEditor::fileDragExit(const juce::StringArray& files)
{
    juce::ignoreUnused(files);
    clearImportDropPreview();
}

void OpenTuneAudioProcessorEditor::updateImportDropPreview(ImportDropTarget target)
{
    ImportDropPreview preview;
    preview.active = true;
    preview.visibleTrackCount = trackPanel_.getVisibleTrackCount();
    preview.trackHeight = processorRef_.getTrackHeight();

    switch (target.kind)
    {
    case ImportDropTarget::Kind::ExistingTrack:
        preview.targetTrackId = target.trackId;
        break;

    case ImportDropTarget::Kind::NewTrack:
        preview.isNewTrack = true;
        break;

    case ImportDropTarget::Kind::FallbackActiveTrack:
    case ImportDropTarget::Kind::Reject:
        preview.active = false;
        break;
    }

    arrangementView_.setImportDropPreview(preview);
}

void OpenTuneAudioProcessorEditor::clearImportDropPreview()
{
    arrangementView_.clearImportDropPreview();
}

// ============================================================================
// Import Drop Target Resolver
// ============================================================================

ImportDropTarget OpenTuneAudioProcessorEditor::resolveImportDropTarget(int globalX, int globalY) const
{
    ImportDropTarget result;

    // Convert global (PluginEditor) coordinates to ArrangementViewComponent local coordinates
    const juce::Point<int> localPt = arrangementView_.getLocalPoint(this, juce::Point<int>(globalX, globalY));

    // Check if the drop point falls within the ArrangementView bounds
    const bool isInsideArrangement = arrangementView_.getLocalBounds().contains(localPt);

    if (!isInsideArrangement)
    {
        // Non-Arrangement drop 鈫?fallback to active track
        result.kind = ImportDropTarget::Kind::FallbackActiveTrack;
        result.trackId = getStandaloneActiveTrack(processorRef_);
        result.timelineStartSeconds = computeTrackAppendStartSeconds(result.trackId);
        return result;
    }

    if (localPt.y < arrangementView_.getRulerHeight())
    {
        result.kind = ImportDropTarget::Kind::FallbackActiveTrack;
        result.trackId = getStandaloneActiveTrack(processorRef_);
        result.timelineStartSeconds = computeTrackAppendStartSeconds(result.trackId);
        return result;
    }

    // Resolve the track from Y coordinate
    const int resolvedTrackId = arrangementView_.trackIdForViewportY(localPt.y);
    const int visibleTracks = trackPanel_.getVisibleTrackCount();

    if (resolvedTrackId < visibleTracks)
    {
        // Drop landed on a visible track lane
        result.kind = ImportDropTarget::Kind::ExistingTrack;
        result.trackId = resolvedTrackId;
        result.timelineStartSeconds = arrangementView_.viewportXToAbsoluteTime(localPt.x);
        if (result.timelineStartSeconds < 0.0)
            result.timelineStartSeconds = 0.0;
        return result;
    }

    // Drop landed below the last visible track 鈫?blank area
    if (visibleTracks >= OpenTuneAudioProcessor::MAX_TRACKS)
    {
        // Already at MAX_TRACKS 鈥?reject with a direct message
        result.kind = ImportDropTarget::Kind::Reject;
        result.rejectReason = juce::String("Maximum track count reached (")
                              + juce::String(OpenTuneAudioProcessor::MAX_TRACKS)
                              + juce::String("). Cannot create more tracks.");
        return result;
    }

    // Blank area 鈫?create a new visible track
    result.kind = ImportDropTarget::Kind::NewTrack;
    result.trackId = visibleTracks;  // new track will be at this index (0-based)
    result.timelineStartSeconds = juce::jmax(0.0, arrangementView_.viewportXToAbsoluteTime(localPt.x));
    return result;
}

// ============================================================================
// Apply Import Drop Target
// ============================================================================

void OpenTuneAudioProcessorEditor::applyImportDropTarget(ImportDropTarget target, const juce::File& file)
{
    switch (target.kind)
    {
    case ImportDropTarget::Kind::ExistingTrack:
        importAudioFileToTrack(target.trackId, file, target.timelineStartSeconds);
        break;

    case ImportDropTarget::Kind::NewTrack:
    {
        // Create one new visible track and import there
        trackPanel_.showMoreTracks();
        arrangementView_.setVisibleTrackCount(trackPanel_.getVisibleTrackCount());
        const int newTrackId = trackPanel_.getVisibleTrackCount() - 1;
        importAudioFileToTrack(newTrackId, file, target.timelineStartSeconds);
        break;
    }

    case ImportDropTarget::Kind::FallbackActiveTrack:
        importAudioFileToTrack(target.trackId, file, target.timelineStartSeconds);
        break;

    case ImportDropTarget::Kind::Reject:
        ConfirmDialogContent::showMessage(
            this,
            juce::String::fromUTF8(u8"\u5BFC\u5165\u97F3\u9891"),
            target.rejectReason
        );
        break;
    }
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

void OpenTuneAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    rippleOverlay_.setBounds(bounds);
    rippleOverlay_.toFront(false);

// Shadow margin: reserve space for panel shadow rendering
// Each component paint() uses reduced(shadowMargin) for background; shadow renders in margin
    const int shadowMargin = 12;
    const int gap = 6;  // Gap between panels (瑙嗚闂磋窛锛屼笉鍚槾褰?

    bounds.reduce(gap, gap); // Global padding

// TopBar: height + shadow margin (12px top and bottom)
    const int topBarHeight = menuBar_.isVisible() ? (MENU_BAR_HEIGHT + TRANSPORT_BAR_HEIGHT) : TRANSPORT_BAR_HEIGHT;
    const int topBarHeightWithShadow = topBarHeight + shadowMargin * 2;
    topBar_.setBounds(bounds.removeFromTop(topBarHeightWithShadow));
// Visual gap: subtract bottom shadow margin already consumed
    bounds.removeFromTop(juce::jmax(0, gap - shadowMargin));

// Visual gap: subtract bottom shadow margin already consumed
// Width + shadow margin (12px left and right)
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

// Right Properties Panel (collapsible)
// Width + shadow margin (12px left and right)
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

// Center area (PianoRoll / ArrangementView)
// PianoRoll already uses reduced(12.0f) for background; bounds unchanged
    arrangementView_.setBounds(bounds);
    pianoRoll_.setBounds(bounds);
    
// AutoRenderOverlay covers entire PianoRoll area
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
        auto f0Service = ProcessF0Runtime::getInstance().getF0Service();
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
        const ContentKey activeKey = (activeTrack >= 0 && activePlacementIndex >= 0)
            ? getStandaloneContentKey(processorRef_, activeTrack, activePlacementIndex)
            : ContentKey{};
        auto snap = processorRef_.getContentSnapshot(activeKey);
        auto curve = snap ? snap->pitchCurve : nullptr;
        std::shared_ptr<const juce::AudioBuffer<float>> contentBuffer =
            snap ? snap->audioBuffer : nullptr;
        const uint64_t currentNotesRevision = activeKey.isValid() && snap
            ? snap->notesRevision
            : 0;
        const bool contentChanged =
            activeKey != lastPianoRollContentKey_
            || sr != lastPianoRollSampleRate_
            || curve != lastPianoRollCurve_
            || contentBuffer != lastPianoRollBuffer_;
        if (contentChanged) {
            pianoRoll_.setEditedContent(activeKey, curve, contentBuffer, sr);
            lastPianoRollContentKey_ = activeKey;
            lastPianoRollSampleRate_ = sr;
            lastPianoRollCurve_ = curve;
            lastPianoRollBuffer_ = contentBuffer;
        } else if (currentNotesRevision != lastPianoRollNotesRevision_) {
            // Same content, fresh notes – typically GAME's async commit.
            pianoRoll_.refreshEditedContentNotes();
            pianoRoll_.requestContentRedraw();
        }
        if (activeTrack >= 0 && activePlacementIndex >= 0) {
            const DetectedKey resolvedKey =
                resolveScaleForPlacementContent(activeTrack, activePlacementIndex, nullptr);
            const int resolvedRootNote = static_cast<int>(resolvedKey.root);
            const int resolvedScaleType = scaleToUiScaleType(resolvedKey.scale);
            if (resolvedRootNote != lastScaleRootNote_ || resolvedScaleType != lastScaleType_) {
                applyResolvedScaleForPlacementContent(activeTrack, activePlacementIndex);
            }
        }

        lastPianoRollNotesRevision_ = currentNotesRevision;
    }

// Playhead position read by each component via positionSource_ directly from Processor
    transportBar_.setPositionSeconds(currentPositionSeconds);

    const RenderStatusSnapshot statusSnapshot = getRenderStatusSnapshot();

    // RMVPE overlay：与 vocoder 无关，独立于渲染状态
    if (rmvpeOverlayLatched_ && !isWorkspaceView_) {
        const ContentKey targetContentKey = rmvpeOverlayTargetContentKey_;

        bool shouldUnlatch = false;
        if (!targetContentKey.isValid()) {
            shouldUnlatch = true;
        } else {
            auto snap = processorRef_.getContentSnapshot(targetContentKey);
            const auto f0State = snap ? snap->originalF0State : OriginalF0State::NotRequested;
            const bool f0Done = (f0State == OriginalF0State::Ready
                                  || f0State == OriginalF0State::Failed);
            const bool noteGenBusy = processorRef_.isNoteGenInFlightForContent(targetContentKey);
            // Only unlatch when BOTH F0 and note generation are finished —
            // shared "正在处理音高" overlay covers the whole import pipeline.
            if (f0Done && !noteGenBusy) {
                shouldUnlatch = true;
            }
        }

        if (shouldUnlatch) {
            rmvpeOverlayLatched_ = false;
            rmvpeOverlayTargetContentKey_ = ContentKey{};
        }
    }

    bool shouldShowOverlay = false;

    if (rmvpeOverlayLatched_ && !isWorkspaceView_) {
        autoRenderOverlay_.setMessageText(juce::String::fromUTF8(u8"\u6B63\u5728\u5904\u7406\u97F3\u9891"));
        shouldShowOverlay = true;
    }

    // Reference feature (timing anchor) extraction overlay —
    // shares the same overlay system as RMVPE extraction.
    if (!shouldShowOverlay && !isWorkspaceView_) {
        const int activeTrack = getStandaloneActiveTrack(processorRef_);
        const int activePlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
        const ContentKey activeContentKey = (activeTrack >= 0 && activePlacementIndex >= 0)
            ? getStandaloneContentKey(processorRef_, activeTrack, activePlacementIndex)
            : ContentKey{};
        if (activeContentKey.isValid()) {
            // Check if GAME timing anchor extraction is in flight
            const ReferenceFeatureSet refFeatures = processorRef_.getReferenceFeatures(activeContentKey);
            if (refFeatures.status == ReferenceFeatureStatus::Extracting) {
                autoRenderOverlay_.setMessageText(juce::String::fromUTF8("正在提取节奏锚点"));
                shouldShowOverlay = true;
            }
        }
    }

    // ============================================================================
    // Render badge logic — Stage 2 (time-stretch) is now synchronous inside CRS,
    // so there is no async "in-flight" state to display. Badge reflects Stage 1 only.
    // ============================================================================
    const bool isAutoProcessing = pianoRoll_.isAutoTuneProcessing();

    // Stage 1 — meaningful only when vocoder loaded
    bool stage1HasWork = false;
    int  stage1Done = 0;
    int  stage1Total = 0;
    if (vocoderDomain != nullptr) {
        const int activeTrack = getStandaloneActiveTrack(processorRef_);
        const int activePlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);

        const ContentKey activeContentKey = (activeTrack >= 0 && activePlacementIndex >= 0)
            ? getStandaloneContentKey(processorRef_, activeTrack, activePlacementIndex)
            : ContentKey{};
        auto rc = processorRef_.getContentRenderService()->getRenderCache(activeContentKey);
        const auto chunkStats = rc ? rc->getChunkStats() : RenderCache::ChunkStats{};
        stage1HasWork = chunkStats.hasActiveWork();
        stage1Done    = chunkStats.idle + chunkStats.blank;
        stage1Total   = chunkStats.total();

        if (isAutoProcessing) {
            const float olProgress = (stage1Total > 0)
                ? static_cast<float>(stage1Done) / static_cast<float>(stage1Total) : 0.0f;
            autoRenderOverlay_.setMessageText(buildRenderingOverlayTitle(stage1Done, stage1Total, olProgress));
            shouldShowOverlay = true;
        }
    }

    // Combined badge visibility — Stage 1 only (Stage 2 is synchronous in CRS).
    const bool shouldShowBadge = stage1HasWork && !isAutoProcessing;
    if (shouldShowBadge) {
        renderBadge_.setMessageText(juce::String::fromUTF8(u8"\u6e32\u67d3\u4e2d (")
            + juce::String(stage1Done) + "/" + juce::String(stage1Total) + ")");
    }
    if (renderBadge_.isVisible() != shouldShowBadge) {
        renderBadge_.setVisible(shouldShowBadge);
    }
    transportBar_.setRenderStatusText(juce::String());

    if (autoRenderOverlay_.isVisible() != shouldShowOverlay) {
        autoRenderOverlay_.setVisible(shouldShowOverlay);
    }

#if JUCE_DEBUG
    if (++diagnosticHeartbeatCounter_ >= 300) {
        diagnosticHeartbeatCounter_ = 0;
        const auto diagnosticInfo = processorRef_.getDiagnosticInfo(getStandaloneActiveTrack(processorRef_), statusSnapshot.placementId);
        AppLogger::log("StandaloneEditor: render status=" + renderStatusToString(statusSnapshot.status)
            + " contentKey=" + juce::String(static_cast<juce::int64>(diagnosticInfo.contentKey.objectId))
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
        FrameScheduler::instance().setTimelinePlaybackActive(processorRef_.isPlaying());
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
    const bool experimentalFeaturesEnabled = sharedPreferences.experimentalFeaturesEnabled;

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
    pianoRoll_.setExperimentalFeaturesEnabled(experimentalFeaturesEnabled);
    pianoRoll_.setZoomSensitivity(sharedPreferences.zoomSensitivity);
    pianoRoll_.setNoteNameMode(visualPreferences.noteNameMode);
    pianoRoll_.setShowChunkBoundaries(visualPreferences.showChunkBoundaries);
    pianoRoll_.setShowUnvoicedFrames(visualPreferences.showUnvoicedFrames);
    parameterPanel_.setExperimentalFeaturesEnabled(experimentalFeaturesEnabled);
    arrangementView_.setZoomSensitivity(sharedPreferences.zoomSensitivity);
    arrangementView_.setExperimentalReferenceControlsEnabled(experimentalFeaturesEnabled);
    menuBar_.setNoteNameMode(visualPreferences.noteNameMode);
    menuBar_.setShowChunkBoundaries(visualPreferences.showChunkBoundaries);
    menuBar_.setShowUnvoicedFrames(visualPreferences.showUnvoicedFrames);

    shortcutSettings_ = preferencesState.shared.shortcuts;
    pianoRoll_.setShortcutSettings(shortcutSettings_);
    arrangementView_.setShortcutSettings(shortcutSettings_);
    menuBar_.setMouseTrailTheme(preferencesState.standalone.mouseTrailTheme);
    rippleOverlay_.setTrailTheme(preferencesState.standalone.mouseTrailTheme);
    menuBar_.setTrackColorMode(sharedPreferences.trackColorMode);
    trackPanel_.setTrackColorMode(sharedPreferences.trackColorMode);
}

void OpenTuneAudioProcessorEditor::syncTrackColorsToPanel()
{
    for (int i = 0; i < MAX_TRACKS; ++i)
        trackPanel_.setTrackColour(i, getStandaloneTrackColour(processorRef_, i));
}

RenderStatusSnapshot OpenTuneAudioProcessorEditor::getRenderStatusSnapshot() const
{
    const int trackId = getStandaloneActiveTrack(processorRef_);
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    const uint64_t placementId = (placementIndex >= 0) ? processorRef_.getPlacementId(trackId, placementIndex) : 0;
    const ContentKey contentKey = getStandaloneContentKey(processorRef_, trackId, placementIndex);

    RenderStatusSnapshot snapshot;
    snapshot.contentKey = contentKey;
    snapshot.placementId = placementId;
    if (!contentKey.isValid()) {
        return snapshot;
    }

    auto renderCache = processorRef_.getContentRenderService()->getRenderCache(contentKey);
    if (renderCache == nullptr) {
        snapshot.contentKey = ContentKey{};
        snapshot.placementId = 0;
        return snapshot;
    }

    return makeRenderStatusSnapshot(contentKey, placementId, renderCache->getStateSnapshot());
}

void OpenTuneAudioProcessorEditor::syncPianoRollFromPlacementSelection(int trackId, int placementIndex)
{
    StandaloneArrangement::Placement placement;
    const bool hasPlacement = (placementIndex >= 0)
        && getStandalonePlacementByIndex(processorRef_, trackId, placementIndex, placement);
    const ContentKey contentKey = hasPlacement ? placement.contentKey : ContentKey{};

    pianoRoll_.setContentProjection(hasPlacement ? makePianoRollProjection(placement, processorRef_)
                                                  : ContentTimelineProjection{});

    const int sr = static_cast<int>(processorRef_.getSampleRate());
    auto snap = processorRef_.getContentSnapshot(contentKey);
    std::shared_ptr<const juce::AudioBuffer<float>> contentBuffer =
        snap ? snap->audioBuffer : nullptr;
    auto curve = snap ? snap->pitchCurve : nullptr;
    pianoRoll_.setEditedContent(contentKey, curve, contentBuffer, sr);

    lastPianoRollContentKey_ = contentKey;
    lastPianoRollSampleRate_ = sr;
    lastPianoRollCurve_ = curve;
    lastPianoRollBuffer_ = contentBuffer;

    applyResolvedScaleForPlacementContent(trackId, placementIndex);

}

void OpenTuneAudioProcessorEditor::applyPlacementSelectionContext(int trackId, uint64_t placementId)
{
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS)
    {
        pianoRoll_.setContentProjection({});
        pianoRoll_.setEditedContent(ContentKey{}, nullptr, nullptr, static_cast<int>(processorRef_.getSampleRate()));
        lastPianoRollContentKey_ = ContentKey{};
        lastPianoRollCurve_.reset();
        lastPianoRollBuffer_.reset();
        return;
    }

    setStandaloneActiveTrack(processorRef_, trackId);
    trackPanel_.setActiveTrack(trackId);

    if (placementId == 0)
    {
        setStandaloneSelectedPlacementIndex(processorRef_, trackId, -1);
        pianoRoll_.setContentProjection({});
        pianoRoll_.setEditedContent(ContentKey{}, nullptr, nullptr, static_cast<int>(processorRef_.getSampleRate()));
        lastPianoRollContentKey_ = ContentKey{};
        lastPianoRollCurve_.reset();
        lastPianoRollBuffer_.reset();
        return;
    }

    const int placementIndex = processorRef_.findPlacementIndexById(trackId, placementId);
    if (placementIndex < 0)
    {
        setStandaloneSelectedPlacementIndex(processorRef_, trackId, -1);
        pianoRoll_.setContentProjection({});
        pianoRoll_.setEditedContent(ContentKey{}, nullptr, nullptr, static_cast<int>(processorRef_.getSampleRate()));
        lastPianoRollContentKey_ = ContentKey{};
        lastPianoRollCurve_.reset();
        lastPianoRollBuffer_.reset();
        return;
    }

    setStandaloneSelectedPlacementIndex(processorRef_, trackId, placementIndex);
    syncPianoRollFromPlacementSelection(trackId, placementIndex);
}

void OpenTuneAudioProcessorEditor::toolSelected(int toolId)
{
    if (toolId < 0 || toolId > static_cast<int>(ToolId::TimeTool)) {
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

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::vibratoDepthChanged(float value)
{
    if (pianoRoll_.applyVibratoDepthToSelection(value)) return;
    pianoRoll_.setVibratoDepth(value);
    const float speed = parameterPanel_.getRetuneSpeed() / 100.0f;
    const float rate = parameterPanel_.getVibratoRate();
    pianoRoll_.applyCorrectionAsyncForEntireClip(speed, value, rate);

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::vibratoRateChanged(float value)
{
    if (pianoRoll_.applyVibratoRateToSelection(value)) return;
    pianoRoll_.setVibratoRate(value);
    const float speed = parameterPanel_.getRetuneSpeed() / 100.0f;
    const float depth = parameterPanel_.getVibratoDepth();
    pianoRoll_.applyCorrectionAsyncForEntireClip(speed, depth, value);

    projectSession_.markDirty();
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

// Import mode enumeration
enum class ImportMode
{
    SameTrack,      // 按顺序导入到同一个轨道
    SeparateTracks  // 分别导入到多个轨道（齐头）
};

void OpenTuneAudioProcessorEditor::importAudioRequested()
{
// Direct file chooser dialog with multi-select support
// Import destination track determined by currently selected track
    DBG("OpenTuneAudioProcessorEditor::importAudioRequested called");

    if (isImportInProgress_)
    {
        ConfirmDialogContent::showMessage(
            this,
            juce::String("Import Audio"),
            juce::String("Audio import is already in progress. Please try again later.")
        );
        return;
    }

    const auto wildcardFilter = getImportWildcardFilter();
    auto chooser = std::make_shared<juce::FileChooser>(
        juce::String::fromUTF8(u8"\u9009\u62E9\u8981\u5BFC\u5165\u7684\u97F3\u9891\u6587\u4EF6"),
        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        wildcardFilter
    );

// Support multi-select
    auto chooserFlags = juce::FileBrowserComponent::openMode 
                      | juce::FileBrowserComponent::canSelectFiles 
                      | juce::FileBrowserComponent::canSelectMultipleItems;

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    chooser->launchAsync(chooserFlags, [safeThis, chooser, this](const juce::FileChooser& fc)
    {
        if (safeThis == nullptr)
            return;

        const juce::Array<juce::File>& selectedFiles = fc.getResults();
        
        if (selectedFiles.isEmpty())
        {
            DBG("No files selected");
            return;
        }

// Get currently selected track
        int currentTrack = getStandaloneActiveTrack(safeThis->processorRef_);
        int visibleTracks = safeThis->trackPanel_.getVisibleTrackCount();

        if (selectedFiles.size() == 1)
        {
// Single file: import directly to currently selected track
            safeThis->importAudioFileToTrack(currentTrack, selectedFiles[0]);
        }
        else
        {
// Multiple files: prompt for import mode
            auto filesPtr = std::make_shared<juce::Array<juce::File>>(selectedFiles);

            ConfirmDialogContent::launch(
                new ConfirmDialogContent(
                    juce::String("Choose Import Mode"),
                    juce::String("You selected ") + juce::String(selectedFiles.size()) + juce::String(" audio files. Choose an import mode."),
                    {
                        { juce::String("Import Sequentially To Current Track"), [=]() {
                            if (safeThis == nullptr)
                                return;

// Sequential import into same track (currently selected)
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
                        }, true },
                        { juce::String("Import To Separate Tracks"), [=]() {
                            if (safeThis == nullptr)
                                return;

// Aligned import across multiple tracks
                            const int remainingTrackCapacity = juce::jmax(0, OpenTuneAudioProcessor::MAX_TRACKS - currentTrack);
                            const int acceptedFileCount = juce::jmin(filesPtr->size(), remainingTrackCapacity);
                            if (acceptedFileCount <= 0)
                            {
                                ConfirmDialogContent::showMessage(
                                    safeThis,
                                    juce::String("Import Failed"),
                                    juce::String("There are no available tracks after the current track.")
                                );
                                return;
                            }

// Auto-expand visible track count
                            int requiredTracks = currentTrack + acceptedFileCount;
                            if (requiredTracks > visibleTracks)
                            {
                                int newVisibleTracks = std::min(requiredTracks, OpenTuneAudioProcessor::MAX_TRACKS);
                                safeThis->trackPanel_.setVisibleTrackCount(newVisibleTracks);
                                safeThis->arrangementView_.setVisibleTrackCount(newVisibleTracks);
                            }

// Starting from current track, import into subsequent tracks in order
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
                                ConfirmDialogContent::showMessage(
                                    safeThis,
                                    juce::String("Import Count Trimmed"),
                                    juce::String("Only ")
                                        + juce::String(acceptedFileCount)
                                        + juce::String(" tracks are available after the current track. Extra files were not queued.")
                                );
                            }
                        } },
                        { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr }
                    }
                ),
                this
            );
        }
    });
}

void OpenTuneAudioProcessorEditor::importAudioFileToTrack(int trackId, const juce::File& file,
                                                            double timelineStartSeconds)
{
    OpenTuneAudioProcessor::ImportPlacement placement;
    placement.trackId = trackId;
    placement.timelineStartSeconds = (timelineStartSeconds >= 0.0)
        ? timelineStartSeconds
        : computeTrackAppendStartSeconds(trackId);

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
    const auto sourceFilePath = sourceFile.getFullPathName();

    asyncAudioLoader_.loadAudioFile(
        sourceFile,
        {},
        [safeThis, pendingImport = std::move(pendingImport), fileName, sourceFilePath](AsyncAudioLoader::LoadResult result) mutable
        {
            if (safeThis == nullptr)
                return;

            if (!result.success)
            {
                safeThis->isImportInProgress_ = false;
                safeThis->releaseImportBatchSlot(pendingImport.batchId);
                safeThis->processNextImportInQueue();
                ConfirmDialogContent::showMessage(
                    safeThis.getComponent(),
                    juce::String::fromUTF8(u8"\u5BFC\u5165\u5931\u8D25"),
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
                                              sourceFilePath,
                                              sampleRate = result.sampleRate,
                                              audioBuffer = std::move(result.audioBuffer)]() mutable
            {
                if (safeThis == nullptr)
                    return;

                OpenTuneAudioProcessor::PreparedImport preparedImport;
                {
                    if (!safeThis->processorRef_.prepareImport(std::move(audioBuffer), sampleRate, fileName, sourceFilePath, preparedImport))
                    {
                        juce::MessageManager::callAsync([safeThis, batchId = pendingImport.batchId]()
                        {
                            if (safeThis == nullptr)
                                return;
                            safeThis->isImportInProgress_ = false;
                            safeThis->releaseImportBatchSlot(batchId);
                            safeThis->processNextImportInQueue();
                            ConfirmDialogContent::showMessage(
                                safeThis.getComponent(),
                                juce::String("Import Failed"),
                                juce::String("Audio import preprocessing failed. Please try again.")
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
                        ConfirmDialogContent::showMessage(
                            safeThis.getComponent(),
                            juce::String("Import Failed"),
                            juce::String("Audio import commit failed. Please try again.")
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
                    auto importSnap = safeThis->processorRef_.getContentSnapshot(committedPlacement.contentKey);
                    auto importBuf = importSnap ? importSnap->audioBuffer : nullptr;
                    safeThis->pianoRoll_.setEditedContent(committedPlacement.contentKey,
                                                          nullptr,
                                                            importBuf,
                                                          static_cast<int>(safeThis->processorRef_.getSampleRate()));
                    safeThis->lastPianoRollContentKey_ = committedPlacement.contentKey;
                    safeThis->lastPianoRollCurve_.reset();
                    safeThis->lastPianoRollBuffer_ = importBuf;

                    OpenTuneAudioProcessor::ContentRefreshRequest refreshRequest;
                    refreshRequest.contentKey = committedPlacement.contentKey;
                    if (!safeThis->processorRef_.requestContentRefresh(refreshRequest)) {
                        AppLogger::log("ClipDerivedRefresh: standalone request rejected contentKey.objectId="
                            + juce::String(static_cast<juce::int64>(committedPlacement.contentKey.objectId)));
                    } else {
                        safeThis->rmvpeOverlayLatched_ = true;
                        safeThis->rmvpeOverlayTargetContentKey_ = committedPlacement.contentKey;
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

// Process next file in the import queue
void OpenTuneAudioProcessorEditor::processNextImportInQueue()
{
    if (importQueue_.empty())
        return;
    
// Dequeue the first pending import item
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
        if (!arrangement->getPlacementByIndex(trackId, placementIndex, placement) || !placement.contentKey.isValid()) {
            continue;
        }

        auto* clip = processorRef_.getStandaloneContentRepository()
            ? processorRef_.getStandaloneContentRepository()->findClip(placement.contentKey) : nullptr;
        const auto buffer = clip ? clip->payload().audioBuffer : nullptr;
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
        ConfirmDialogContent::showMessage(
            this,
            juce::String("Export Audio"),
            juce::String("An export task is already in progress. Please try again later."));
        return;
    }
    
// Determine default filename based on export type
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
                    ConfirmDialogContent::showMessage(
                        safeThis.getComponent(),
                        juce::String("Export Failed"),
                        juce::String("No audio clip is selected. Select a clip on the track first."));
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
                        ConfirmDialogContent::showMessage(
                            uiSafe.getComponent(),
                            juce::String::fromUTF8(u8"\u5BFC\u51FA\u5B8C\u6210"),
                            outRequest.targetName + juce::String::fromUTF8(u8" \u5DF2\u5BFC\u51FA\u5230: ") + outFile.getFullPathName());
                        return;
                    }

                    juce::String failText = juce::String::fromUTF8(u8"\u65E0\u6CD5\u5BFC\u51FA\u97F3\u9891\u5230 ") + outFile.getFullPathName();
                    if (errorText.isNotEmpty())
                    {
                        failText += juce::String::fromUTF8(u8"\n\u539F\u56E0: ") + errorText;
                    }

                    ConfirmDialogContent::showMessage(
                        uiSafe.getComponent(),
                        juce::String::fromUTF8(u8"\u5BFC\u51FA\u5931\u8D25"),
                        failText);
                });
            });
    });
}

void OpenTuneAudioProcessorEditor::saveProjectRequested()
{
    if (!projectSession_.hasProjectPath()) {
        saveProjectAsRequested();
        return;
    }
    if (saveWorker_.joinable()) saveWorker_.join();

    // Capture snapshot and paths on message thread 鈥?ProjectSession only accessed here
    auto task = projectSession_.prepareSave();
    const uint64_t gen = projectSession_.getDirtyGeneration();
    const auto path = task.targetFile;

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
    saveWorker_ = std::thread([safeThis, task = std::move(task), gen, path]() mutable {
        // Background thread: pure file I/O only
        auto result = ProjectSession::executeSaveToFile(task);
        juce::MessageManager::callAsync([safeThis, result, gen, path]() {
            if (safeThis == nullptr) return;
            if (!result.ok()) {
                ConfirmDialogContent::launch(
                    new ConfirmDialogContent(
                        juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B\u5931\u8D25"),
                        result.error().fullMessage(),
                        { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                    safeThis.getComponent());
                return;
            }
            // State changes back on message thread
            if (safeThis->projectSession_.getDirtyGeneration() == gen)
                safeThis->projectSession_.clearDirty();
            safeThis->projectSession_.pushRecentProject(path);
            safeThis->syncRecentProjectsToMenu();
            safeThis->updateTitleWithProjectPath();
        });
    });
}

void OpenTuneAudioProcessorEditor::openProjectRequested()
{
    if (!projectSession_.isDirty()) {
        launchOpenProjectChooser();
        return;
    }

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
    ConfirmDialogContent::launch(
        new ConfirmDialogContent(
            juce::String::fromUTF8(u8"\u5F53\u524D\u5DE5\u7A0B\u5C1A\u672A\u4FDD\u5B58"),
            juce::String::fromUTF8(u8"\u6253\u5F00\u5176\u4ED6\u5DE5\u7A0B\u524D\uFF0C\u662F\u5426\u4FDD\u5B58\u5F53\u524D\u5DE5\u7A0B\u7684\u66F4\u6539\uFF1F"),
            {               { juce::String::fromUTF8(u8"\u4FDD\u5B58"), [safeThis] {
                    if (safeThis == nullptr) return;
                    if (!safeThis->projectSession_.hasProjectPath()) {
                        safeThis->saveProjectAsThenOpenProject();
                        return;
                    }
                    if (safeThis->saveWorker_.joinable()) safeThis->saveWorker_.join();
                    {
                        // Capture on message thread
                        auto task = safeThis->projectSession_.prepareSave();
                        const uint64_t gen = safeThis->projectSession_.getDirtyGeneration();
                        const auto path = task.targetFile;
                        safeThis->saveWorker_ = std::thread([safeThis, task = std::move(task), gen, path]() mutable {
                            auto saveResult = ProjectSession::executeSaveToFile(task);
                            juce::MessageManager::callAsync([safeThis, saveResult, gen, path]() {
                                if (safeThis == nullptr) return;
                                if (!saveResult.ok()) {
                                    ConfirmDialogContent::launch(
                                        new ConfirmDialogContent(
                                            juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B\u5931\u8D25"),
                                            saveResult.error().fullMessage(),
                                            { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                                        safeThis.getComponent());
                                    return;
                                }
                                if (safeThis->projectSession_.getDirtyGeneration() == gen)
                                    safeThis->projectSession_.clearDirty();
                                safeThis->projectSession_.pushRecentProject(path);
                                safeThis->updateTitleWithProjectPath();
                                safeThis->syncRecentProjectsToMenu();
                                safeThis->launchOpenProjectChooser();
                            });
                        });
                    }
                }, true },
              { juce::String("Do Not Save"), [safeThis] {
                    if (safeThis == nullptr) return;
                    safeThis->launchOpenProjectChooser();
                }, false },
              { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr, false } }),
        this);
}

void OpenTuneAudioProcessorEditor::launchOpenProjectChooser()
{
    auto chooser = std::make_shared<juce::FileChooser>(juce::String::fromUTF8(u8"\u6253\u5F00\u5DE5\u7A0B"), juce::File(), "*.otproj");
    auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    chooser->launchAsync(chooserFlags, [safeThis, chooser](const juce::FileChooser& fc) {
        if (safeThis == nullptr) return;
        auto file = fc.getResult();
        if (file == juce::File{}) return;

        if (safeThis->saveWorker_.joinable()) safeThis->saveWorker_.join();
        if (safeThis->openWorker_.joinable()) safeThis->openWorker_.join();

        safeThis->openWorker_ = std::thread([safeThis, file]() {
            auto result = safeThis->projectSession_.openProject(file);

            juce::MessageManager::callAsync([safeThis, result, file]() {
                if (safeThis == nullptr) return;

                if (!result.ok()) {
                    ConfirmDialogContent::launch(
                        new ConfirmDialogContent(
                            juce::String::fromUTF8(u8"\u6253\u5F00\u5DE5\u7A0B\u5931\u8D25"),
                            result.error().fullMessage(),
                            { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                        safeThis.getComponent());
                    safeThis->projectSession_.clearRecentProjects();
                    return;
                }

                safeThis->syncRecentProjectsToMenu();
                safeThis->updateTitleWithProjectPath();
                safeThis->refreshAllUIFromProject();
            });
        });
    });
}

void OpenTuneAudioProcessorEditor::saveProjectAsThenOpenProject()
{
    auto chooser = std::make_shared<juce::FileChooser>(juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B"), juce::File(), "*.otproj");
    auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles;
    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    chooser->launchAsync(chooserFlags, [safeThis, chooser](const juce::FileChooser& fc) {
        if (safeThis == nullptr) return;
        auto file = fc.getResult();
        if (file == juce::File{}) return;
        if (!file.hasFileExtension(".otproj"))
            file = file.withFileExtension(".otproj");

        if (file.existsAsFile()) {
            ConfirmDialogContent::launch(
                new ConfirmDialogContent(
                    juce::String("Overwrite Existing Project?"),
                    juce::String("The target project file already exists. Overwrite it?"),
                    { { juce::String::fromUTF8(u8"\u8986\u76D6"), [safeThis, file] {
                            if (safeThis == nullptr) return;
                            if (safeThis->saveWorker_.joinable()) safeThis->saveWorker_.join();
                            safeThis->projectSession_.setCurrentProjectFile(file);
                            auto task = safeThis->projectSession_.prepareSave();
                            const uint64_t gen = safeThis->projectSession_.getDirtyGeneration();
                            safeThis->saveWorker_ = std::thread([safeThis, task = std::move(task), gen, file]() mutable {
                                auto result = ProjectSession::executeSaveToFile(task);
                                juce::MessageManager::callAsync([safeThis, result, gen, file]() {
                                    if (safeThis == nullptr) return;
                                    if (!result.ok()) {
                                        ConfirmDialogContent::launch(
                                            new ConfirmDialogContent(
                                                juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B\u5931\u8D25"),
                                                result.error().fullMessage(),
                                                { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                                            safeThis.getComponent());
                                        return;
                                    }
                                    if (safeThis->projectSession_.getDirtyGeneration() == gen)
                                        safeThis->projectSession_.clearDirty();
                                    safeThis->projectSession_.pushRecentProject(file);
                                    safeThis->syncRecentProjectsToMenu();
                                    safeThis->updateTitleWithProjectPath();
                                    safeThis->refreshAllUIFromProject();
                                    safeThis->launchOpenProjectChooser();
                                });
                            });
                        }, true },
                      { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr, false } }),
                safeThis.getComponent());
            return;
        }

        if (safeThis->saveWorker_.joinable()) safeThis->saveWorker_.join();
        safeThis->projectSession_.setCurrentProjectFile(file);
        auto task = safeThis->projectSession_.prepareSave();
        const uint64_t gen = safeThis->projectSession_.getDirtyGeneration();
        safeThis->saveWorker_ = std::thread([safeThis, task = std::move(task), gen, file]() mutable {
            auto result = ProjectSession::executeSaveToFile(task);
            juce::MessageManager::callAsync([safeThis, result, gen, file]() {
                if (safeThis == nullptr) return;
                if (!result.ok()) {
                    ConfirmDialogContent::launch(
                        new ConfirmDialogContent(
                            juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B\u5931\u8D25"),
                            result.error().fullMessage(),
                            { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                        safeThis.getComponent());
                    return;
                }
                if (safeThis->projectSession_.getDirtyGeneration() == gen)
                    safeThis->projectSession_.clearDirty();
                safeThis->projectSession_.pushRecentProject(file);
                safeThis->syncRecentProjectsToMenu();
                safeThis->updateTitleWithProjectPath();
                safeThis->refreshAllUIFromProject();
                safeThis->launchOpenProjectChooser();
            });
        });
    });
}

void OpenTuneAudioProcessorEditor::preferencesRequested()
{
    showPreferencesDialog();
}

void OpenTuneAudioProcessorEditor::showPreferencesDialog()
{
    auto* holder = juce::StandalonePluginHolder::getInstance();
    auto onVocoderModelWeightChanged = [this](VocoderModelWeight weight) {
        processorRef_.setVocoderModelWeight(weight);
    };
    auto pages = StandalonePreferencePages::createAudioPages(
        holder != nullptr ? &holder->deviceManager : nullptr,
        appPreferences_,
        [this] { syncSharedAppPreferences(); },
        [this](bool forceCpu) { processorRef_.resetInferenceBackend(forceCpu); },
        std::move(onVocoderModelWeightChanged));

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
        ConfirmDialogContent::showMessage(
            this,
            LOC(kClose),
            juce::String("Help file not found: ") + helpFile.getFullPathName()
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

// Sync playhead color to high-performance playhead overlay
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

void OpenTuneAudioProcessorEditor::trackColorModeChanged(TrackColorMode mode)
{
    appPreferences_.setTrackColorMode(mode);
    trackPanel_.setTrackColorMode(mode);
    menuBar_.setTrackColorMode(mode);
}

void OpenTuneAudioProcessorEditor::performUndoRedoAction(bool isUndo)
{
    if (isUndo)
        processorRef_.getUndoManager().undo();
    else
        processorRef_.getUndoManager().redo();

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::undoRequested() { performUndoRedoAction(true); }

void OpenTuneAudioProcessorEditor::redoRequested() { performUndoRedoAction(false); }

void OpenTuneAudioProcessorEditor::languageChanged(Language newLanguage)
{
    juce::ignoreUnused(newLanguage);
    
// Refresh menu bar - JUCE requires menuItemsChanged() to rebuild menu
    menuBar_.menuItemsChanged();
    menuBar_.repaint();
    
// Refresh top toolbar
    transportBar_.refreshLocalizedText();
    topBar_.refreshLocalizedText();
    
// Refresh parameter panel
    parameterPanel_.refreshLocalizedText();
    
// Refresh entire UI
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
    FrameScheduler::instance().setTimelinePlaybackActive(true);
}

void OpenTuneAudioProcessorEditor::pauseRequested()
{
    processorRef_.setPlaying(false);
    processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Pause);
    transportBar_.setPlaying(false);
    pianoRoll_.setIsPlaying(false);  // Notify PianoRoll to stop auto-scroll
    arrangementView_.setIsPlaying(false);  // Notify ArrangementView to stop overlay updates
    FrameScheduler::instance().setTimelinePlaybackActive(false);
}

void OpenTuneAudioProcessorEditor::stopRequested()
{
    processorRef_.setPlaying(false);
    processorRef_.setPosition(0);
    processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Stop);
    transportBar_.setPlaying(false);
    pianoRoll_.setIsPlaying(false);  // Notify PianoRoll to stop auto-scroll
    arrangementView_.setIsPlaying(false);  // Notify ArrangementView to stop overlay updates
    FrameScheduler::instance().setTimelinePlaybackActive(false);
}

void OpenTuneAudioProcessorEditor::loopToggled(bool enabled)
{
    processorRef_.setLoopEnabled(enabled);
}

void OpenTuneAudioProcessorEditor::bpmChanged(double newBpm)
{
    processorRef_.setBpm(newBpm);
    pianoRoll_.setBpm(newBpm);  // Update piano roll to redraw time grid

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::scaleChanged(int rootNote, int scaleType)
{
    if (suppressScaleChangedCallback_) {
        return;
    }

    const int activeTrack = getStandaloneActiveTrack(processorRef_);
    const int activePlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
    const ContentKey activeContentKey = getStandaloneContentKey(processorRef_, activeTrack, activePlacementIndex);

    const int newRoot = juce::jlimit(0, 11, rootNote);
    const int newScaleType = juce::jlimit(1, 8, scaleType);

    const DetectedKey oldResolved = resolveScaleForPlacementContent(activeTrack, activePlacementIndex, nullptr);
    const int oldRootNote = static_cast<int>(oldResolved.root);
    const int oldScaleType = scaleToUiScaleType(oldResolved.scale);

    if (oldRootNote == newRoot && oldScaleType == newScaleType) {
        applyScaleToUi(newRoot, newScaleType);
        return;
    }

    const DetectedKey newKey = makeDetectedKeyFromUi(newRoot, newScaleType, 1.0f);

    if (activeContentKey.isValid()) {
        processorRef_.setContentDetectedKey(activeContentKey, newKey);
    }
    applyScaleToUi(newRoot, newScaleType);

    DBG("ScaleSyncTrace: source=manual trackId=" + juce::String(activeTrack)
        + " placementIndex=" + juce::String(activePlacementIndex)
        + " contentKey.objectId=" + juce::String(static_cast<juce::int64>(activeContentKey.objectId))
        + " root=" + juce::String(newRoot)
        + " scale=" + juce::String(newScaleType));

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::viewToggled(bool workspaceView)
{
    isWorkspaceView_ = workspaceView;
    arrangementView_.setVisible(isWorkspaceView_);
    pianoRoll_.setVisible(!isWorkspaceView_);
    
    // Explicitly grab focus for the active view to ensure keyboard shortcuts work immediately
    if (isWorkspaceView_) {
        arrangementView_.grabKeyboardFocus();
        arrangementView_.syncPlayheadOverlay();
    } else {
        pianoRoll_.grabKeyboardFocus();
    }

    resized();
    repaint();

// Delayed auto-zoom call, ensures resized() completes first
    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
    juce::Timer::callAfterDelay(50, [safeThis, workspaceView]() {
        if (safeThis == nullptr) return;

        if (workspaceView) {
// Switch to ArrangementView
            if (!safeThis->arrangementView_.hasUserManuallyZoomed()) {
                safeThis->arrangementView_.fitToContent();
            }
        } else {
// Switch to PianoRoll
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
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::trackSoloToggled(int trackId, bool solo)
{
    setStandaloneTrackSolo(processorRef_, trackId, solo);
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::trackVolumeChanged(int trackId, float volume)
{
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS) return;
    
    setStandaloneTrackVolume(processorRef_, trackId, volume);
    lastTrackVolumes_[static_cast<size_t>(trackId)] = volume;
    projectSession_.markDirty();
}

// Y-axis zoom sync: when TrackPanel or ArrangementView zooms via Ctrl+scrollwheel, sync the other component
void OpenTuneAudioProcessorEditor::trackHeightChanged(int newHeight)
{
// Update track height in processor
    processorRef_.setTrackHeight(newHeight);
    
// Sync TrackPanel if not triggered by it
    if (trackPanel_.getTrackHeight() != newHeight)
    {
        trackPanel_.setTrackHeight(newHeight);
    }
    
    // 鍒锋柊ArrangementView
    arrangementView_.repaint();
}

void OpenTuneAudioProcessorEditor::visibleTrackCountChanged(int newCount)
{
    arrangementView_.setVisibleTrackCount(newCount);
}

void OpenTuneAudioProcessorEditor::trackColorChangeRequested(int trackId)
{
    // Wrapper component: holds ColourSelector, applies result when dialog closes via destructor
    struct ColourPickerContent : public juce::Component
    {
        ColourPickerContent(OpenTuneAudioProcessorEditor& owner, int tid, juce::Colour current)
            : owner_(owner), trackId_(tid)
        {
            selector_ = std::make_unique<juce::ColourSelector>(
                juce::ColourSelector::showColourAtTop |
                juce::ColourSelector::showSliders |
                juce::ColourSelector::showColourspace);
            selector_->setCurrentColour(current);
            selector_->setSize(380, 300);
            addAndMakeVisible(selector_.get());
        }

        ~ColourPickerContent() override
        {
            if (selector_)
            {
                juce::Colour selected = selector_->getCurrentColour();
                setStandaloneTrackColour(owner_.processorRef_, trackId_, selected);
                owner_.trackPanel_.setTrackColour(trackId_, selected);
                owner_.trackPanel_.repaint();
                owner_.arrangementView_.repaint();
                owner_.projectSession_.markDirty();
            }
        }

        void resized() override
        {
            if (selector_)
                selector_->setBounds(getLocalBounds());
        }

    private:
        OpenTuneAudioProcessorEditor& owner_;
        int trackId_;
        std::unique_ptr<juce::ColourSelector> selector_;
    };

    juce::Colour current = getStandaloneTrackColour(processorRef_, trackId);
    auto* content = new ColourPickerContent(*this, trackId, current);
    content->setSize(380, 300);

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle = "Track " + juce::String(trackId + 1) + " Color";
    opts.content.setOwned(content);
    opts.dialogBackgroundColour = UIColors::backgroundDark;
    opts.componentToCentreAround = this;
    opts.escapeKeyTriggersCloseButton = true;
    opts.launchAsync();
}

void OpenTuneAudioProcessorEditor::trackAddRequested()
{
    const int current = trackPanel_.getVisibleTrackCount();
    if (current >= OpenTuneAudioProcessor::MAX_TRACKS)
        return;

    trackPanel_.setVisibleTrackCount(current + 1);
    arrangementView_.setVisibleTrackCount(current + 1);
    arrangementView_.repaint();
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::trackDuplicateRequested(int trackId)
{
    auto* arrangement = processorRef_.getStandaloneArrangement();
    if (arrangement == nullptr)
        return;

    const int visibleCount = trackPanel_.getVisibleTrackCount();
    if (visibleCount >= OpenTuneAudioProcessor::MAX_TRACKS)
        return;

    const int targetSlot = visibleCount;

    // Copy track mix state via public API
    arrangement->setTrackMuted(targetSlot, arrangement->isTrackMuted(trackId));
    arrangement->setTrackSolo(targetSlot, arrangement->isTrackSolo(trackId));
    arrangement->setTrackVolume(targetSlot, arrangement->getTrackVolume(trackId));
    arrangement->setTrackColour(targetSlot, arrangement->getTrackColour(trackId));

    // Copy placements
    const int numPlacements = arrangement->getNumPlacements(trackId);
    for (int i = 0; i < numPlacements; ++i) {
        StandaloneArrangement::Placement p;
        if (arrangement->getPlacementByIndex(trackId, i, p)) {
            p.placementId = 0;  // Let insertPlacement assign a new ID
            arrangement->insertPlacement(targetSlot, p);
        }
    }

    // Expand visible count to show the new track
    trackPanel_.setVisibleTrackCount(visibleCount + 1);
    arrangementView_.setVisibleTrackCount(visibleCount + 1);
    trackPanel_.setTrackColour(targetSlot, arrangement->getTrackColour(trackId));
    arrangementView_.repaint();
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::trackDeleteRequested(int trackId)
{
    auto* arrangement = processorRef_.getStandaloneArrangement();
    if (arrangement == nullptr)
        return;

    const int visibleCount = trackPanel_.getVisibleTrackCount();
    if (visibleCount <= 1)
        return; // 鑷冲皯淇濈暀涓€鏉¤建閬?

    if (trackId < 0 || trackId >= visibleCount)
        return;

// Atomically shift subsequent tracks up, clear the last slot
    arrangement->removeTrackAndShift(trackId, visibleCount);

// Sync TrackPanel UI state (color, mute/solo/volume)
    const int newVisibleCount = visibleCount - 1;
    for (int i = 0; i < newVisibleCount; ++i) {
        trackPanel_.setTrackMuted(i, arrangement->isTrackMuted(i));
        trackPanel_.setTrackSolo(i, arrangement->isTrackSolo(i));
        trackPanel_.setTrackVolume(i, arrangement->getTrackVolume(i));
        trackPanel_.setTrackColour(i, arrangement->getTrackColour(i));
    }

// Reduce visible track count (triggers resized + repaint + listener notification)
    trackPanel_.setVisibleTrackCount(newVisibleCount);
    arrangementView_.setVisibleTrackCount(newVisibleCount);
    arrangementView_.repaint();
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::trackColorRandomizeRequested(int trackId)
{
    auto* arrangement = processorRef_.getStandaloneArrangement();
    if (arrangement == nullptr)
        return;

    // Pick a random color from the pastel palette
    const int colorIndex = juce::Random::getSystemRandom().nextInt(12);
    const auto newColour = juce::Colour(OpenTune::trackPastelColors[colorIndex]);

    arrangement->setTrackColour(trackId, newColour);
    trackPanel_.setTrackColour(trackId, newColour);
    arrangementView_.repaint();
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::placementSelectionChanged(int trackId, uint64_t placementId)
{
    applyPlacementSelectionContext(trackId, placementId);

    // 鏇存柊 reference context
    refreshReferenceContext();

// If in PianoRoll view and user has not manually zoomed, auto-fit to new clip
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

    projectSession_.markDirty();
}

// Y-axis scroll sync: notify other component when ArrangementView or TrackPanel scrolls
void OpenTuneAudioProcessorEditor::verticalScrollChanged(int newOffset)
{
    // 鍚屾TrackPanel
    trackPanel_.setVerticalScrollOffset(newOffset);
    // 鍚屾ArrangementView
    arrangementView_.setVerticalScrollOffset(newOffset);
}

void OpenTuneAudioProcessorEditor::horizontalScrollChanged(int newOffset)
{
    pianoRoll_.setScrollOffset(newOffset);
}

void OpenTuneAudioProcessorEditor::zoomLevelChanged(double newZoom)
{
    pianoRoll_.setZoomLevel(newZoom);
}

void OpenTuneAudioProcessorEditor::scrollModeChanged(bool isContinuous)
{
    pianoRoll_.setScrollMode(isContinuous
        ? PianoRollComponent::ScrollMode::Continuous
        : PianoRollComponent::ScrollMode::Page);
}

void OpenTuneAudioProcessorEditor::placementDoubleClicked(int trackId, int placementIndex)
{
    // 1. Switch to Piano Roll View
    if (isWorkspaceView_)
    {
        transportBar_.setWorkspaceView(false);
        viewToggled(false); // 浼氳Е鍙戣嚜鍔ㄧ缉鏀?
    }
    else
    {
// If already in PianoRoll view, also call fitToScreen
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
        FrameScheduler::instance().setTimelinePlaybackActive(false);
    } else {
        double startPos = processorRef_.getPlayStartPosition();
        processorRef_.setPosition(startPos);
        processorRef_.setPlaying(true);
        processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Play);
        transportBar_.setPlaying(true);
        pianoRoll_.setIsPlaying(true);
        arrangementView_.setIsPlaying(true);
        FrameScheduler::instance().setTimelinePlaybackActive(true);
    }
}

void OpenTuneAudioProcessorEditor::autoTuneRequested()
{
    const auto autoRefUiState = evaluateAutoRefUiState();
    if (autoRefUiState.shouldRunReferenceAuto()) {
        if (handleAutoRefExecute()) {
            projectSession_.markDirty();
        }
        return;
    }

    const auto result = pianoRoll_.applyAutoTuneToSelection();
    if (!result.applied()) {
        ConfirmDialogContent::showMessage(this,
                                          juce::String("AUTO"),
                                          result.message());
        return;
    }

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::pitchShiftRequested()
{
    const int trackId = getStandaloneActiveTrack(processorRef_);
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    if (placementIndex < 0) return;

    const ContentKey contentKey = getStandaloneContentKey(processorRef_, trackId, placementIndex);
    if (!contentKey.isValid()) return;

    const auto currentSettings = processorRef_.getPitchShiftSettings(contentKey);

    auto* content = new PitchShiftDialogContent(currentSettings);

    auto commands = processorRef_.getContentCommands();
    content->setOnConfirm([this, contentKey, currentSettings, commands](const PitchShiftSettings& newSettings) {
        if (newSettings != currentSettings) {
            processorRef_.getUndoManager().addAction(std::make_unique<PitchShiftEditAction>(
                commands, contentKey, currentSettings, newSettings));
            if (commands)
                commands->setPitchShiftSettings(contentKey, newSettings);
            parameterPanel_.setPitchShiftIndicator(newSettings.semitone, newSettings.cents);
            projectSession_.markDirty();
        }
    });

    content->setOnReset([this, contentKey, currentSettings, commands]() {
        const auto identity = PitchShiftSettings::identity();
        if (identity != currentSettings) {
            processorRef_.getUndoManager().addAction(std::make_unique<PitchShiftEditAction>(
                commands, contentKey, currentSettings, identity));
            if (commands)
                commands->setPitchShiftSettings(contentKey, identity);
            parameterPanel_.setPitchShiftIndicator(0, 0);
            projectSession_.markDirty();
        }
    });

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

void OpenTuneAudioProcessorEditor::pitchCurveEdited(int startFrame, int endFrame)
{
    DBG("Editor: Pitch curve edited frames " + juce::String(startFrame) + " to " + juce::String(endFrame));

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::escapeKeyPressed()
{
// ESC equivalent to view toggle button: switch PianoRoll/ArrangementView and sync button state
    const bool targetWorkspaceView = !isWorkspaceView_;
    transportBar_.setWorkspaceView(targetWorkspaceView);
    viewToggled(targetWorkspaceView);
}

void OpenTuneAudioProcessorEditor::currentToolChanged(ToolId tool)
{
    parameterPanel_.setActiveTool(static_cast<int>(tool));
}

void OpenTuneAudioProcessorEditor::saveProjectAsRequested()
{
    auto chooser = std::make_shared<juce::FileChooser>(juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B"), juce::File(), "*.otproj");
    auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles;
    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    chooser->launchAsync(chooserFlags, [safeThis, chooser](const juce::FileChooser& fc) {
        if (safeThis == nullptr) return;
        auto file = fc.getResult();
        if (file == juce::File{}) return;
        if (!file.hasFileExtension(".otproj"))
            file = file.withFileExtension(".otproj");

        if (file.existsAsFile()) {
            ConfirmDialogContent::launch(
                new ConfirmDialogContent(
                    juce::String("Overwrite Existing Project?"),
                    juce::String("The target project file already exists. Overwrite it?"),
                    { { juce::String::fromUTF8(u8"\u8986\u76D6"), [safeThis, file] {
                            if (safeThis == nullptr) return;
                            if (safeThis->saveWorker_.joinable()) safeThis->saveWorker_.join();
                            safeThis->projectSession_.setCurrentProjectFile(file);
                            auto task = safeThis->projectSession_.prepareSave();
                            const uint64_t gen = safeThis->projectSession_.getDirtyGeneration();
                            safeThis->saveWorker_ = std::thread([safeThis, task = std::move(task), gen, file]() mutable {
                                auto result = ProjectSession::executeSaveToFile(task);
                                juce::MessageManager::callAsync([safeThis, result, gen, file]() {
                                    if (safeThis == nullptr) return;
                                    if (!result.ok()) {
                                        ConfirmDialogContent::launch(
                                            new ConfirmDialogContent(
                                                juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B\u5931\u8D25"),
                                                result.error().fullMessage(),
                                                { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                                            safeThis.getComponent());
                                        return;
                                    }
                                    if (safeThis->projectSession_.getDirtyGeneration() == gen)
                                        safeThis->projectSession_.clearDirty();
                                    safeThis->projectSession_.pushRecentProject(file);
                                    safeThis->syncRecentProjectsToMenu();
                                    safeThis->updateTitleWithProjectPath();
                                });
                            });
                        }, true },
                      { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr, false } }),
                safeThis.getComponent());
            return; // Don't continue in outer callback — the inner callback handles save
        }

        if (safeThis->saveWorker_.joinable()) safeThis->saveWorker_.join();
        safeThis->projectSession_.setCurrentProjectFile(file);
        auto task = safeThis->projectSession_.prepareSave();
        const uint64_t gen = safeThis->projectSession_.getDirtyGeneration();
        safeThis->saveWorker_ = std::thread([safeThis, task = std::move(task), gen, file]() mutable {
            auto result = ProjectSession::executeSaveToFile(task);
            juce::MessageManager::callAsync([safeThis, result, gen, file]() {
                if (safeThis == nullptr) return;
                if (!result.ok()) {
                    ConfirmDialogContent::launch(
                        new ConfirmDialogContent(
                            juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B\u5931\u8D25"),
                            result.error().fullMessage(),
                            { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                        safeThis.getComponent());
                    return;
                }
                if (safeThis->projectSession_.getDirtyGeneration() == gen)
                    safeThis->projectSession_.clearDirty();
                safeThis->projectSession_.pushRecentProject(file);
                safeThis->syncRecentProjectsToMenu();
                safeThis->updateTitleWithProjectPath();
            });
        });
    });
}

void OpenTuneAudioProcessorEditor::openRecentProjectRequested(const juce::File& file)
{
    if (saveWorker_.joinable()) saveWorker_.join();
    if (!projectSession_.isDirty()) {
        if (openWorker_.joinable()) openWorker_.join();

        juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
        openWorker_ = std::thread([safeThis, file]() {
            auto result = safeThis->projectSession_.openProject(file);

            juce::MessageManager::callAsync([safeThis, result, file]() {
                if (safeThis == nullptr) return;

                if (!result.ok()) {
                    ConfirmDialogContent::launch(
                        new ConfirmDialogContent(
                            juce::String::fromUTF8(u8"\u6253\u5F00\u5DE5\u7A0B\u5931\u8D25"),
                            result.error().fullMessage(),
                            { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                        safeThis.getComponent());
                    safeThis->projectSession_.clearRecentProjects();
                    return;
                }

                safeThis->syncRecentProjectsToMenu();
                safeThis->updateTitleWithProjectPath();
                safeThis->refreshAllUIFromProject();
            });
        });
        return;
    }

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
    ConfirmDialogContent::launch(
        new ConfirmDialogContent(
            juce::String::fromUTF8(u8"\u5F53\u524D\u5DE5\u7A0B\u5C1A\u672A\u4FDD\u5B58"),
            juce::String::fromUTF8(u8"\u6253\u5F00\u5176\u4ED6\u5DE5\u7A0B\u524D\uFF0C\u662F\u5426\u4FDD\u5B58\u5F53\u524D\u5DE5\u7A0B\u7684\u66F4\u6539\uFF1F"),
            {               { juce::String::fromUTF8(u8"\u4FDD\u5B58"), [safeThis, file] {
                    if (safeThis == nullptr) return;
                    if (!safeThis->projectSession_.hasProjectPath()) {
                        // No project path: async save-as, then open recent file
                        auto chooser = std::make_shared<juce::FileChooser>(juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B"), juce::File(), "*.otproj");
                        auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles;
                        chooser->launchAsync(chooserFlags, [safeThis, chooser, file](const juce::FileChooser& fc) {
                            if (safeThis == nullptr) return;
                            auto saveFile = fc.getResult();
                            if (saveFile == juce::File{}) return;
                            if (!saveFile.hasFileExtension(".otproj"))
                                saveFile = saveFile.withFileExtension(".otproj");

                            auto handleSaveAndOpen = [safeThis, file](const juce::File& saveFile, bool overwrite) {
                                if (safeThis == nullptr) return;
                                if (safeThis->saveWorker_.joinable()) safeThis->saveWorker_.join();
                                safeThis->projectSession_.setCurrentProjectFile(saveFile);
                                auto task = safeThis->projectSession_.prepareSave();
                                const uint64_t gen = safeThis->projectSession_.getDirtyGeneration();
                                safeThis->saveWorker_ = std::thread([safeThis, task = std::move(task), gen, saveFile, file]() mutable {
                                    auto result = ProjectSession::executeSaveToFile(task);
                                    juce::MessageManager::callAsync([safeThis, result, gen, saveFile, file]() {
                                        if (safeThis == nullptr) return;
                                        if (!result.ok()) {
                                            ConfirmDialogContent::launch(
                                                new ConfirmDialogContent(
                                                    juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B\u5931\u8D25"),
                                                    result.error().fullMessage(),
                                                    { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                                                safeThis.getComponent());
                                            return;
                                        }
                                        if (safeThis->projectSession_.getDirtyGeneration() == gen)
                                            safeThis->projectSession_.clearDirty();
                                        safeThis->projectSession_.pushRecentProject(saveFile);
                                        safeThis->syncRecentProjectsToMenu();
                                        safeThis->updateTitleWithProjectPath();
                                        // Open the recent file after save completes
                                        if (safeThis->openWorker_.joinable()) safeThis->openWorker_.join();
                                        safeThis->openWorker_ = std::thread([safeThis, file]() {
                                            auto openResult = safeThis->projectSession_.openProject(file);
                                            juce::MessageManager::callAsync([safeThis, openResult, file]() {
                                                if (safeThis == nullptr) return;
                                                if (!openResult.ok()) {
                                                    ConfirmDialogContent::launch(
                                                        new ConfirmDialogContent(
                                                            juce::String::fromUTF8(u8"\u6253\u5F00\u5DE5\u7A0B\u5931\u8D25"),
                                                            openResult.error().fullMessage(),
                                                            { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                                                        safeThis.getComponent());
                                                    safeThis->projectSession_.clearRecentProjects();
                                                    return;
                                                }
                                                safeThis->syncRecentProjectsToMenu();
                                                safeThis->updateTitleWithProjectPath();
                                                safeThis->refreshAllUIFromProject();
                                            });
                                        });
                                    });
                                });
                            };

                            if (saveFile.existsAsFile()) {
                                ConfirmDialogContent::launch(
                                    new ConfirmDialogContent(
                                        juce::String("Overwrite Existing Project?"),
                                        juce::String("The target project file already exists. Overwrite it?"),
                                        { { juce::String::fromUTF8(u8"\u8986\u76D6"), [safeThis, saveFile, handleSaveAndOpen] {
                                                handleSaveAndOpen(saveFile, true);
                                            }, true },
                                          { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr, false } }),
                                    safeThis.getComponent());
                                return;
                            }
                            handleSaveAndOpen(saveFile, false);
                        });
                        return;
                    }
                    if (safeThis->saveWorker_.joinable()) safeThis->saveWorker_.join();
                    {
                        // Capture on message thread
                        auto task = safeThis->projectSession_.prepareSave();
                        const uint64_t gen = safeThis->projectSession_.getDirtyGeneration();
                        const auto path = task.targetFile;
                        safeThis->saveWorker_ = std::thread([safeThis, task = std::move(task), gen, path, file]() mutable {
                            auto saveResult = ProjectSession::executeSaveToFile(task);
                            juce::MessageManager::callAsync([safeThis, saveResult, gen, path, file]() {
                                if (safeThis == nullptr) return;
                                if (!saveResult.ok()) {
                                    ConfirmDialogContent::launch(
                                        new ConfirmDialogContent(
                                            juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B\u5931\u8D25"),
                                            saveResult.error().fullMessage(),
                                            { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                                        safeThis.getComponent());
                                    return;
                                }
                                if (safeThis->projectSession_.getDirtyGeneration() == gen)
                                    safeThis->projectSession_.clearDirty();
                                safeThis->projectSession_.pushRecentProject(path);
                                safeThis->updateTitleWithProjectPath();
                                safeThis->syncRecentProjectsToMenu();
                                if (safeThis->openWorker_.joinable()) safeThis->openWorker_.join();
                                safeThis->openWorker_ = std::thread([safeThis, file]() {
                                    auto openResult = safeThis->projectSession_.openProject(file);
                                    juce::MessageManager::callAsync([safeThis, openResult, file]() {
                                        if (safeThis == nullptr) return;
                                        if (!openResult.ok()) {
                                            ConfirmDialogContent::launch(
                                                new ConfirmDialogContent(
                                                    juce::String::fromUTF8(u8"\u6253\u5F00\u5DE5\u7A0B\u5931\u8D25"),
                                                    openResult.error().fullMessage(),
                                                    { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                                                safeThis.getComponent());
                                            safeThis->projectSession_.clearRecentProjects();
                                            return;
                                        }
                                        safeThis->syncRecentProjectsToMenu();
                                        safeThis->updateTitleWithProjectPath();
                                        safeThis->refreshAllUIFromProject();
                                    });
                                });
                             });
                          });
                      }
                  }, true },
{ juce::String("Do Not Save"), [safeThis, file] {
                     if (safeThis == nullptr) return;
                     if (safeThis->saveWorker_.joinable()) safeThis->saveWorker_.join();
                     if (safeThis->openWorker_.joinable()) safeThis->openWorker_.join();
                     safeThis->openWorker_ = std::thread([safeThis, file]() {
                         auto openResult = safeThis->projectSession_.openProject(file);
                         juce::MessageManager::callAsync([safeThis, openResult, file]() {
                             if (safeThis == nullptr) return;
                             if (!openResult.ok()) {
                                 ConfirmDialogContent::launch(
                                     new ConfirmDialogContent(
                                         juce::String::fromUTF8(u8"\u6253\u5F00\u5DE5\u7A0B\u5931\u8D25"),
                                         openResult.error().fullMessage(),
                                         { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                                     safeThis.getComponent());
                                 safeThis->projectSession_.clearRecentProjects();
                                 return;
                             }
                             safeThis->syncRecentProjectsToMenu();
                             safeThis->updateTitleWithProjectPath();
                             safeThis->refreshAllUIFromProject();
                         });
                     });
                 }, false },
              { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr, false } }),
        this);
}

void OpenTuneAudioProcessorEditor::clearRecentProjectsRequested()
{
    projectSession_.clearRecentProjects();
    syncRecentProjectsToMenu();
}

void OpenTuneAudioProcessorEditor::updateTitleWithProjectPath()
{
    const auto name = projectSession_.getProjectName();
    juce::String title = "OpenTune - " + name;
    if (projectSession_.isDirty()) {
        title += " *";
    }
    if (auto* dw = getTopLevelComponent()) {
        dw->setName(title);
    }
}

void OpenTuneAudioProcessorEditor::syncRecentProjectsToMenu()
{
    const auto files = projectSession_.getRecentProjects();
    menuBar_.setRecentProjects(files);
}

void OpenTuneAudioProcessorEditor::refreshAllUIFromProject()
{
    // Sync track panel with restored arrangement
    syncTrackColorsToPanel();
    for (int i = 0; i < OpenTuneAudioProcessor::MAX_TRACKS; ++i) {
        trackPanel_.setTrackMuted(i, getStandaloneTrackMuted(processorRef_, i));
        trackPanel_.setTrackSolo(i, getStandaloneTrackSolo(processorRef_, i));
        trackPanel_.setTrackVolume(i, getStandaloneTrackVolume(processorRef_, i));
    }
    const int visibleCount = trackPanel_.getVisibleTrackCount();
    trackPanel_.setVisibleTrackCount(visibleCount);

    // Sync arrangement view
    arrangementView_.setVisibleTrackCount(visibleCount);
    arrangementView_.repaint();

    // Sync piano roll to current selection
    const int activeTrack = getStandaloneActiveTrack(processorRef_);
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
    syncPianoRollFromPlacementSelection(activeTrack, placementIndex);

    // Clear selection state
    pianoRoll_.requestContentRedraw();
}

// ============================================================================
// Reference Auto-Align Methods
// ============================================================================

// Look up a reference placement's content across all tracks.
static bool findReferencePlacementInfo(OpenTuneAudioProcessor& processor,
                                       uint64_t refPlacementId,
                                       StandaloneArrangement::Placement& out)
{
    for (int t = 0; t < MAX_TRACKS; ++t) {
        if (processor.getPlacementById(t, refPlacementId, out)) {
            return true;
        }
    }
    return false;
}

OpenTuneAudioProcessorEditor::AutoRefUiState OpenTuneAudioProcessorEditor::evaluateAutoRefUiState() const
{
    AutoRefUiState uiState;
    uiState.presentation.mode = ParameterPanel::AutoButtonPresentation::Mode::StandardAuto;
    uiState.presentation.tooltip = juce::String("Auto tune to nearby notes");

    const int trackId = getStandaloneActiveTrack(processorRef_);
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    const uint64_t targetPlacementId = placementIndex >= 0
        ? processorRef_.getPlacementId(trackId, placementIndex)
        : 0;

    const auto preferencesState = appPreferences_.getState();
    const bool experimentalFeaturesEnabled = preferencesState.shared.experimentalFeaturesEnabled;
    const auto expMode = preferencesState.shared.experimentalReferenceAlignMode;
    processorRef_.setExperimentalReferenceAlignMode(expMode);

    uiState.availability = processorRef_.queryAutoRefAvailability(targetPlacementId);
    if (!experimentalFeaturesEnabled || expMode == ExperimentalReferenceAlignMode::Off) {
        return uiState;
    }

    if (uiState.availability.status == OpenTuneAudioProcessor::AutoRefAvailability::Status::Ready) {
        uiState.presentation.mode = ParameterPanel::AutoButtonPresentation::Mode::ReferenceAuto;
        uiState.presentation.tooltip = juce::String("Auto tune and align to the reference clip");
        return uiState;
    }

    if (uiState.availability.status == OpenTuneAudioProcessor::AutoRefAvailability::Status::GameUnavailable
        && uiState.availability.hasReferenceBinding()) {
        uiState.presentation.mode =
            ParameterPanel::AutoButtonPresentation::Mode::ReferenceBoundButFallbackToAuto;
        uiState.presentation.tooltip = uiState.availability.message;
    }

    return uiState;
}

void OpenTuneAudioProcessorEditor::refreshReferenceContext()
{
    const auto autoRefUiState = evaluateAutoRefUiState();
    parameterPanel_.setAutoButtonPresentation(autoRefUiState.presentation);

    if (!autoRefUiState.availability.hasReferenceBinding()) {
        pianoRoll_.setReferenceOverlay(std::nullopt);
        return;
    }

    const auto preferencesState = appPreferences_.getState();
    const bool experimentalFeaturesEnabled = preferencesState.shared.experimentalFeaturesEnabled;
    const auto expMode = preferencesState.shared.experimentalReferenceAlignMode;
    if (!experimentalFeaturesEnabled || expMode == ExperimentalReferenceAlignMode::Off) {
        pianoRoll_.setReferenceOverlay(std::nullopt);
        return;
    }

    // Resolve reference content
    StandaloneArrangement::Placement refPlacement;
    if (!findReferencePlacementInfo(processorRef_, autoRefUiState.availability.referencePlacementId, refPlacement)) {
        pianoRoll_.setReferenceOverlay(std::nullopt);
        return;
    }

    // If reference features are ready, set up piano roll overlay
    if (refPlacement.contentKey.isValid()) {
        const ReferenceFeatureSet refFeatures = processorRef_.getReferenceFeatures(refPlacement.contentKey);
        if (refFeatures.isReady()
            && refFeatures.producer == ReferenceFeatureProducer::Game)
        {
            PianoRollRenderer::ReferenceOverlay overlay;
            overlay.ghostNotes = refFeatures.pitch.notes;
            for (const auto& event : refFeatures.timing.anchors) {
                PianoRollRenderer::ReferenceOverlay::GhostAnchor ga;
                ga.sourceSeconds = event.sourceSeconds;
                ga.strength = event.strength;
                overlay.ghostAnchors.push_back(ga);
            }
            overlay.ghostColour = juce::Colours::steelblue;
            overlay.enabled = true;
            overlay.sourceProjection = makePianoRollProjection(refPlacement, processorRef_);
            pianoRoll_.setReferenceOverlay(overlay);
        } else {
            pianoRoll_.setReferenceOverlay(std::nullopt);
        }
    } else {
        pianoRoll_.setReferenceOverlay(std::nullopt);
    }
}

void OpenTuneAudioProcessorEditor::referenceButtonClicked(int trackId, uint64_t placementId,
                                                           juce::Rectangle<int> buttonScreenArea)
{
    if (!appPreferences_.getState().shared.experimentalFeaturesEnabled) {
        return;
    }

    resolveReferenceBindingMenu(trackId, placementId, buttonScreenArea);
}

void OpenTuneAudioProcessorEditor::resolveReferenceBindingMenu(int trackId, uint64_t targetPlacementId,
                                                                juce::Rectangle<int> buttonScreenArea)
{
    if (!appPreferences_.getState().shared.experimentalFeaturesEnabled) {
        return;
    }

    auto* arrangement = processorRef_.getStandaloneArrangement();
    juce::PopupMenu menu;

// Get target placement info
    StandaloneArrangement::Placement targetPlacement;
    if (!processorRef_.getPlacementById(trackId, targetPlacementId, targetPlacement)) {
        return;
    }

// Check if already has reference binding
    const uint64_t existingRef = arrangement->getPlacementReferencePlacement(trackId, targetPlacementId);
    if (existingRef != 0) {
        menu.addItem(juce::String::fromUTF8(u8"\u4E0D\u4F7F\u7528\u53C2\u8003Clip"), [this, arrangement, trackId, targetPlacementId]() {
            arrangement->clearPlacementReferencePlacement(trackId, targetPlacementId);
            refreshReferenceContext();
        });
        menu.addSeparator();
    }

// Submenu: Select Reference Clip (list other clips in same view, excluding self)
    juce::PopupMenu refMenu;
    bool hasCandidates = false;

    for (int t = 0; t < MAX_TRACKS; ++t) {
        const int numPlacements = arrangement->getNumPlacements(t);
        for (int pi = 0; pi < numPlacements; ++pi) {
            StandaloneArrangement::Placement candidate;
            if (!processorRef_.getPlacementByIndex(t, pi, candidate)) continue;
            if (candidate.placementId == targetPlacementId) continue; // 排除自身
            if (candidate.isRetired) continue;

// Exclude clips already serving as target (referenced by other clips)
// Simplified check: only exclude cyclic reference cases
            if (arrangement->isCyclicReference(trackId, targetPlacementId, candidate.placementId)) continue;

            hasCandidates = true;
            const juce::String label = juce::String("Track ") + juce::String(t + 1)
                + " - " + (candidate.name.isNotEmpty() ? candidate.name : "Clip")
                + juce::String(" (Mat#") + juce::String(static_cast<juce::int64>(candidate.contentKey.objectId)) + ")";
            refMenu.addItem(label, [this, arrangement, trackId, targetPlacementId, candidate]() {
                arrangement->setPlacementReferencePlacement(trackId, targetPlacementId, candidate.placementId);
                refreshReferenceContext();
            });
        }
    }

    if (hasCandidates) {
        menu.addSubMenu(juce::String::fromUTF8(u8"\u9009\u62E9\u53C2\u8003Clip"), refMenu);
    } else {
        menu.addItem(juce::String::fromUTF8(u8"(\u65E0\u53EF\u7528\u7684\u53C2\u8003Clip)"), false, false, nullptr);
    }

    if (buttonScreenArea.isEmpty())
    {
        // Fallback: anchor to the arrangement view's bottom-left corner
        buttonScreenArea = juce::Rectangle<int>(
            arrangementView_.getScreenBounds().getBottomLeft(),
            juce::Point<int>(arrangementView_.getScreenBounds().getX() + 200,
                             arrangementView_.getScreenBounds().getBottom()));
    }
    menu.showMenuAsync(juce::PopupMenu::Options()
        .withTargetScreenArea(buttonScreenArea));
}

bool OpenTuneAudioProcessorEditor::handleAutoRefExecute()
{
    const int trackId = getStandaloneActiveTrack(processorRef_);
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    if (placementIndex < 0) {
        return false;
    }

    const uint64_t targetPlacementId = processorRef_.getPlacementId(trackId, placementIndex);
    auto result = processorRef_.executeReferenceAlignmentForPlacement(targetPlacementId);
    if (!result.succeeded()) {
        const juce::String message = result.message.isNotEmpty()
            ? result.message
            : juce::String::fromUTF8(u8"AUTO Ref alignment failed.");
        ConfirmDialogContent::showMessage(
            this,
            juce::String::fromUTF8(u8"AUTO Ref"),
            message);
        refreshReferenceContext();
        return false;
    }

    syncPianoRollFromPlacementSelection(trackId, placementIndex);
    refreshReferenceContext();
    return true;
}

} // namespace OpenTune
