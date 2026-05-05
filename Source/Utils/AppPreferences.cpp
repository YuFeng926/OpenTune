#include "AppPreferences.h"

#include <array>

namespace OpenTune {

namespace {

constexpr const char* kSharedLanguageKey = "shared.language";
constexpr const char* kSharedThemeKey = "shared.theme.activeTheme";
constexpr const char* kSharedAudioEditingSchemeKey = "shared.audioEditing.scheme";
constexpr const char* kSharedPianoRollNoteNameModeKey = "shared.pianoRoll.noteNameMode";
constexpr const char* kSharedPianoRollShowChunkBoundariesKey = "shared.pianoRoll.showChunkBoundaries";
constexpr const char* kSharedPianoRollShowUnvoicedFramesKey = "shared.pianoRoll.showUnvoicedFrames";
constexpr const char* kSharedZoomHorizontalFactorKey = "shared.zoom.horizontalFactor";
constexpr const char* kSharedZoomVerticalFactorKey = "shared.zoom.verticalFactor";
constexpr const char* kSharedScrollSpeedKey = "shared.scroll.speed";
constexpr const char* kStandaloneMouseTrailThemeKey = "standalone.mouseTrail.theme";
constexpr const char* kSharedRenderingPriorityKey = "shared.rendering.priority";

constexpr std::array<const char*, static_cast<size_t>(KeyShortcutConfig::ShortcutId::Count)> kShortcutStorageKeys{{
    "standalone.shortcuts.playPause",
    "standalone.shortcuts.stop",
    "standalone.shortcuts.playFromStart",
    "standalone.shortcuts.undo",
    "standalone.shortcuts.redo",
    "standalone.shortcuts.cut",
    "standalone.shortcuts.copy",
    "standalone.shortcuts.paste",
    "standalone.shortcuts.selectAll",
    "standalone.shortcuts.delete",
}};

juce::File resolveSettingsDirectory(const AppPreferences::StorageOptions& storageOptions)
{
    if (storageOptions.settingsDirectory != juce::File{}) {
        return storageOptions.settingsDirectory;
    }

    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile(storageOptions.applicationName);
}

juce::File resolveSettingsFile(const AppPreferences::StorageOptions& storageOptions)
{
    const auto directory = resolveSettingsDirectory(storageOptions);
    const auto fileName = storageOptions.fileName.isNotEmpty()
        ? storageOptions.fileName
        : storageOptions.applicationName + ".settings";
    return directory.getChildFile(fileName);
}

juce::PropertiesFile::Options makePropertiesOptions(const AppPreferences::StorageOptions& storageOptions,
                                                    juce::InterProcessLock* processLock)
{
    const auto settingsFile = resolveSettingsFile(storageOptions);

    juce::PropertiesFile::Options options;
    options.applicationName = settingsFile.getFileNameWithoutExtension();
    options.filenameSuffix = settingsFile.getFileExtension();
    if (options.filenameSuffix.isEmpty()) {
        options.filenameSuffix = ".settings";
    }

    options.folderName = settingsFile.getParentDirectory().getFullPathName();
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.commonToAllUsers = false;
    options.ignoreCaseOfKeyNames = false;
    options.millisecondsBeforeSaving = 0;
    options.processLock = processLock;
    return options;
}

juce::String buildProcessLockName(const AppPreferences::StorageOptions& storageOptions)
{
    return resolveSettingsFile(storageOptions).getFullPathName() + ".lock";
}

const char* toLanguageToken(Language language)
{
    switch (language) {
        case Language::English: return "english";
        case Language::Chinese: return "chinese";
        case Language::Japanese: return "japanese";
        case Language::Russian: return "russian";
        case Language::Spanish: return "spanish";
        case Language::Count: break;
    }

    return "chinese";
}

Language languageFromToken(const juce::String& token)
{
    if (token == "english") return Language::English;
    if (token == "chinese") return Language::Chinese;
    if (token == "japanese") return Language::Japanese;
    if (token == "russian") return Language::Russian;
    if (token == "spanish") return Language::Spanish;
    return Language::Chinese;
}

const char* toThemeToken(ThemeId theme)
{
    switch (theme) {
        case ThemeId::BlueBreeze: return "blue-breeze";
        case ThemeId::DarkBlueGrey: return "dark-blue-grey";
        case ThemeId::Aurora: return "aurora";
    }

    return "aurora";
}

ThemeId themeFromToken(const juce::String& token)
{
    if (token == "blue-breeze") return ThemeId::BlueBreeze;
    if (token == "dark-blue-grey") return ThemeId::DarkBlueGrey;
    return ThemeId::Aurora;
}

const char* toAudioEditingSchemeToken(AudioEditingScheme::Scheme scheme)
{
    switch (scheme) {
        case AudioEditingScheme::Scheme::CorrectedF0Primary: return "corrected-f0-primary";
        case AudioEditingScheme::Scheme::NotesPrimary: return "notes-primary";
    }

    return "corrected-f0-primary";
}

AudioEditingScheme::Scheme audioEditingSchemeFromToken(const juce::String& token)
{
    if (token == "notes-primary") {
        return AudioEditingScheme::Scheme::NotesPrimary;
    }

    return AudioEditingScheme::Scheme::CorrectedF0Primary;
}

const char* toNoteNameModeToken(NoteNameMode noteNameMode)
{
    switch (noteNameMode) {
        case NoteNameMode::ShowAll: return "show-all";
        case NoteNameMode::COnly: return "c-only";
        case NoteNameMode::Hide: return "hide";
    }

    return "c-only";
}

NoteNameMode noteNameModeFromToken(const juce::String& token)
{
    if (token == "show-all") return NoteNameMode::ShowAll;
    if (token == "hide") return NoteNameMode::Hide;
    return NoteNameMode::COnly;
}

const char* toMouseTrailThemeToken(MouseTrailConfig::TrailTheme theme)
{
    switch (theme) {
        case MouseTrailConfig::TrailTheme::None: return "none";
        case MouseTrailConfig::TrailTheme::Classic: return "classic";
        case MouseTrailConfig::TrailTheme::Neon: return "neon";
        case MouseTrailConfig::TrailTheme::Fire: return "fire";
        case MouseTrailConfig::TrailTheme::Ocean: return "ocean";
        case MouseTrailConfig::TrailTheme::Galaxy: return "galaxy";
        case MouseTrailConfig::TrailTheme::CherryBlossom: return "cherry-blossom";
        case MouseTrailConfig::TrailTheme::Matrix: return "matrix";
    }

    return "classic";
}

MouseTrailConfig::TrailTheme mouseTrailThemeFromToken(const juce::String& token)
{
    if (token == "none") return MouseTrailConfig::TrailTheme::None;
    if (token == "classic") return MouseTrailConfig::TrailTheme::Classic;
    if (token == "neon") return MouseTrailConfig::TrailTheme::Neon;
    if (token == "fire") return MouseTrailConfig::TrailTheme::Fire;
    if (token == "ocean") return MouseTrailConfig::TrailTheme::Ocean;
    if (token == "galaxy") return MouseTrailConfig::TrailTheme::Galaxy;
    if (token == "cherry-blossom") return MouseTrailConfig::TrailTheme::CherryBlossom;
    if (token == "matrix") return MouseTrailConfig::TrailTheme::Matrix;
    return MouseTrailConfig::TrailTheme::Classic;
}

const char* toRenderingPriorityToken(RenderingPriority priority)
{
    switch (priority) {
        case RenderingPriority::GpuFirst: return "gpu-first";
        case RenderingPriority::CpuFirst: return "cpu-first";
    }
    return "gpu-first";
}

RenderingPriority renderingPriorityFromToken(const juce::String& token)
{
    if (token == "cpu-first") return RenderingPriority::CpuFirst;
    return RenderingPriority::GpuFirst;
}

KeyShortcutConfig::KeyShortcutSettings decodeShortcutSettings(const juce::PropertiesFile& properties)
{
    auto settings = KeyShortcutConfig::KeyShortcutSettings::getDefault();

    for (size_t index = 0; index < kShortcutStorageKeys.size(); ++index) {
        const auto stored = properties.getValue(kShortcutStorageKeys[index], {});
        if (stored.isEmpty()) {
            continue;
        }

        KeyShortcutConfig::ShortcutBinding decoded;
        if (KeyShortcutConfig::parseShortcutBinding(stored, decoded) && !decoded.bindings.empty()) {
            settings.bindings[index] = std::move(decoded);
        }
    }

    return settings;
}

AppPreferencesState loadStateFromProperties(const juce::PropertiesFile& properties)
{
    AppPreferencesState state;
    state.shared.language = languageFromToken(properties.getValue(kSharedLanguageKey, toLanguageToken(state.shared.language)));
    state.shared.theme = themeFromToken(properties.getValue(kSharedThemeKey, toThemeToken(state.shared.theme)));
    state.shared.audioEditingScheme = audioEditingSchemeFromToken(
        properties.getValue(kSharedAudioEditingSchemeKey, toAudioEditingSchemeToken(state.shared.audioEditingScheme)));
    state.shared.pianoRollVisualPreferences.noteNameMode = noteNameModeFromToken(
        properties.getValue(kSharedPianoRollNoteNameModeKey,
                            toNoteNameModeToken(state.shared.pianoRollVisualPreferences.noteNameMode)));
    state.shared.pianoRollVisualPreferences.showChunkBoundaries = properties.getBoolValue(
        kSharedPianoRollShowChunkBoundariesKey,
        state.shared.pianoRollVisualPreferences.showChunkBoundaries);
    state.shared.pianoRollVisualPreferences.showUnvoicedFrames = properties.getBoolValue(
        kSharedPianoRollShowUnvoicedFramesKey,
        state.shared.pianoRollVisualPreferences.showUnvoicedFrames);
    state.shared.zoomSensitivity.horizontalZoomFactor = static_cast<float>(
        properties.getDoubleValue(kSharedZoomHorizontalFactorKey, state.shared.zoomSensitivity.horizontalZoomFactor));
    state.shared.zoomSensitivity.verticalZoomFactor = static_cast<float>(
        properties.getDoubleValue(kSharedZoomVerticalFactorKey, state.shared.zoomSensitivity.verticalZoomFactor));
    state.shared.zoomSensitivity.scrollSpeed = static_cast<float>(
        properties.getDoubleValue(kSharedScrollSpeedKey, state.shared.zoomSensitivity.scrollSpeed));
    state.shared.renderingPriority = renderingPriorityFromToken(
        properties.getValue(kSharedRenderingPriorityKey,
                            toRenderingPriorityToken(state.shared.renderingPriority)));

    state.standalone.shortcuts = decodeShortcutSettings(properties);
    state.standalone.mouseTrailTheme = mouseTrailThemeFromToken(
        properties.getValue(kStandaloneMouseTrailThemeKey, toMouseTrailThemeToken(state.standalone.mouseTrailTheme)));
    return state;
}

void writeStateToProperties(juce::PropertiesFile& properties, const AppPreferencesState& state)
{
    properties.setValue(kSharedLanguageKey, toLanguageToken(state.shared.language));
    properties.setValue(kSharedThemeKey, toThemeToken(state.shared.theme));
    properties.setValue(kSharedAudioEditingSchemeKey, toAudioEditingSchemeToken(state.shared.audioEditingScheme));
    properties.setValue(kSharedPianoRollNoteNameModeKey,
                        toNoteNameModeToken(state.shared.pianoRollVisualPreferences.noteNameMode));
    properties.setValue(kSharedPianoRollShowChunkBoundariesKey,
                        state.shared.pianoRollVisualPreferences.showChunkBoundaries);
    properties.setValue(kSharedPianoRollShowUnvoicedFramesKey,
                        state.shared.pianoRollVisualPreferences.showUnvoicedFrames);
    properties.setValue(kSharedZoomHorizontalFactorKey, static_cast<double>(state.shared.zoomSensitivity.horizontalZoomFactor));
    properties.setValue(kSharedZoomVerticalFactorKey, static_cast<double>(state.shared.zoomSensitivity.verticalZoomFactor));
    properties.setValue(kSharedScrollSpeedKey, static_cast<double>(state.shared.zoomSensitivity.scrollSpeed));
    properties.setValue(kSharedRenderingPriorityKey,
                        toRenderingPriorityToken(state.shared.renderingPriority));
    properties.setValue(kStandaloneMouseTrailThemeKey, toMouseTrailThemeToken(state.standalone.mouseTrailTheme));

    for (size_t index = 0; index < kShortcutStorageKeys.size(); ++index) {
        properties.setValue(kShortcutStorageKeys[index], KeyShortcutConfig::toCanonicalString(state.standalone.shortcuts.bindings[index]));
    }
}

} // namespace

AppPreferences::AppPreferences()
    : AppPreferences(StorageOptions{})
{
}

AppPreferences::AppPreferences(const StorageOptions& storageOptions)
    : storageOptions_(storageOptions)
{
    initialiseStorage();
    load();
}

AppPreferencesState AppPreferences::getState() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void AppPreferences::load()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    auto* userSettings = properties_.getUserSettings();
    jassert(userSettings != nullptr);
    if (userSettings == nullptr) {
        return;
    }

