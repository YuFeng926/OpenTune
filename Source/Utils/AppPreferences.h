#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <mutex>

#include "AudioEditingScheme.h"
#include "KeyShortcutConfig.h"
#include "LocalizationManager.h"
#include "MouseTrailConfig.h"
#include "PianoRollVisualPreferences.h"
#include "../Standalone/UI/ThemeTokens.h"
#include "ZoomSensitivityConfig.h"

namespace OpenTune {

enum class ReferenceTrackType
{
    Vocals = 0,  // 人声 — direct RMVPE (existing behavior)
    Song   = 1   // 歌曲 — MDX-NET vocal separation first, then RMVPE
};
inline juce::String toString(ReferenceTrackType t) { return t == ReferenceTrackType::Song ? "song" : "vocals"; }
inline ReferenceTrackType referenceTrackTypeFromToken(const juce::String& token) {
    return token == "song" ? ReferenceTrackType::Song : ReferenceTrackType::Vocals;
}

enum class RenderingPriority {
    GpuFirst = 0,   // GPU 优先（默认）
    CpuFirst         // CPU 优先
};

struct SharedPreferencesState {
    Language language = Language::Chinese;
    ThemeId theme = ThemeId::Aurora;
    AudioEditingScheme::Scheme audioEditingScheme = AudioEditingScheme::Scheme::CorrectedF0Primary;
    PianoRollVisualPreferences pianoRollVisualPreferences;
    ZoomSensitivityConfig::ZoomSensitivitySettings zoomSensitivity =
        ZoomSensitivityConfig::ZoomSensitivitySettings::getDefault();
    RenderingPriority renderingPriority = RenderingPriority::GpuFirst;
    ReferenceTrackType referenceTrackType = ReferenceTrackType::Vocals;
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
    void setReferenceVisualization(ReferenceVisualization viz);
    void setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& zoomSensitivity);
    void setStandaloneShortcuts(const KeyShortcutConfig::KeyShortcutSettings& shortcuts);
    void setRenderingPriority(RenderingPriority priority);
    void setReferenceTrackType(ReferenceTrackType type);

    void setMouseTrailTheme(MouseTrailConfig::TrailTheme theme);

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
