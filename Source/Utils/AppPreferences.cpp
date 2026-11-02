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
constexpr const char* kSharedPianoRollShowPianoKeyboardKey = "shared.pianoRoll.showPianoKeyboard";
constexpr const char* kSharedPianoRollScaleAssistEnabledKey = "shared.pianoRoll.scaleAssistEnabled";
constexpr const char* kSharedPianoRollBackgroundBrightnessKey = "shared.pianoRoll.backgroundBrightness";
constexpr const char* kSharedPianoRollCorrectedF0ColourKey = "shared.pianoRoll.correctedF0Colour";
constexpr const char* kSharedPianoRollOriginalF0ColourKey = "shared.pianoRoll.originalF0Colour";
constexpr const char* kSharedZoomHorizontalFactorKey = "shared.zoom.horizontalFactor";
constexpr const char* kSharedZoomVerticalFactorKey = "shared.zoom.verticalFactor";
constexpr const char* kSharedScrollSpeedKey = "shared.scroll.speed";
constexpr const char* kStandaloneMouseTrailThemeKey = "standalone.mouseTrail.theme";
constexpr const char* kStandaloneCursorStyleKey = "standalone.cursorStyle";
constexpr const char* kSharedRenderingPriorityKey = "shared.rendering.priority";
constexpr const char* kSharedVocoderWeightKey = "shared.rendering.vocoderWeight";
constexpr const char* kSharedExperimentalFeaturesEnabledKey = "shared.features.experimentalEnabled";
constexpr const char* kSharedRecentProjectsKey = "shared.recentProjects";
constexpr const char* kSharedSnapEnabledKey = "shared.snap.enabled";
constexpr const char* kSharedSnapModeKey = "shared.snap.mode";
constexpr const char* kSharedTrackColorModeKey = "shared.trackColor.mode";
constexpr const char* kSharedLightPitchCorrectionEnabledKey = "shared.rendering.lightPitchCorrection";
constexpr const char* kSharedF0ModelTypeKey = "shared.f0.modelType";
constexpr const char* kSharedTimelineDisplayModeKey = "shared.timeline.displayMode";
constexpr const char* kSharedTuningHzKey = "shared.tuning.hz";
constexpr const char* kSharedGridStyleKey = "shared.pianoRoll.gridStyle";
constexpr const char* kSharedWholeNoteMoveSnapModeKey = "shared.pianoRoll.wholeNoteMoveSnapMode";
constexpr const char* kSharedPitchLaneVisualModeKey = "shared.pianoRoll.pitchLaneVisualMode";
constexpr const char* kSharedEqSuppressRemoveConfirmationKey = "shared.eq.suppressRemoveConfirmation";
constexpr const char* kSharedOnboardingShownKey = "shared.onboardingShown";
constexpr const char* kSharedDemoTourConsumedKey = "shared.demoTourConsumed";
constexpr const char* kPluginEditorSizeKey = "defaultSize";
constexpr const char* kShortcutSchemaVersionKey = "shared.shortcuts.schemaVersion";
constexpr int kShortcutMouseSchemaVersion = 1;
constexpr int kShortcutMacModifierSchemaVersion = 2;
constexpr int kShortcutSchemaVersion = 3;

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
        case F0ModelType::FCPE: return "fcpe";
    }
    return "fcpe";
}

