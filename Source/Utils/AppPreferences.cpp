#include "AppPreferences.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OpenTune {

namespace {

constexpr const char* kSharedLanguageKey = "shared.language";
constexpr const char* kSharedThemeKey = "shared.theme.activeTheme";
constexpr const char* kSharedAudioEditingSchemeKey = "shared.audioEditing.scheme";
constexpr const char* kSharedPianoRollNoteNameModeKey = "shared.pianoRoll.noteNameMode";
constexpr const char* kSharedPianoRollShowUnvoicedFramesKey = "shared.pianoRoll.showUnvoicedFrames";
constexpr const char* kSharedPianoRollBackgroundBrightnessKey = "shared.pianoRoll.backgroundBrightness";
constexpr const char* kSharedZoomHorizontalFactorKey = "shared.zoom.horizontalFactor";
constexpr const char* kSharedZoomVerticalFactorKey = "shared.zoom.verticalFactor";
constexpr const char* kSharedScrollSpeedKey = "shared.scroll.speed";
constexpr const char* kStandaloneMouseTrailThemeKey = "standalone.mouseTrail.theme";
constexpr const char* kStandaloneCursorStyleKey = "standalone.cursorStyle";
constexpr const char* kSharedRenderingPriorityKey = "shared.rendering.priority";
constexpr const char* kSharedVocoderWeightKey = "shared.rendering.vocoderWeight";
constexpr const char* kSharedExperimentalFeaturesEnabledKey = "shared.features.experimentalEnabled";
constexpr const char* kSharedExperimentalReferenceAlignKey = "shared.align.experimental";
constexpr const char* kSharedRecentProjectsKey = "shared.recentProjects";
constexpr const char* kSharedSnapEnabledKey = "shared.snap.enabled";
constexpr const char* kSharedSnapModeKey = "shared.snap.mode";
constexpr const char* kSharedTrackColorModeKey = "shared.trackColor.mode";
constexpr const char* kSharedLightPitchCorrectionEnabledKey = "shared.rendering.lightPitchCorrection";
constexpr const char* kSharedF0ModelTypeKey = "shared.f0.modelType";
constexpr const char* kSharedTimelineDisplayModeKey = "shared.timeline.displayMode";
constexpr const char* kSharedTuningHzKey = "shared.tuning.hz";
constexpr const char* kSharedGridStyleKey = "shared.pianoRoll.gridStyle";
constexpr const char* kSharedEqSuppressRemoveConfirmationKey = "shared.eq.suppressRemoveConfirmation";

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
        case ThemeId::Overdose: return "overdose";
    }

    return "aurora";
}

