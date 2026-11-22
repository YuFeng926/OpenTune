#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <mutex>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "AudioEditingScheme.h"
#include "CursorTheme.h"
#include "../Inference/IF0Extractor.h"
#include "KeyShortcutConfig.h"
#include "LocalizationManager.h"
#include "MouseTrailConfig.h"
#include "PianoRollVisualPreferences.h"
#include "../Standalone/UI/ThemeTokens.h"
#include "TimelineDisplayMode.h"
#include "TuningConfig.h"
#include "ZoomSensitivityConfig.h"

namespace OpenTune {

enum class RenderingPriority {
    GpuFirst = 0,   // GPU first (default)
    CpuFirst        // CPU first
};

/// Track color assignment mode
enum class TrackColorMode {
    Random = 0,   ///< New tracks get random colors (default)
    Custom = 1    ///< User picks custom colors per track
};

/// Snap mode for selected-note group pitch moves: follow the active scale (default) or snap to semitones.
enum class WholeNoteMoveSnapMode {
    Scale = 0,          ///< 跟随全局调式（默认）
    StandardPitch = 1   ///< 半音吸附
};

/// Snap-to-grid settings for arrangement clip editing.
struct SnapSettings {
    enum class Mode { Off = 0, Beat, Bar, Second, Count };
    bool enabled{false};
    Mode mode{Mode::Off};

    bool isActive() const noexcept { return enabled && mode != Mode::Off; }
};

struct PluginEditorSize {
    int width = 1000;
    int height = 900;
};

struct SharedPreferencesState {
    Language language = Language::Chinese;
    ThemeId theme = ThemeId::Aurora;
    AudioEditingScheme::Scheme audioEditingScheme = AudioEditingScheme::Scheme::CorrectedF0Primary;
    PianoRollVisualPreferences pianoRollVisualPreferences;
    ZoomSensitivityConfig::ZoomSensitivitySettings zoomSensitivity =
        ZoomSensitivityConfig::ZoomSensitivitySettings::getDefault();
    RenderingPriority renderingPriority = RenderingPriority::GpuFirst;
    bool experimentalFeaturesEnabled = false;
    std::vector<juce::String> recentProjects;   // Most recently used project paths (MRU, max 10)
    SnapSettings snap;
    TrackColorMode trackColorMode = TrackColorMode::Random;
    KeyShortcutConfig::KeyShortcutSettings shortcuts = KeyShortcutConfig::KeyShortcutSettings::getDefault();
    bool suppressEqRemoveConfirmation = false;  ///< 抑制 EQ 工具删除确认弹窗
    bool lightPitchCorrectionEnabled = true;  ///< 轻量修音开关（AutoTune 周期重采样，默认开）
    bool onboardingShown = false;
    // 首次运行的演示工程已经自动打开过。和 onboardingShown 放在同一份偏好里，
    // 避免只靠旁路标记文件时，中途退出导致下次启动再次自动打开。
    bool demoTourConsumed = false;
    F0ModelType f0ModelType = F0ModelType::FCPE;
    PianoGridStyle gridStyle = PianoGridStyle::PianoLanes;
    TimelineDisplayMode timelineDisplayMode = TimelineDisplayMode::Time;
    TuningConfig::TuningSettings tuning = TuningConfig::TuningSettings::getDefault();
    WholeNoteMoveSnapMode wholeNoteMoveSnapMode = WholeNoteMoveSnapMode::Scale;
    bool noteTransposePreviewEnabled = false;  ///< 拖动音符移调时试听预览
    bool accessibilityModeEnabled = false;  ///< 无障碍模式（读屏播报等，默认关闭）
};

struct StandalonePreferencesState {
    MouseTrailConfig::TrailTheme mouseTrailTheme = MouseTrailConfig::TrailTheme::Classic;
    CursorStyleId cursorStyle = CursorStyleId::System;
};

struct AppPreferencesState {
    SharedPreferencesState shared;
    StandalonePreferencesState standalone;
};

class AppPreferences {
public:
    struct StorageOptions {
        juce::String applicationName = "OpenTune";
        juce::File settingsDirectory;
        juce::String fileName = "app-preferences.settings";
    };

    using AudioEditingSchemeChanged = std::function<void(AudioEditingScheme::Scheme)>;

    AppPreferences();
    explicit AppPreferences(const StorageOptions& storageOptions,
                            AudioEditingSchemeChanged onAudioEditingSchemeChanged = {});

    AppPreferencesState getState() const;

    void load();
    void save();
    void flush();

    void setLanguage(Language language);
    void setOnboardingShown(bool shown);
    void setDemoTourConsumed(bool consumed);
    void setTheme(ThemeId theme);
    void setAudioEditingScheme(AudioEditingScheme::Scheme scheme);
    AudioEditingScheme::Scheme getAudioEditingScheme() const;
    void setNoteNameMode(NoteNameMode noteNameMode);
    void setShowPianoKeyboard(bool shouldShow);
    void setScaleAssistEnabled(bool enabled);
    void setShowUnvoicedFrames(bool shouldShow);
    void setBackgroundBrightness(float brightness);
    void setCorrectedF0Colour(std::uint32_t colour);
    void setOriginalF0Colour(std::uint32_t colour);
    void setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& zoomSensitivity);
    void setTuning(const TuningConfig::TuningSettings& tuning);
    void setShortcuts(const KeyShortcutConfig::KeyShortcutSettings& shortcuts);
    void setSuppressEqRemoveConfirmation(bool suppress);
    void setRenderingPriority(RenderingPriority priority);
    void setExperimentalFeaturesEnabled(bool enabled);
    void setMouseTrailTheme(MouseTrailConfig::TrailTheme theme);
    void setCursorStyle(CursorStyleId style);
    void setSnapSettings(const SnapSettings& snap);
    SnapSettings getSnapSettings() const;