static F0ModelType fromF0ModelTypeToken(const juce::String& token)
{
    if (token == "fcpe") return F0ModelType::FCPE;
    if (token == "rmvpe") return F0ModelType::FCPE;  // legacy: migrate to FCPE
    return F0ModelType::FCPE;
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
    const int storedSchemaVersion = properties.getIntValue(kShortcutSchemaVersionKey, 0);
    const bool migrateMouseDefaults = storedSchemaVersion < kShortcutMouseSchemaVersion;
#if JUCE_MAC || JUCE_IOS
    const bool migrateMacDefaultShortcuts = storedSchemaVersion < kShortcutMacModifierSchemaVersion;
    const bool restoreMacSpacePlay = storedSchemaVersion < kShortcutSchemaVersion;
#endif

    for (const auto& info : KeyShortcutConfig::kShortcutInfos) {
        if (!properties.containsKey(info.storageKey))
            continue;

        const auto stored = properties.getValue(info.storageKey, {});
        if (stored.isEmpty()) {
            settings.bindings[static_cast<size_t>(info.id)].bindings.clear();
            continue;
        }

        KeyShortcutConfig::ShortcutBinding decoded;
        if (KeyShortcutConfig::parseShortcutBinding(stored, decoded)) {
            if (migrateMouseDefaults) {
                for (const auto& defaultBinding : info.defaultBindings) {
                    if (defaultBinding.type != KeyShortcutConfig::BindingType::Keyboard)
                        decoded.addBinding(defaultBinding);
                }
            }
#if JUCE_MAC || JUCE_IOS
            if (migrateMacDefaultShortcuts) {
                const auto zoomModifiers = juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier;
                const auto transportModifiers = zoomModifiers | juce::ModifierKeys::shiftModifier;
                const auto replaceBinding = [&decoded](const KeyShortcutConfig::KeyBinding& oldBinding,
                                                       const KeyShortcutConfig::KeyBinding& newBinding) {
                    if (decoded.hasBinding(oldBinding)) {
                        decoded.removeBinding(oldBinding);
                        decoded.addBinding(newBinding);
                    }
                };

                if (info.id == KeyShortcutConfig::ShortcutId::Delete)
                    decoded.removeBinding({ '1', {} });
                else if (info.id == KeyShortcutConfig::ShortcutId::ToolHandDraw)
                    replaceBinding({ '5', {} }, { 'H', {} });
                else if (info.id == KeyShortcutConfig::ShortcutId::ToolAutoTune)
                    replaceBinding({ '6', {} }, { 'A', {} });
                else if (info.id == KeyShortcutConfig::ShortcutId::PlayPause)
                    replaceBinding({ juce::KeyPress::spaceKey, {} },
                                   { juce::KeyPress::spaceKey, transportModifiers });
                else if (info.id == KeyShortcutConfig::ShortcutId::Stop)
                    replaceBinding({ '.', {} }, { '.', transportModifiers });
                else if (info.id == KeyShortcutConfig::ShortcutId::PlayFromStart)
                    replaceBinding({ '.', juce::ModifierKeys::commandModifier },
                                   { juce::KeyPress::returnKey, transportModifiers });
                else if (info.id == KeyShortcutConfig::ShortcutId::ZoomHorizontalIn)
                    replaceBinding({ juce::KeyPress::rightKey, zoomModifiers },
                                   { juce::KeyPress::rightKey, transportModifiers });
                else if (info.id == KeyShortcutConfig::ShortcutId::ZoomHorizontalOut)
                    replaceBinding({ juce::KeyPress::leftKey, zoomModifiers },
                                   { juce::KeyPress::leftKey, transportModifiers });
                else if (info.id == KeyShortcutConfig::ShortcutId::ZoomVerticalIn)
                    replaceBinding({ juce::KeyPress::upKey, zoomModifiers },
                                   { juce::KeyPress::upKey, transportModifiers });
                else if (info.id == KeyShortcutConfig::ShortcutId::ZoomVerticalOut)
                    replaceBinding({ juce::KeyPress::downKey, zoomModifiers },
                                   { juce::KeyPress::downKey, transportModifiers });
            }
            if (restoreMacSpacePlay && info.id == KeyShortcutConfig::ShortcutId::PlayPause) {
                const auto previousDefault = juce::ModifierKeys::commandModifier
                    | juce::ModifierKeys::altModifier
                    | juce::ModifierKeys::shiftModifier;
                const KeyShortcutConfig::KeyBinding previousPlay(
                    juce::KeyPress::spaceKey, previousDefault);
                if (decoded.hasBinding(previousPlay)) {
                    decoded.removeBinding(previousPlay);
                    decoded.addBinding({ juce::KeyPress::spaceKey, {} });
                }
            }
#endif
            settings.bindings[static_cast<size_t>(info.id)] = std::move(decoded);
        }
    }

    return settings;
}

void writeShortcutSettingsToProperties(
    juce::PropertiesFile& properties,
    const KeyShortcutConfig::KeyShortcutSettings& shortcuts)
{
    for (const auto& info : KeyShortcutConfig::kShortcutInfos) {
        properties.setValue(info.storageKey,
                            KeyShortcutConfig::toCanonicalString(
                                shortcuts.bindings[static_cast<size_t>(info.id)]));
    }
    properties.setValue(kShortcutSchemaVersionKey, kShortcutSchemaVersion);
}

PluginEditorSize readDefaultPluginEditorSize(const juce::PropertiesFile& properties)
{
    const auto defaultSize = PluginEditorSize{};
    const auto stored = properties.getValue(kPluginEditorSizeKey,
                                            juce::String(defaultSize.width) + ","
                                                + juce::String(defaultSize.height));
    const int separator = stored.indexOfChar(',');
    if (separator < 0)
        return defaultSize;

    return { stored.substring(0, separator).getIntValue(),
             stored.substring(separator + 1).getIntValue() };
}