ThemeId themeFromToken(const juce::String& token)
{
    if (token == "blue-breeze") return ThemeId::BlueBreeze;
    if (token == "dark-blue-grey") return ThemeId::DarkBlueGrey;
    if (token == "overdose") return ThemeId::Overdose;
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

static juce::String toVocoderWeightToken(const VocoderModelWeight& w)
{
    return juce::String(w);
}

static VocoderModelWeight fromVocoderWeightToken(const juce::String& token)
{
    // 旧版枚举 token 迁移到文件名标识
    if (token == "community") return kVocoderWeightCommunity;
    if (token == "coulin9-v4") return kVocoderWeightCoulin9;
    if (token.isEmpty()) return kDefaultVocoderWeight;
    return token.toStdString();
}

static juce::String toF0ModelTypeToken(F0ModelType type)
{
    switch (type) {
        case F0ModelType::RMVPE: return "rmvpe";
        case F0ModelType::FCPE: return "fcpe";
    }
    return "fcpe";  // Default to FCPE (RMVPE disabled)
}

static F0ModelType fromF0ModelTypeToken(const juce::String& token)
{
    if (token == "fcpe") return F0ModelType::FCPE;
    if (token == "rmvpe") return F0ModelType::RMVPE;
    return F0ModelType::FCPE;  // Default to FCPE (RMVPE disabled)
}

static juce::String toExperimentalRefAlignModeToken(ExperimentalReferenceAlignMode mode)
{
    switch (mode) {
        case ExperimentalReferenceAlignMode::StandardAuto: return "standard-auto";
        case ExperimentalReferenceAlignMode::Game:         return "game";
    }
    return "standard-auto";
}

static ExperimentalReferenceAlignMode fromExperimentalRefAlignModeToken(const juce::String& token)
{
    if (token == "game") {
        return ExperimentalReferenceAlignMode::Game;
    }
    return ExperimentalReferenceAlignMode::StandardAuto;
}

const char* toTrackColorModeToken(TrackColorMode mode)
{
    switch (mode) {
        case TrackColorMode::Random: return "random";
        case TrackColorMode::Custom: return "custom";
    }
    return "random";
}

TrackColorMode trackColorModeFromToken(const juce::String& token)
{
    if (token == "custom") return TrackColorMode::Custom;
    return TrackColorMode::Random;
}

KeyShortcutConfig::KeyShortcutSettings decodeShortcutSettings(const juce::PropertiesFile& properties)
{
    auto settings = KeyShortcutConfig::KeyShortcutSettings::getDefault();

    for (const auto& info : KeyShortcutConfig::kShortcutInfos) {
        const auto stored = properties.getValue(info.storageKey, {});
        if (stored.isEmpty()) {
            continue;
        }

        KeyShortcutConfig::ShortcutBinding decoded;
        if (KeyShortcutConfig::parseShortcutBinding(stored, decoded) && !decoded.bindings.empty()) {
            settings.bindings[static_cast<size_t>(info.id)] = std::move(decoded);
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
    state.shared.pianoRollVisualPreferences.showUnvoicedFrames = properties.getBoolValue(
        kSharedPianoRollShowUnvoicedFramesKey,
        state.shared.pianoRollVisualPreferences.showUnvoicedFrames);
    state.shared.pianoRollVisualPreferences.backgroundBrightness = static_cast<float>(
        properties.getDoubleValue(kSharedPianoRollBackgroundBrightnessKey,
                                  state.shared.pianoRollVisualPreferences.backgroundBrightness));
    state.shared.zoomSensitivity.horizontalZoomFactor = static_cast<float>(
        properties.getDoubleValue(kSharedZoomHorizontalFactorKey, state.shared.zoomSensitivity.horizontalZoomFactor));
    state.shared.zoomSensitivity.verticalZoomFactor = static_cast<float>(
        properties.getDoubleValue(kSharedZoomVerticalFactorKey, state.shared.zoomSensitivity.verticalZoomFactor));
    state.shared.zoomSensitivity.scrollSpeed = static_cast<float>(
        properties.getDoubleValue(kSharedScrollSpeedKey, state.shared.zoomSensitivity.scrollSpeed));
    state.shared.tuning.tuningHz = static_cast<float>(
        properties.getDoubleValue(kSharedTuningHzKey, state.shared.tuning.tuningHz));
    state.shared.gridStyle = static_cast<PianoGridStyle>(
        properties.getIntValue(kSharedGridStyleKey, static_cast<int>(state.shared.gridStyle)));
    state.shared.renderingPriority = renderingPriorityFromToken(
        properties.getValue(kSharedRenderingPriorityKey,
                            toRenderingPriorityToken(state.shared.renderingPriority)));
    state.shared.vocoderModelWeight = fromVocoderWeightToken(
        properties.getValue(kSharedVocoderWeightKey,
                            toVocoderWeightToken(state.shared.vocoderModelWeight)));
    state.shared.experimentalFeaturesEnabled = properties.getBoolValue(
        kSharedExperimentalFeaturesEnabledKey,
        state.shared.experimentalFeaturesEnabled);
    state.shared.lightPitchCorrectionEnabled = properties.getBoolValue(
        kSharedLightPitchCorrectionEnabledKey,
        state.shared.lightPitchCorrectionEnabled);
    state.shared.f0ModelType = fromF0ModelTypeToken(
        properties.getValue(kSharedF0ModelTypeKey, toF0ModelTypeToken(state.shared.f0ModelType)));
    state.shared.suppressEqRemoveConfirmation = properties.getBoolValue(
        kSharedEqSuppressRemoveConfirmationKey,
        state.shared.suppressEqRemoveConfirmation);
    state.shared.experimentalReferenceAlignMode = fromExperimentalRefAlignModeToken(
        properties.getValue(kSharedExperimentalReferenceAlignKey,
                            toExperimentalRefAlignModeToken(state.shared.experimentalReferenceAlignMode)));
    state.shared.snap.enabled = properties.getBoolValue(kSharedSnapEnabledKey, false);
    state.shared.snap.mode = static_cast<SnapSettings::Mode>(
        properties.getIntValue(kSharedSnapModeKey, static_cast<int>(SnapSettings::Mode::Off)));
    state.shared.trackColorMode = trackColorModeFromToken(
        properties.getValue(kSharedTrackColorModeKey, toTrackColorModeToken(state.shared.trackColorMode)));
    {
        const int modeVal = properties.getIntValue(kSharedTimelineDisplayModeKey, static_cast<int>(TimelineDisplayMode::Time));
        state.shared.timelineDisplayMode = (modeVal == static_cast<int>(TimelineDisplayMode::Bars))
            ? TimelineDisplayMode::Bars : TimelineDisplayMode::Time;
    }

    state.shared.shortcuts = decodeShortcutSettings(properties);
    state.standalone.mouseTrailTheme = mouseTrailThemeFromToken(
        properties.getValue(kStandaloneMouseTrailThemeKey, toMouseTrailThemeToken(state.standalone.mouseTrailTheme)));
    state.standalone.cursorStyle = static_cast<CursorStyleId>(
        properties.getIntValue(kStandaloneCursorStyleKey, static_cast<int>(state.standalone.cursorStyle)));

    const auto recentRaw = properties.getValue(kSharedRecentProjectsKey, "");
    if (recentRaw.isNotEmpty()) {
        state.shared.recentProjects.clear();
        juce::StringArray parts;
        parts.addTokens(recentRaw, "|", "");
        for (const auto& path : parts) {
            state.shared.recentProjects.push_back(path);
        }
    }

    return state;
}

void writeStateToProperties(juce::PropertiesFile& properties, const AppPreferencesState& state)
{
    properties.setValue(kSharedLanguageKey, toLanguageToken(state.shared.language));
    properties.setValue(kSharedThemeKey, toThemeToken(state.shared.theme));
    properties.setValue(kSharedAudioEditingSchemeKey, toAudioEditingSchemeToken(state.shared.audioEditingScheme));
    properties.setValue(kSharedPianoRollNoteNameModeKey,
                        toNoteNameModeToken(state.shared.pianoRollVisualPreferences.noteNameMode));
    properties.setValue(kSharedPianoRollShowUnvoicedFramesKey,
                        state.shared.pianoRollVisualPreferences.showUnvoicedFrames);
    properties.setValue(kSharedPianoRollBackgroundBrightnessKey,
                        static_cast<double>(state.shared.pianoRollVisualPreferences.backgroundBrightness));
    properties.setValue(kSharedZoomHorizontalFactorKey, static_cast<double>(state.shared.zoomSensitivity.horizontalZoomFactor));
    properties.setValue(kSharedZoomVerticalFactorKey, static_cast<double>(state.shared.zoomSensitivity.verticalZoomFactor));
    properties.setValue(kSharedScrollSpeedKey, static_cast<double>(state.shared.zoomSensitivity.scrollSpeed));
    properties.setValue(kSharedTuningHzKey, static_cast<double>(state.shared.tuning.tuningHz));
    properties.setValue(kSharedGridStyleKey, static_cast<int>(state.shared.gridStyle));
    properties.setValue(kSharedRenderingPriorityKey,
                        toRenderingPriorityToken(state.shared.renderingPriority));
    properties.setValue(kSharedVocoderWeightKey,
                        toVocoderWeightToken(state.shared.vocoderModelWeight));
    properties.setValue(kSharedExperimentalFeaturesEnabledKey,
                        state.shared.experimentalFeaturesEnabled);
    properties.setValue(kSharedLightPitchCorrectionEnabledKey,
                        state.shared.lightPitchCorrectionEnabled);
    properties.setValue(kSharedF0ModelTypeKey,
                        toF0ModelTypeToken(state.shared.f0ModelType));
    properties.setValue(kSharedEqSuppressRemoveConfirmationKey,
                        state.shared.suppressEqRemoveConfirmation);
    properties.setValue(kSharedExperimentalReferenceAlignKey,
                        toExperimentalRefAlignModeToken(state.shared.experimentalReferenceAlignMode));
    properties.setValue(kSharedSnapEnabledKey, state.shared.snap.enabled);
    properties.setValue(kSharedSnapModeKey, static_cast<int>(state.shared.snap.mode));
    properties.setValue(kSharedTrackColorModeKey, toTrackColorModeToken(state.shared.trackColorMode));
    properties.setValue(kSharedTimelineDisplayModeKey, static_cast<int>(state.shared.timelineDisplayMode));
    properties.setValue(kStandaloneMouseTrailThemeKey, toMouseTrailThemeToken(state.standalone.mouseTrailTheme));
    properties.setValue(kStandaloneCursorStyleKey, static_cast<int>(state.standalone.cursorStyle));

    juce::StringArray recentPaths;
    for (const auto& path : state.shared.recentProjects) {
        recentPaths.add(path);
    }
    properties.setValue(kSharedRecentProjectsKey, recentPaths.joinIntoString("|"));

    for (const auto& info : KeyShortcutConfig::kShortcutInfos) {
        properties.setValue(info.storageKey,
                            KeyShortcutConfig::toCanonicalString(
                                state.shared.shortcuts.bindings[static_cast<size_t>(info.id)]));
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
    TuningConfig::currentTuningHz() = state_.shared.tuning.tuningHz;
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

void AppPreferences::setShowUnvoicedFrames(bool shouldShow)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.pianoRollVisualPreferences.showUnvoicedFrames = shouldShow;
    saveLocked();
}

void AppPreferences::setBackgroundBrightness(float brightness)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.pianoRollVisualPreferences.backgroundBrightness = brightness;
    saveLocked();
}

void AppPreferences::setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& zoomSensitivity)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.zoomSensitivity = zoomSensitivity;
    saveLocked();
}

void AppPreferences::setTuning(const TuningConfig::TuningSettings& tuning)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.tuning = tuning;
    TuningConfig::currentTuningHz() = tuning.tuningHz;
    saveLocked();
}

