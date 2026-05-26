#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <mutex>
#include <vector>

#include "AudioEditingScheme.h"
#include "KeyShortcutConfig.h"
#include "LocalizationManager.h"
#include "MouseTrailConfig.h"
#include "PianoRollVisualPreferences.h"
#include "../Standalone/UI/ThemeTokens.h"
#include "VocoderModelWeight.h"
#include "ZoomSensitivityConfig.h"

namespace OpenTune {

enum class RenderingPriority {
    GpuFirst = 0,   // GPU 优先（默认）
    CpuFirst         // CPU 优先
};

enum class ExperimentalReferenceAlignMode
{
    Off = 0,        // 关闭（默认）
    Basic = 1,      // 自动对齐参考源（基础）— LegacyNoteGenerator
    Aggressive = 2  // 自动对齐参考源（激进）— GAME note generator
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
    ExperimentalReferenceAlignMode experimentalReferenceAlignMode = ExperimentalReferenceAlignMode::Off;
    std::vector<juce::String> recentProjects;   // Most recently used project paths (MRU, max 10)
};

struct StandalonePreferencesState {
    KeyShortcutConfig::KeyShortcutSettings shortcuts = KeyShortcutConfig::KeyShortcutSettings::getDefault();
    MouseTrailConfig::TrailTheme mouseTrailTheme = MouseTrailConfig::TrailTheme::Classic;
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
    void setShowChunkBoundaries(bool shouldShow);
    void setShowUnvoicedFrames(bool shouldShow);
    void setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& zoomSensitivity);
    void setStandaloneShortcuts(const KeyShortcutConfig::KeyShortcutSettings& shortcuts);
    void setRenderingPriority(RenderingPriority priority);
    void setVocoderModelWeight(VocoderModelWeight weight);
    void setExperimentalReferenceAlignMode(ExperimentalReferenceAlignMode mode);
    void setMouseTrailTheme(MouseTrailConfig::TrailTheme theme);

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