#if JUCE_IOS
Language languageFromSystemLocale()
{
    const auto code = juce::SystemStats::getDisplayLanguage().toLowerCase();
    if (code.startsWith("zh")) return Language::Chinese;
    if (code.startsWith("ja")) return Language::Japanese;
    if (code.startsWith("ru")) return Language::Russian;
    if (code.startsWith("es")) return Language::Spanish;
    return Language::English;
}
#endif

AppPreferencesState loadStateFromProperties(const juce::PropertiesFile& properties)
{
    AppPreferencesState state;
#if JUCE_IOS
    // 首次启动跟随设备语言；不支持的语言回退 English。
    if (!properties.containsKey(kSharedLanguageKey))
        state.shared.language = languageFromSystemLocale();
#endif
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
    const bool hasKeyboardKey = properties.containsKey(kSharedPianoRollShowPianoKeyboardKey);
    const bool hasScaleAssistKey = properties.containsKey(kSharedPianoRollScaleAssistEnabledKey);
    const int legacyMode = properties.getIntValue(kSharedPitchLaneVisualModeKey, 1);
    const bool legacyKeyboard = legacyMode == 1;
    const bool legacyScaleAssist = legacyMode == 0;
    state.shared.pianoRollVisualPreferences.showPianoKeyboard = hasKeyboardKey
        ? properties.getBoolValue(kSharedPianoRollShowPianoKeyboardKey, true) : legacyKeyboard;
    state.shared.pianoRollVisualPreferences.scaleAssistEnabled = hasScaleAssistKey
        ? properties.getBoolValue(kSharedPianoRollScaleAssistEnabledKey, false) : legacyScaleAssist;
    state.shared.pianoRollVisualPreferences.backgroundBrightness = static_cast<float>(
        properties.getDoubleValue(kSharedPianoRollBackgroundBrightnessKey,
                                  state.shared.pianoRollVisualPreferences.backgroundBrightness));
    state.shared.pianoRollVisualPreferences.correctedF0Colour = juce::Colour::fromString(
        properties.getValue(kSharedPianoRollCorrectedF0ColourKey,
                            juce::Colour(state.shared.pianoRollVisualPreferences.correctedF0Colour).toString()))
        .withAlpha(1.0f).getARGB();
    state.shared.pianoRollVisualPreferences.originalF0Colour = juce::Colour::fromString(
        properties.getValue(kSharedPianoRollOriginalF0ColourKey,
                            juce::Colour(state.shared.pianoRollVisualPreferences.originalF0Colour).toString()))
        .withAlpha(1.0f).getARGB();
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
    state.shared.wholeNoteMoveSnapMode = static_cast<WholeNoteMoveSnapMode>(
        properties.getIntValue(kSharedWholeNoteMoveSnapModeKey,
                               static_cast<int>(state.shared.wholeNoteMoveSnapMode)));
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
    state.shared.onboardingShown = properties.getBoolValue(kSharedOnboardingShownKey, state.shared.onboardingShown);
    state.shared.demoTourConsumed = properties.getBoolValue(kSharedDemoTourConsumedKey, state.shared.demoTourConsumed);
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
    properties.setValue(kSharedPianoRollShowPianoKeyboardKey,
                        state.shared.pianoRollVisualPreferences.showPianoKeyboard);
    properties.setValue(kSharedPianoRollScaleAssistEnabledKey,
                        state.shared.pianoRollVisualPreferences.scaleAssistEnabled);
    properties.setValue(kSharedPianoRollBackgroundBrightnessKey,
                        static_cast<double>(state.shared.pianoRollVisualPreferences.backgroundBrightness));
    properties.setValue(kSharedPianoRollCorrectedF0ColourKey,
                        juce::Colour(state.shared.pianoRollVisualPreferences.correctedF0Colour).toString());
    properties.setValue(kSharedPianoRollOriginalF0ColourKey,
                        juce::Colour(state.shared.pianoRollVisualPreferences.originalF0Colour).toString());
    properties.setValue(kSharedZoomHorizontalFactorKey, static_cast<double>(state.shared.zoomSensitivity.horizontalZoomFactor));
    properties.setValue(kSharedZoomVerticalFactorKey, static_cast<double>(state.shared.zoomSensitivity.verticalZoomFactor));
    properties.setValue(kSharedScrollSpeedKey, static_cast<double>(state.shared.zoomSensitivity.scrollSpeed));
    properties.setValue(kSharedTuningHzKey, static_cast<double>(state.shared.tuning.tuningHz));
    properties.setValue(kSharedGridStyleKey, static_cast<int>(state.shared.gridStyle));
    properties.setValue(kSharedWholeNoteMoveSnapModeKey, static_cast<int>(state.shared.wholeNoteMoveSnapMode));
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
    properties.setValue(kSharedOnboardingShownKey, state.shared.onboardingShown);
    properties.setValue(kSharedDemoTourConsumedKey, state.shared.demoTourConsumed);
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

    writeShortcutSettingsToProperties(properties, state.shared.shortcuts);
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

    const bool migrateShortcutSchema = userSettings->getIntValue(kShortcutSchemaVersionKey, 0)
        < kShortcutSchemaVersion;
    const bool hasLegacyVisualPreference = userSettings->containsKey(kSharedPitchLaneVisualModeKey);
    state_ = loadStateFromProperties(*userSettings);
    if (migrateShortcutSchema)
        writeShortcutSettingsToProperties(*userSettings, state_.shared.shortcuts);
    if (hasLegacyVisualPreference) {
        const bool needsVisualPreferenceMigration =
            !userSettings->containsKey(kSharedPianoRollShowPianoKeyboardKey)
            || !userSettings->containsKey(kSharedPianoRollScaleAssistEnabledKey);
        if (needsVisualPreferenceMigration)
            writeStateToProperties(*userSettings, state_);
        userSettings->removeValue(kSharedPitchLaneVisualModeKey);
    }
    if (migrateShortcutSchema || hasLegacyVisualPreference)
        userSettings->saveIfNeeded();
    TuningConfig::setCurrentTuningHz(state_.shared.tuning.tuningHz);
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

void AppPreferences::setOnboardingShown(bool shown)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.onboardingShown = shown;
    saveLocked();
}