void AppPreferences::setShortcuts(const KeyShortcutConfig::KeyShortcutSettings& shortcuts)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.shortcuts = shortcuts;
    saveLocked();
}

void AppPreferences::setSuppressEqRemoveConfirmation(bool suppress)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.suppressEqRemoveConfirmation = suppress;
    saveLocked();
}

void AppPreferences::setRenderingPriority(RenderingPriority priority)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.renderingPriority = priority;
    saveLocked();
}

void AppPreferences::setVocoderModelWeight(const VocoderModelWeight& weight)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.vocoderModelWeight = weight;
    saveLocked();
}

void AppPreferences::setExperimentalFeaturesEnabled(bool enabled)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.experimentalFeaturesEnabled = enabled;
    saveLocked();
}

void AppPreferences::setExperimentalReferenceAlignMode(ExperimentalReferenceAlignMode mode)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.experimentalReferenceAlignMode = mode;
    saveLocked();
}

void AppPreferences::setMouseTrailTheme(MouseTrailConfig::TrailTheme theme)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.standalone.mouseTrailTheme = theme;
    saveLocked();
}

void AppPreferences::setCursorStyle(CursorStyleId style)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.standalone.cursorStyle = style;
    saveLocked();
}

void AppPreferences::setSnapSettings(const SnapSettings& snap)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.snap = snap;
    saveLocked();
}