    state_ = loadStateFromProperties(*userSettings);
}

void AppPreferences::save()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    saveLocked();
}

void AppPreferences::flush()
{
    save();
}

void AppPreferences::setLanguage(Language language)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.language = language;
    saveLocked();
}

void AppPreferences::setTheme(ThemeId theme)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.theme = theme;
    saveLocked();
}

void AppPreferences::setAudioEditingScheme(AudioEditingScheme::Scheme scheme)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.audioEditingScheme = scheme;
    saveLocked();
}

void AppPreferences::setPianoRollVisualPreferences(const PianoRollVisualPreferences& visualPreferences)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.pianoRollVisualPreferences = visualPreferences;
    saveLocked();
}

void AppPreferences::setNoteNameMode(NoteNameMode noteNameMode)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.pianoRollVisualPreferences.noteNameMode = noteNameMode;
    saveLocked();
}

void AppPreferences::setShowChunkBoundaries(bool shouldShow)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.pianoRollVisualPreferences.showChunkBoundaries = shouldShow;
    saveLocked();
}

void AppPreferences::setShowUnvoicedFrames(bool shouldShow)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.pianoRollVisualPreferences.showUnvoicedFrames = shouldShow;
    saveLocked();
}

void AppPreferences::setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& zoomSensitivity)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.zoomSensitivity = zoomSensitivity;
    saveLocked();
}

void AppPreferences::setStandaloneShortcuts(const KeyShortcutConfig::KeyShortcutSettings& shortcuts)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.standalone.shortcuts = shortcuts;
    saveLocked();
}

void AppPreferences::setRenderingPriority(RenderingPriority priority)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.renderingPriority = priority;
    saveLocked();
}

void AppPreferences::setMouseTrailTheme(MouseTrailConfig::TrailTheme theme)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.standalone.mouseTrailTheme = theme;
    saveLocked();
}

void AppPreferences::initialiseStorage()
{
    const auto settingsDirectory = resolveSettingsDirectory(storageOptions_);
    settingsDirectory.createDirectory();

    processLock_ = std::make_unique<juce::InterProcessLock>(buildProcessLockName(storageOptions_));

    const auto options = makePropertiesOptions(storageOptions_, processLock_.get());
    properties_.setStorageParameters(options);
}

void AppPreferences::saveLocked()
{
    auto* userSettings = properties_.getUserSettings();
    jassert(userSettings != nullptr);
    if (userSettings == nullptr) {
        return;
    }

    writeStateToProperties(*userSettings, state_);
    userSettings->saveIfNeeded();
}

} // namespace OpenTune