void AppPreferences::setDemoTourConsumed(bool consumed)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_.shared.demoTourConsumed == consumed)
        return;
    state_.shared.demoTourConsumed = consumed;
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

void AppPreferences::setNoteNameMode(NoteNameMode noteNameMode)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.pianoRollVisualPreferences.noteNameMode = noteNameMode;
    saveLocked();
}

void AppPreferences::setShowPianoKeyboard(bool shouldShow)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.pianoRollVisualPreferences.showPianoKeyboard = shouldShow;
    saveLocked();
}

void AppPreferences::setScaleAssistEnabled(bool enabled)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.pianoRollVisualPreferences.scaleAssistEnabled = enabled;
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

void AppPreferences::setCorrectedF0Colour(std::uint32_t colour)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.pianoRollVisualPreferences.correctedF0Colour = juce::Colour(colour).withAlpha(1.0f).getARGB();
    saveLocked();
}

void AppPreferences::setOriginalF0Colour(std::uint32_t colour)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.pianoRollVisualPreferences.originalF0Colour = juce::Colour(colour).withAlpha(1.0f).getARGB();
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
    TuningConfig::setCurrentTuningHz(tuning.tuningHz);
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

    auto editorSizeStorageOptions = storageOptions_;
    editorSizeStorageOptions.fileName = resolveSettingsFile(storageOptions_).getFileName() + ".editor-size";
    pluginEditorSizeProperties_ = std::make_unique<juce::PropertiesFile>(
        makePropertiesOptions(editorSizeStorageOptions, processLock_.get()));
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

void AppPreferences::setWholeNoteMoveSnapMode(WholeNoteMoveSnapMode mode)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.shared.wholeNoteMoveSnapMode = mode;
    saveLocked();
}

WholeNoteMoveSnapMode AppPreferences::getWholeNoteMoveSnapMode() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_.shared.wholeNoteMoveSnapMode;
}

PluginEditorSize AppPreferences::getDefaultPluginEditorSize()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    jassert(pluginEditorSizeProperties_ != nullptr);
    if (pluginEditorSizeProperties_ == nullptr)
        return {};

    pluginEditorSizeProperties_->reload();
    return readDefaultPluginEditorSize(*pluginEditorSizeProperties_);
}

void AppPreferences::setDefaultPluginEditorSize(PluginEditorSize size)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    jassert(pluginEditorSizeProperties_ != nullptr);
    if (pluginEditorSizeProperties_ == nullptr)
        return;

    pluginEditorSizeProperties_->setValue(kPluginEditorSizeKey,
                           juce::String(size.width) + "," + juce::String(size.height));
}

} // namespace OpenTune