    void setTrackColorMode(TrackColorMode mode);
    TrackColorMode getTrackColorMode() const;

    void setLightPitchCorrectionEnabled(bool enabled);
    void setNoteTransposePreviewEnabled(bool enabled);
    bool getNoteTransposePreviewEnabled() const;

    void setAccessibilityModeEnabled(bool enabled);
    bool getAccessibilityModeEnabled() const;

    void setF0ModelType(F0ModelType type);
    F0ModelType getF0ModelType() const;

    void setGridStyle(PianoGridStyle gridStyle);
    PianoGridStyle getGridStyle() const;

    void setTimelineDisplayMode(TimelineDisplayMode mode);
    TimelineDisplayMode getTimelineDisplayMode() const;

    void setWholeNoteMoveSnapMode(WholeNoteMoveSnapMode mode);
    WholeNoteMoveSnapMode getWholeNoteMoveSnapMode() const;

    PluginEditorSize getDefaultPluginEditorSize();
    void setDefaultPluginEditorSize(PluginEditorSize size);

    std::vector<juce::String> getRecentProjects() const;
    void pushRecentProject(const juce::String& projectPath);
    void clearRecentProjects();

private:
    StorageOptions storageOptions_;
    AudioEditingSchemeChanged onAudioEditingSchemeChanged_;
    mutable std::mutex mutex_;
    AppPreferencesState state_;
    std::unique_ptr<juce::InterProcessLock> processLock_;
    juce::ApplicationProperties properties_;
    std::unique_ptr<juce::PropertiesFile> pluginEditorSizeProperties_;

    void initialiseStorage();
    void saveLocked();
};

struct ProcessingPreferencePatch
{
    std::optional<float> tuningHz;
    std::optional<AudioEditingScheme::Scheme> audioEditingScheme;
    std::optional<bool> lightPitchCorrectionEnabled;
    std::optional<bool> experimentalFeaturesEnabled;
    std::optional<RenderingPriority> renderingPriority;
    std::optional<F0ModelType> f0ModelType;
    std::optional<bool> accessibilityModeEnabled;
};

struct ProcessingPreferenceState
{
    float tuningHz = TuningConfig::kDefaultTuningHz;
    AudioEditingScheme::Scheme audioEditingScheme = AudioEditingScheme::Scheme::CorrectedF0Primary;
    bool lightPitchCorrectionEnabled = true;
    bool experimentalFeaturesEnabled = false;
    RenderingPriority renderingPriority = RenderingPriority::GpuFirst;
    F0ModelType f0ModelType = F0ModelType::FCPE;
    bool accessibilityModeEnabled = false;
};

struct ProcessingPreferenceDelta
{
    bool tuning = false;
    bool scheme = false;
    bool hybrid = false;
    bool experimental = false;
    bool priority = false;
    bool f0 = false;
    bool accessibilityMode = false;

    bool any() const noexcept;
    bool needsJob() const noexcept;
    bool needsRuntimeCompletion() const noexcept;
};

ProcessingPreferenceDelta diffProcessingPreferenceStates(const ProcessingPreferenceState& before,
                                                         const ProcessingPreferenceState& after);

enum class ProcessingPreferenceRuntimeStatus
{
    Changed,
    Unchanged,
    Failed
};

using ProcessingPreferenceRuntimeCallback =
    std::function<void(ProcessingPreferenceRuntimeStatus, const juce::String&)>;

struct ProcessingPreferenceRuntimeCompletion
{
    bool failed = false;
    bool backendChanged = false;
    juce::String failureReason;
};

struct ProcessingPreferenceRuntimeOperations
{
    std::function<bool(F0ModelType, juce::String&)> setF0ModelType;
    std::function<bool(bool, ProcessingPreferenceRuntimeCallback)> resetInferenceBackend;
    std::function<void()> invalidateAllContentCaches;
};

struct ProcessingPreferenceApplyResult
{
    enum class Status
    {
        Applied,
        NoChange,
        F0ModelRejected,
        RuntimeCommandRejected,
        F0RestoreRejected
    };

    ProcessingPreferenceState before;
    ProcessingPreferenceDelta delta;
    Status status = Status::NoChange;
    juce::String failureReason;

    bool succeeded() const noexcept
    {
        return status == Status::Applied || status == Status::NoChange;
    }
};

ProcessingPreferenceState readProcessingPreferenceState(const AppPreferences& preferences);
ProcessingPreferenceDelta computeProcessingPreferenceDelta(const ProcessingPreferenceState& state,
                                                           const ProcessingPreferencePatch& patch);
/** Shared product entry for the preferences UI and prefs.set. Message-thread only. */
ProcessingPreferenceApplyResult applyProcessingPreferencePatch(
    AppPreferences& preferences,
    const ProcessingPreferencePatch& patch,
    const ProcessingPreferenceRuntimeOperations& runtimeOperations,
    std::function<void()> onPreferencesChanged = {},
    std::function<void(ProcessingPreferenceRuntimeCompletion)> onRuntimeCompletion = {});

} // namespace OpenTune
