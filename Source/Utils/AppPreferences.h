#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <mutex>
#include <vector>

#include "AudioEditingScheme.h"
#include "CursorTheme.h"
#include "KeyShortcutConfig.h"
#include "LocalizationManager.h"
#include "MouseTrailConfig.h"
#include "PianoRollVisualPreferences.h"
#include "../Standalone/UI/ThemeTokens.h"
#include "TimelineDisplayMode.h"
#include "TuningConfig.h"
#include "VocoderModelWeight.h"
#include "ZoomSensitivityConfig.h"

namespace OpenTune {

enum class RenderingPriority {
    GpuFirst = 0,   // GPU first (default)
    CpuFirst        // CPU first
};

enum class ExperimentalReferenceAlignMode
{
    StandardAuto = 0,
    Game = 1
};

/// Track color assignment mode
enum class TrackColorMode {
    Random = 0,   ///< New tracks get random colors (default)
    Custom = 1    ///< User picks custom colors per track
};

/// Snap-to-grid settings for arrangement clip editing.
struct SnapSettings {
    enum class Mode { Off = 0, Beat, Bar, Second, Count };
    bool enabled{false};
    Mode mode{Mode::Off};

    bool isActive() const noexcept { return enabled && mode != Mode::Off; }
};

struct SharedPreferencesState {
    Language language = Language::Chinese;
    ThemeId theme = ThemeId::Aurora;
    AudioEditingScheme::Scheme audioEditingScheme = AudioEditingScheme::Scheme::CorrectedF0Primary;
    PianoRollVisualPreferences pianoRollVisualPreferences;
    ZoomSensitivityConfig::ZoomSensitivitySettings zoomSensitivity =
        ZoomSensitivityConfig::ZoomSensitivitySettings::getDefault();
    RenderingPriority renderingPriority = RenderingPriority::GpuFirst;
    VocoderModelWeight vocoderModelWeight = VocoderModelWeight::Community;
    bool experimentalFeaturesEnabled = false;
    ExperimentalReferenceAlignMode experimentalReferenceAlignMode = ExperimentalReferenceAlignMode::StandardAuto;
    std::vector<juce::String> recentProjects;   // Most recently used project paths (MRU, max 10)
    SnapSettings snap;
    TrackColorMode trackColorMode = TrackColorMode::Random;
    KeyShortcutConfig::KeyShortcutSettings shortcuts = KeyShortcutConfig::KeyShortcutSettings::getDefault();
    bool suppressEqRemoveConfirmation = false;  ///< 抑制 EQ 工具删除确认弹窗
    bool lightPitchCorrectionEnabled = false;  ///< 轻量修音开关（AutoTune cycle resampling，默认关）
    PianoGridStyle gridStyle = PianoGridStyle::PianoLanes;
    TimelineDisplayMode timelineDisplayMode = TimelineDisplayMode::Time;
    TuningConfig::TuningSettings tuning = TuningConfig::TuningSettings::getDefault();
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

    AppPreferences();
    explicit AppPreferences(const StorageOptions& storageOptions);

    AppPreferencesState getState() const;

    void load();
    void save();
    void flush();

    void setLanguage(Language language);
    void setTheme(ThemeId theme);
    void setAudioEditingScheme(AudioEditingScheme::Scheme scheme);
    void setPianoRollVisualPreferences(const PianoRollVisualPreferences& visualPreferences);
    void setNoteNameMode(NoteNameMode noteNameMode);
    void setShowUnvoicedFrames(bool shouldShow);
    void setBackgroundBrightness(float brightness);
    void setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& zoomSensitivity);
    void setTuning(const TuningConfig::TuningSettings& tuning);
    void setShortcuts(const KeyShortcutConfig::KeyShortcutSettings& shortcuts);
    void setSuppressEqRemoveConfirmation(bool suppress);
    void setRenderingPriority(RenderingPriority priority);
    void setVocoderModelWeight(VocoderModelWeight weight);
    void setExperimentalFeaturesEnabled(bool enabled);
    void setExperimentalReferenceAlignMode(ExperimentalReferenceAlignMode mode);
    void setMouseTrailTheme(MouseTrailConfig::TrailTheme theme);
    void setCursorStyle(CursorStyleId style);
    void setSnapSettings(const SnapSettings& snap);
    SnapSettings getSnapSettings() const;

    void setTrackColorMode(TrackColorMode mode);
    TrackColorMode getTrackColorMode() const;

    void setLightPitchCorrectionEnabled(bool enabled);

    void setGridStyle(PianoGridStyle gridStyle);
    PianoGridStyle getGridStyle() const;

    void setTimelineDisplayMode(TimelineDisplayMode mode);
    TimelineDisplayMode getTimelineDisplayMode() const;

    std::vector<juce::String> getRecentProjects() const;
    void pushRecentProject(const juce::String& projectPath);
    void clearRecentProjects();

private:
    StorageOptions storageOptions_;
    mutable std::mutex mutex_;
    AppPreferencesState state_;
    juce::ApplicationProperties properties_;
    std::unique_ptr<juce::InterProcessLock> processLock_;

    void initialiseStorage();
    void saveLocked();
};

} // namespace OpenTune