SnapSettings AppPreferences::getSnapSettings() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_.shared.snap;
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

std::vector<juce::String> AppPreferences::getRecentProjects() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_.shared.recentProjects;
}

void AppPreferences::pushRecentProject(const juce::String& projectPath)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    auto& list = state_.shared.recentProjects;
    // Remove existing entry
    list.erase(std::remove(list.begin(), list.end(), projectPath), list.end());
    // Insert at front
    list.insert(list.begin(), projectPath);
    // Trim to 10
    if (list.size() > 10) {
        list.resize(10);
    }
    saveLocked();
}

void AppPreferences::clearRecentProjects()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.recentProjects.clear();
    saveLocked();
}

void AppPreferences::setTrackColorMode(TrackColorMode mode)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.trackColorMode = mode;
    saveLocked();
}

TrackColorMode AppPreferences::getTrackColorMode() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_.shared.trackColorMode;
}

void AppPreferences::setLightPitchCorrectionEnabled(bool enabled)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.lightPitchCorrectionEnabled = enabled;
    saveLocked();
}

void AppPreferences::setF0ModelType(F0ModelType type)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.f0ModelType = type;
    saveLocked();
}

F0ModelType AppPreferences::getF0ModelType() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_.shared.f0ModelType;
}

void AppPreferences::setTimelineDisplayMode(TimelineDisplayMode mode)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.timelineDisplayMode = mode;
    saveLocked();
}

void AppPreferences::setGridStyle(PianoGridStyle gridStyle)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.gridStyle = gridStyle;
    saveLocked();
}

PianoGridStyle AppPreferences::getGridStyle() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_.shared.gridStyle;
}

TimelineDisplayMode AppPreferences::getTimelineDisplayMode() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_.shared.timelineDisplayMode;
}

} // namespace OpenTune
