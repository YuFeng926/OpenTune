#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace OpenTune {

enum class Language
{
    English = 0,
    Chinese,
    Japanese,
    Russian,
    Spanish,
    Count
};

inline juce::String getLanguageName(Language lang)
{
    switch (lang)
    {
        case Language::English:  return "English";
        case Language::Chinese:  return "中文";
        case Language::Japanese: return "日本語";
        case Language::Russian:  return "Русский";
        case Language::Spanish:  return "Español";
        default: return "English";
    }
}

inline juce::String getLanguageNativeName(Language lang)
{
    switch (lang)
    {
        case Language::English:  return juce::String::fromUTF8("English");
        case Language::Chinese:  return juce::String::fromUTF8("\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87");  // 简体中文
        case Language::Japanese: return juce::String::fromUTF8("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");  // 日本語
        case Language::Russian:  return juce::String::fromUTF8("\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9");  // Русский
        case Language::Spanish:  return juce::String::fromUTF8("Espa\xc3\xb1ol");  // Español
        default: return juce::String::fromUTF8("English");
    }
}

// LanguageChangeListener 接口 - 观察者模式
class LanguageChangeListener
{
public:
    virtual ~LanguageChangeListener() = default;
    virtual void languageChanged(Language newLanguage) = 0;
};

class LocalizationManager
{
public:
    struct LanguageState {
        Language language = Language::Chinese;
    };

    class ScopedLanguageBinding
    {
    public:
        explicit ScopedLanguageBinding(std::shared_ptr<LanguageState> state)
            : state_(std::move(state))
        {
            LocalizationManager::getInstance().bindLanguageState(state_);
        }

        ~ScopedLanguageBinding()
        {
            LocalizationManager::getInstance().unbindLanguageState(state_);
        }

        ScopedLanguageBinding(const ScopedLanguageBinding&) = delete;
        ScopedLanguageBinding& operator=(const ScopedLanguageBinding&) = delete;

    private:
        std::shared_ptr<LanguageState> state_;
    };

    static LocalizationManager& getInstance()
    {
        static LocalizationManager instance;
        return instance;
    }

    void bindLanguageState(const std::shared_ptr<LanguageState>& state)
    {
        if (state == nullptr) {
            return;
        }

        pruneExpiredBindings();
        const auto* rawState = state.get();
        languageBindings_.erase(std::remove_if(languageBindings_.begin(),
                                               languageBindings_.end(),
                                               [rawState](const std::weak_ptr<LanguageState>& binding) {
                                                   auto locked = binding.lock();
                                                   return locked != nullptr && locked.get() == rawState;
                                               }),
                                languageBindings_.end());
        languageBindings_.push_back(state);
    }

    void unbindLanguageState(const std::shared_ptr<LanguageState>& state)
    {
        if (state == nullptr) {
            return;
        }

        const auto* rawState = state.get();
        languageBindings_.erase(std::remove_if(languageBindings_.begin(),
                                               languageBindings_.end(),
                                               [rawState](const std::weak_ptr<LanguageState>& binding) {
                                                   auto locked = binding.lock();
                                                   return locked == nullptr || locked.get() == rawState;
                                               }),
                                languageBindings_.end());
    }

    Language resolveLanguage()
    {
        pruneExpiredBindings();

        for (auto it = languageBindings_.rbegin(); it != languageBindings_.rend(); ++it) {
            if (auto state = it->lock()) {
                return state->language;
            }
        }

        jassertfalse;
        return Language::Chinese;
    }

    void notifyLanguageChanged(Language lang)
    {
        listeners_.call(&LanguageChangeListener::languageChanged, lang);
    }

    void addListener(LanguageChangeListener* listener) { listeners_.add(listener); }
    void removeListener(LanguageChangeListener* listener) { listeners_.remove(listener); }

private:
    LocalizationManager() = default;

    void pruneExpiredBindings()
    {
        languageBindings_.erase(std::remove_if(languageBindings_.begin(),
                                               languageBindings_.end(),
                                               [](const std::weak_ptr<LanguageState>& binding) {
                                                   return binding.expired();
                                               }),
                                languageBindings_.end());
    }

    std::vector<std::weak_ptr<LanguageState>> languageBindings_;
    juce::ListenerList<LanguageChangeListener> listeners_;
};

namespace Loc {

template<typename... Args>
juce::String tr(const char* key, Args... args)
{
    return juce::String::fromUTF8(key);
}

namespace Keys {

constexpr const char* kFile = "File";
constexpr const char* kEdit = "Edit";
constexpr const char* kView = "View";

constexpr const char* kImportAudio = "Import Audio...";
constexpr const char* kExportAudio = "Export Audio";
constexpr const char* kExportSelectedClip = "Export Selected Clip";
constexpr const char* kExportTrack = "Export Track";
constexpr const char* kExportBus = "Export Bus (Master Mix)";
constexpr const char* kSaveProject = "Save Project";
constexpr const char* kSaveProjectAs = "Save Project As...";
constexpr const char* kOpenProject = "Open Project...";
constexpr const char* kRecentProjects = "Recent Projects";
constexpr const char* kClearRecentProjects = "Clear Recent Projects";
constexpr const char* kOptions = "Options";

constexpr const char* kUndo = "Undo";
constexpr const char* kRedo = "Redo";

constexpr const char* kShowWaveform = "Show Waveform";
constexpr const char* kPianoKeyboard = "Piano Keyboard";
constexpr const char* kScaleBrightness = "Scale Brightness";
constexpr const char* kNoteLabels = "Note Labels";
constexpr const char* kNoteLabelsShowAll = "Show All";
constexpr const char* kNoteLabelsCOnly = "C Only";
constexpr const char* kNoteLabelsHide = "Hide";
constexpr const char* kShowUnvoicedFrames = "Show Unvoiced Frames";
constexpr const char* kUiZoom = "UI Zoom";
constexpr const char* kBackgroundBrightness = "Background Brightness";
constexpr const char* kCorrectedF0Colour = "Corrected F0 Curve Color";
constexpr const char* kOriginalF0Colour = "Original F0 Curve Color";
constexpr const char* kTrackColors = "Track Colors";
constexpr const char* kTrackColorsRandom = "Random Colors";
constexpr const char* kTrackColorsCustom = "Custom Colors";
constexpr const char* kAddTrack = "Add Track";
constexpr const char* kDuplicateTrack = "Duplicate Track";
constexpr const char* kCustomColor = "Custom Color...";
constexpr const char* kRandomColor = "Random Color";
constexpr const char* kTrackColor = "Track Color";
constexpr const char* kDeleteTrack = "Delete Track";
constexpr const char* kTheme = "Theme";
constexpr const char* kThemeBlueBreeze = "Blue Breeze";
constexpr const char* kThemeDarkBlueGrey = "Dark Blue-Grey";
constexpr const char* kThemeAurora = "Aurora Glass";
constexpr const char* kThemeOverdose = "升天 / Overdose";
constexpr const char* kMouseTrail = "Mouse Trail";
constexpr const char* kOff = "Off";
constexpr const char* kClassic = "Classic";
constexpr const char* kNeon = "Neon";
constexpr const char* kFire = "Fire";
constexpr const char* kOcean = "Ocean";
constexpr const char* kGalaxy = "Galaxy";
constexpr const char* kCherryBlossom = "Cherry Blossom";
constexpr const char* kMatrix = "Matrix";

constexpr const char* kMouseCursorStyle = "Mouse Cursor Style";
constexpr const char* kCursorStyleSystem = "System";
constexpr const char* kCursorStyleAdwaita = "Adwaita";
constexpr const char* kCursorStyleCapitaine = "Capitaine";
constexpr const char* kCursorStyleBreeze = "Breeze";

constexpr const char* kAudio = "Audio";
constexpr const char* kAudioDeviceType = "Device Type";
constexpr const char* kAudioOutput = "Output";
constexpr const char* kAudioSampleRate = "Sample Rate";
constexpr const char* kAudioBufferSize = "Buffer Size";
constexpr const char* kAudioBufferFormat = "{0} samples ({1} ms)";
constexpr const char* kAudioTest = "Test";
constexpr const char* kAudioNoDevice = "No Device";
constexpr const char* kAudioActiveOutputChannels = "Active Output Channels";
constexpr const char* kAudioNoOutputChannels = "No output channels available";
constexpr const char* kAudioChannel = "Channel {0}";
constexpr const char* kAudioDeviceError = "Audio device error";
constexpr const char* kAudioDeviceSwitchFailed = "Failed to switch audio device";
constexpr const char* kEditing = "Editing";
constexpr const char* kMouse = "Mouse";
constexpr const char* kShortcuts = "Shortcuts";
constexpr const char* kShortcutGeneral = "General";
constexpr const char* kShortcutOpenTuneMode = "OpenTune Mode";
constexpr const char* kShortcutOpenDyneMode = "OpenDyne Mode";
constexpr const char* kLanguage = "Language";
constexpr const char* kLanguageLabel = "Interface Language";
constexpr const char* kAudioEditingScheme = "Audio Editing Scheme";
constexpr const char* kSchemeOpenTune = "OpenTune";
constexpr const char* kSchemeOpenDyne = "OpenDyne";
constexpr const char* kGridStyle = "Grid Style";
constexpr const char* kGridStylePianoLanes = "Piano Lanes";
constexpr const char* kGridStyleEqualSpacing = "Equal Spacing";
constexpr const char* kWholeNoteMoveSnap = "Move Snap";
constexpr const char* kWholeNoteMoveSnapScale = "Follow Global Scale";
constexpr const char* kWholeNoteMoveSnapStandardPitch = "Snap to Semitones";

constexpr const char* kHorizontalZoomSensitivity = "Horizontal Zoom Sensitivity";
constexpr const char* kVerticalZoomSensitivity = "Vertical Zoom Sensitivity";
constexpr const char* kScrollSpeed = "Scroll Speed";
constexpr const char* kTuningHz = "Tuning Hz";
constexpr const char* kResetToDefaults = "Reset to Defaults";
constexpr const char* kRenderingPriority = "Rendering Priority";
constexpr const char* kRendering = "Rendering";
constexpr const char* kGpuFirst = "GPU First";
constexpr const char* kCpuFirst = "CPU First";
constexpr const char* kHybridMode = "Hybrid Mode";

constexpr const char* kVocoderWeight = "Vocoder Model";

constexpr const char* kSetShortcut = "Set Shortcut";
constexpr const char* kPressNewKeyCombination = "Press the new key combination";
constexpr const char* kCurrent = "Current";
constexpr const char* kCancel = "Cancel";
constexpr const char* kOK = "OK";
constexpr const char* kShortcutConflict = "Shortcut Conflict";
constexpr const char* kShortcutConflictMessage = "This shortcut is already assigned to \"{0}\".\n\nDo you want to reassign it?";
constexpr const char* kYes = "Yes";
constexpr const char* kNo = "No";
constexpr const char* kResetAllToDefaults = "Reset All to Defaults";

// Shortcut capture dialog
constexpr const char* kShortcutCaptureClickHint = "Click here, then press a key";
constexpr const char* kShortcutCapturePressKey = "Press a key...";
constexpr const char* kShortcutCaptureCancelHint = "Click Cancel to abort";
constexpr const char* kDiagnosticLogTail = "(Only the last 256 KiB are retained)";

constexpr const char* kPlayPause = "Play/Pause";
constexpr const char* kStop = "Stop";
constexpr const char* kPlayFromStart = "Play from Start";
constexpr const char* kCut = "Cut";
constexpr const char* kCopy = "Copy";
constexpr const char* kPaste = "Paste";
constexpr const char* kSelectAll = "Select All";
constexpr const char* kDelete = "Delete";
constexpr const char* kSplitClip = "Split Clip";
constexpr const char* kMergeClips = "Merge Clips";
constexpr const char* kDuplicateClip = "Duplicate Clip";
constexpr const char* kNudgeLeft = "Nudge Left";
constexpr const char* kNudgeRight = "Nudge Right";
constexpr const char* kToggleSnap = "Toggle Snap";

constexpr const char* kToolDrawNote = "Tool: Draw Note";
constexpr const char* kToolSelect = "Tool: Select";
constexpr const char* kToolLineAnchor = "Tool: Line Anchor";
constexpr const char* kToolHandDraw = "Tool: Hand Draw";
constexpr const char* kToolAutoTune = "Tool: AutoTune";
constexpr const char* kToolTimeTool = "Tool: Time";
constexpr const char* kToolPitch = "Tool: Pitch";
constexpr const char* kToolModulation = "Tool: Modulation";
constexpr const char* kToolDrift = "Tool: Drift";
constexpr const char* kToolVolumeEnvelope = "Tool: Volume Envelope";
constexpr const char* kToolScissors = "Tool: Scissors";
constexpr const char* kCancelSelection = "Cancel Selection";
constexpr const char* kToolODSelect = "Select";
constexpr const char* kToolODPitch = "Pitch";
constexpr const char* kToolODPitchModulation = "Pitch Modulation";
constexpr const char* kToolODPitchDrift = "Pitch Drift";
constexpr const char* kToolODVolumeEnvelope = "Volume Envelope";
constexpr const char* kToolODScissors = "Scissors";
constexpr const char* kToolEq = "Tool: EQ";

constexpr const char* kPitchCorrection = "Pitch correction";
constexpr const char* kRetuneSpeed = "Retune Speed";
constexpr const char* kVibratoDepth = "Vib. Depth";
constexpr const char* kVibratoRate = "Vib. Rate";
constexpr const char* kNoteSplit = "Note Split";
constexpr const char* kTools = "Tools";
constexpr const char* kAuto = "Auto";
constexpr const char* kSelect = "Select";
constexpr const char* kDrawNotes = "Draw notes";
constexpr const char* kLineAnchor = "Line anchor";
constexpr const char* kHandDraw = "Hand draw pitch";

constexpr const char* kPlay = "Play";
constexpr const char* kPause = "Pause";
constexpr const char* kLoop = "Loop";
constexpr const char* kTapTempo = "Tap Tempo";
constexpr const char* kRecord = "Record";
constexpr const char* kTrackView = "Track View";
constexpr const char* kPianoRollView = "Piano Roll View";

constexpr const char* kTracks = "Tracks";
constexpr const char* kProps = "Props";
constexpr const char* kScale = "Scale";
constexpr const char* kRootNote = "Root";
constexpr const char* kScrollContinuous = "Cont";
constexpr const char* kScrollPage = "Page";
constexpr const char* kTime = "Time";
constexpr const char* kNoSnap = "No Snap";
constexpr const char* kChromatic = "Chromatic";
constexpr const char* kKeyScale = "Key Scale";
constexpr const char* kPitchShift = "Pitch Shift";
constexpr const char* kPreferences = "Preferences";
constexpr const char* kHostControlledTransport = "Host-controlled transport";
constexpr const char* kUntitled = "Untitled";
constexpr const char* kTrackPrefix = "Track ";
constexpr const char* kClip = "Clip";
constexpr const char* kImportAudioDialog = "Import Audio";
constexpr const char* kImportFailed = "Import Failed";
constexpr const char* kExportFailed = "Export Failed";
constexpr const char* kOpenProjectDialog = "Open Project";
constexpr const char* kAutoDialog = "AUTO";
constexpr const char* kAudioImportInProgress = "Audio import is already in progress. Please try again later.";
constexpr const char* kUnsupportedFileType = "Unsupported file type.\nSupported extensions: {0}";
constexpr const char* kMultipleFilesDetected = "Multiple files detected. Only the first file will be imported.";
constexpr const char* kNoAvailableTracksAfterCurrent = "There are no available tracks after the current track.";
constexpr const char* kImportCountTrimmed = "Only {0} tracks are available after the current track. Extra files were not queued.";
constexpr const char* kAudioImportPreprocessingFailed = "Audio import preprocessing failed. Please try again.";
constexpr const char* kAudioImportCommitFailed = "Audio import commit failed. Please try again.";
constexpr const char* kImportCountTrimmedTitle = "Import Count Trimmed";
constexpr const char* kSelectAudioFilesToImport = "Select audio files to import";
constexpr const char* kChooseImportMode = "Choose Import Mode";
constexpr const char* kSelectedAudioFilesImportMode = "You selected {0} audio files. Choose an import mode.";
constexpr const char* kImportSequentiallyToCurrentTrack = "Import Sequentially To Current Track";
constexpr const char* kImportToSeparateTracks = "Import To Separate Tracks";
constexpr const char* kMaximumTrackCountReached = "Maximum track count reached ({0}). Cannot create more tracks.";
constexpr const char* kExportAudioFile = "Export Audio File";
constexpr const char* kOverwriteExistingFile = "Overwrite Existing File?";
constexpr const char* kOverwriteExistingFileMessage = "The target file already exists. Overwrite it?";
constexpr const char* kOverwriteExistingProject = "Overwrite Existing Project?";
constexpr const char* kOverwriteExistingProjectMessage = "The target project file already exists. Overwrite it?";
constexpr const char* kExportInProgress = "An export task is already in progress. Please try again later.";
constexpr const char* kNoAudioClipSelected = "No audio clip is selected. Select a clip on the track first.";
constexpr const char* kSelectedPlacement = "Selected Placement (Track {0}, Clip {1})";
constexpr const char* kTrackTarget = "Track {0}";
constexpr const char* kBusMasterMix = "Bus (Master Mix)";
constexpr const char* kHelpFileNotFound = "Help file not found: {0}";
constexpr const char* kReferenceMenuEntry = "Track {0} - {1} (Mat#{2})";
constexpr const char* kAutoRef = "AUTO Ref";
constexpr const char* kAutoRefAlignmentFailed = "AUTO Ref alignment failed.";
constexpr const char* kHostManagedActionPrefix = "In VST3 mode this action is managed by your DAW.\n\n";
constexpr const char* kVst3ImportAudio = "Please import audio from your DAW in VST3 mode.";
constexpr const char* kVst3ExportAudio = "Please render/export from your DAW in VST3 mode.";
constexpr const char* kProjectFileManagementStandalone = "Project file management is handled in the Standalone version.";
constexpr const char* kOpenRecentProject = "Open Recent Project";
constexpr const char* kHelpDialog = "Help";
constexpr const char* kVst3Help = "Open the host DAW plugin help/manual entry for VST3 usage guidance.";
constexpr const char* kVst3ReadAudioNotReady = "This VST3 instance is not ready for audio capture or ARA reading.";
constexpr const char* kVst3ReadAudioSelectionNotReady = "The selected item is not ready. Please re-select and try again.";
constexpr const char* kVst3ReadAudioRegionsFailed = "Audio regions could not be processed.";
constexpr const char* kAutoNeedsActiveAra = "AUTO needs an active ARA audio modification.";
constexpr const char* kOriginalF0 = "OriginalF0";
constexpr const char* kOriginalF0NotReady = "OriginalF0 is not ready.";
constexpr const char* kOriginalF0Extracting = "OriginalF0 is being extracted. Please retry in a moment.";
constexpr const char* kOriginalF0ExtractionFailed = "OriginalF0 extraction failed for this clip. Re-import the audio to regenerate OriginalF0.";
constexpr const char* kOriginalF0NotReadyForClip = "OriginalF0 is not ready for this clip.";
constexpr const char* kAutoQueued = "AUTO has been queued.";
constexpr const char* kAutoNeedsPitchCurve = "AUTO needs an active pitch curve. Run audio analysis first.";
constexpr const char* kAutoNoContentCommands = "AUTO cannot run because content edit commands are not attached.";
constexpr const char* kAutoNeedsEditableClip = "AUTO needs an active editable clip.";
constexpr const char* kAutoCannotReadContentSnapshot = "AUTO cannot read the editable content snapshot.";
constexpr const char* kAutoOriginalF0NotReady = "AUTO needs OriginalF0 to be ready for this clip.";
constexpr const char* kAutoCannotReadCurveSnapshot = "AUTO cannot read the current pitch-curve snapshot.";
constexpr const char* kAutoNeedsOriginalF0 = "AUTO needs non-empty OriginalF0 data.";
constexpr const char* kAutoCannotMapF0Timeline = "AUTO cannot map this clip to an F0 timeline.";
constexpr const char* kAutoNeedsSelection = "AUTO needs a selected note, F0 range, or selection area.";
constexpr const char* kAutoTargetRangeEmpty = "AUTO target range is empty.";
constexpr const char* kAutoCouldNotApply = "AUTO could not be applied.";
constexpr const char* kRenderFailure = "Render Failure";
constexpr const char* kRenderFailed = "Render failed";
constexpr const char* kRenderFailureDetail = "Render failed; dry audio may be used. Reason: {0}";
constexpr const char* kProjectRootNodeInvalid = "Root node is not {0}";
constexpr const char* kUnsupportedProjectFormat = "Unsupported project format version: {0} (supported {1}-{2})";
constexpr const char* kProjectInvalidDynamicEq = "Project contains invalid dynamic EqSettings";
constexpr const char* kProjectMissingSettings = "Project is missing required ProjectSettings node";
constexpr const char* kProjectSettingsMissingTimeSignature = "ProjectSettings is missing required timeSignatureNumerator or timeSignatureDenominator";
constexpr const char* kProjectFileNotFound = "Project file not found: {0}";
constexpr const char* kProjectParseFailed = "Failed to parse project file: {0} — {1}";
constexpr const char* kProjectInvalidXml = "Invalid XML structure in: {0}";
constexpr const char* kProjectCoreStoresUnavailable = "Cannot commit project: core stores unavailable";
constexpr const char* kProjectWriteFailed = "Failed to write project file: {0}";
constexpr const char* kProjectNoMediaDirectory = "Cannot copy media: no media directory";
constexpr const char* kProjectMediaDirectoryCreateFailed = "Failed to create media directory: {0}";
constexpr const char* kProjectSourceFileNotFoundForCopy = "Source file not found for copy: {0}";
constexpr const char* kProjectMediaCopyFailed = "Failed to copy media file: {0} -> {1}";
constexpr const char* kPitchEditing = "Pitch editing";
constexpr const char* kSplitNotes = "Split notes";
constexpr const char* kEraser = "Eraser";

        constexpr const char* kClose = "Close";
        constexpr const char* kHelp = "Help...";
        constexpr const char* kOnboarding = "Onboarding...";
        constexpr const char* kReplayOnboarding = "Replay onboarding";
        constexpr const char* kUserGuide = "User Guide...";

constexpr const char* kMouseSelectTool = "Mouse Select Tool";
constexpr const char* kDrawNoteTool = "Draw Note Tool";
constexpr const char* kLineAnchorTool = "Line Anchor Tool";
constexpr const char* kHandDrawTool = "Hand Draw Tool";

// Tooltip descriptions (with shortcuts where applicable)
constexpr const char* kTooltipPlay = "Play";
constexpr const char* kTooltipPause = "Pause";
constexpr const char* kTooltipStop = "Stop";
constexpr const char* kTooltipLoop = "Loop";
constexpr const char* kTooltipRecord = "Read Audio";
constexpr const char* kTooltipTrackView = "Track View";
constexpr const char* kTooltipPianoRollView = "Piano Roll View";
constexpr const char* kTooltipTapTempo = "Tap Tempo";
constexpr const char* kTooltipFile = "File Menu";
constexpr const char* kTooltipEdit = "Edit Menu";
constexpr const char* kTooltipView = "View Menu";
constexpr const char* kTooltipAutoTune = "Auto Correction";
constexpr const char* kTooltipSelect = "Selection Tool";
constexpr const char* kTooltipDrawNote = "Draw Note";
constexpr const char* kTooltipLineAnchor = "Line Anchor";
constexpr const char* kTooltipHandDraw = "Hand Draw Pitch";
constexpr const char* kTooltipEraser = "Eraser - Remove notes and pitch";
constexpr const char* kTooltipTimeTool = "Time Tool";
constexpr const char* kTooltipTrackPanel = "Track Panel";
constexpr const char* kTooltipParameterPanel = "Parameter Panel";
constexpr const char* kTooltipBpm = "Tempo (BPM)";
constexpr const char* kTooltipTimeline = "Playback Time";
constexpr const char* kTooltipTimeUnit = "Toggle Time/Bars Display";
constexpr const char* kTooltipScrollMode = "Scroll Mode";
constexpr const char* kTooltipRetuneSpeed = "Retune Speed - Controls how fast pitch is corrected";
constexpr const char* kTooltipVibratoDepth = "Vibrato Depth - Controls vibrato amplitude";
constexpr const char* kTooltipVibratoRate = "Vibrato Rate - Controls vibrato speed";
constexpr const char* kTooltipNoteSplit = "Note Split - Threshold for splitting notes";
constexpr const char* kTooltipPitchModulation = "Modulation - Vibrato Depth";
constexpr const char* kTooltipPitchDrift = "Drift - Pitch Drift Correction";

constexpr const char* kTooltipEqMaximize = "Expand EQ Editor";
constexpr const char* kTooltipEqMinimize = "Collapse to Preview";
constexpr const char* kTooltipEqBypass = "Toggle EQ Bypass";
constexpr const char* kTooltipEqRemove = "Remove EQ from Note";
constexpr const char* kTooltipEqClose = "Close EQ Editor";

}

inline juce::String get(Language lang, const char* key)
{
    static const struct Entry {
        const char* key;
        const char* en;
        const char* zh;
        const char* ja;
        const char* ru;
        const char* es;
    } translations[] = {
        { Keys::kFile, "File", "文件", "ファイル", "Файл", "Archivo" },
        { Keys::kEdit, "Edit", "编辑", "編集", "Правка", "Editar" },
        { Keys::kView, "View", "视图", "表示", "Вид", "Ver" },
        
        { Keys::kImportAudio, "Import Audio...", "导入音频...", "オーディオをインポート...", "Импорт аудио...", "Importar audio..." },
        { Keys::kExportAudio, "Export Audio", "导出音频", "オーディオをエクスポート", "Экспорт аудио", "Exportar audio" },
        { Keys::kExportSelectedClip, "Export Selected Clip", "导出选中的片段", "選択したクリップをエクスポート", "Экспорт клипа", "Exportar clip seleccionado" },
        { Keys::kExportTrack, "Export Track", "导出轨道", "トラックをエクスポート", "Экспорт дорожки", "Exportar pista" },
        { Keys::kExportBus, "Export Bus (Master Mix)", "导出总线混音", "バス（マスターミックス）をエクスポート", "Экспорт шины", "Exportar bus (mezcla maestra)" },
        { Keys::kSaveProject, "Save Project", "保存工程", "プロジェクトを保存", "Сохранить проект", "Guardar proyecto" },
        { Keys::kSaveProjectAs, "Save Project As...", "另存为工程...", "プロジェクトを別名で保存...", "Сохранить как...", "Guardar proyecto como..." },
        { Keys::kOpenProject, "Open Project...", "打开工程...", "プロジェクトを開く...", "Открыть проект...", "Abrir proyecto..." },
        { Keys::kRecentProjects, "Recent Projects", "最近工程", "最近のプロジェクト", "Недавние проекты", "Proyectos recientes" },
        { Keys::kClearRecentProjects, "Clear Recent Projects", "清除最近工程", "最近のプロジェクトをクリア", "Очистить список", "Limpiar proyectos recientes" },
        { Keys::kOptions, "Options", "选项", "オプション", "Настройки", "Opciones" },
        
        { Keys::kUndo, "Undo", "撤销", "元に戻す", "Отменить", "Deshacer" },
        { Keys::kRedo, "Redo", "重做", "やり直す", "Повтор", "Rehacer" },
        
        { Keys::kShowWaveform, "Show Waveform", "显示波形", "波形を表示", "Волновая форма", "Ver forma de onda" },
        { Keys::kPianoKeyboard, "Piano Keyboard", "钢琴键盘", "ピアノキーボード", "Клавиатура пианино", "Teclado de piano" },
        { Keys::kScaleBrightness, "Scale Brightness", "音阶明暗", "明るさ", "Яркость лада", "Brillo de escala" },
        { Keys::kNoteLabels, "Note Labels", "音名标签", "音名ラベル", "Названия нот", "Etiquetas de notas" },
        { Keys::kNoteLabelsShowAll, "Show All", "全部显示", "全表示", "Показывать все", "Mostrar todo" },
        { Keys::kNoteLabelsCOnly, "C Only", "仅 C", "C のみ", "Только C", "Solo C" },
        { Keys::kNoteLabelsHide, "Hide", "隐藏", "非表示", "Скрыть", "Ocultar" },
        { Keys::kShowUnvoicedFrames, "Show Unvoiced Frames", "显示无声音帧", "無声音フレームを表示", "Показывать глухие кадры", "Mostrar cuadros sordos" },
        { Keys::kUiZoom, "UI Zoom", "界面缩放", "UIズーム", "Масштаб интерфейса", "Zoom de interfaz" },
        { Keys::kBackgroundBrightness, "Background Brightness", "背景亮度", "背景の明るさ", "Яркость фона", "Brillo de fondo" },
        { Keys::kCorrectedF0Colour, "Pitch Correction Curve Color", "音高修正曲线颜色", "ピッチ補正曲線の色", "Цвет кривой коррекции высоты тона", "Color de la curva de corrección de tono" },
        { Keys::kOriginalF0Colour, "Original Pitch Curve Color", "原始音高曲线颜色", "元のピッチ曲線の色", "Цвет исходной кривой высоты тона", "Color de la curva de tono original" },
        { Keys::kTrackColors, "Track Colors", "轨道颜色", "トラック色", "Цвет дорожки", "Color pista" },
        { Keys::kTrackColorsRandom, "Random Colors", "随机颜色", "ランダム色", "Случайный цвет", "Color aleatorio" },
        { Keys::kTrackColorsCustom, "Custom Colors", "自定义颜色", "カスタム色", "Пользовательский", "Color personalizado" },
        { Keys::kAddTrack, "Add Track", "新建轨道", "トラックを追加", "Добавить дорожку", "Añadir pista" },
        { Keys::kDuplicateTrack, "Duplicate Track", "复制轨道", "トラックを複製", "Дублировать дорожку", "Duplicar pista" },
        { Keys::kCustomColor, "Custom Color...", "自定义颜色...", "カスタム色...", "Свой цвет...", "Color personalizado..." },
        { Keys::kRandomColor, "Random Color", "随机颜色", "ランダム色", "Случайный цвет", "Color aleatorio" },
        { Keys::kTrackColor, "Track Color", "轨道颜色", "トラック色", "Цвет дорожки", "Color pista" },
        { Keys::kDeleteTrack, "Delete Track", "删除轨道", "トラックを削除", "Удалить дорожку", "Eliminar pista" },
        { Keys::kTheme, "Theme", "主题", "テーマ", "Тема", "Tema" },
        { Keys::kThemeBlueBreeze, "Blue Breeze", "蓝色清风", "ブルーブリーズ", "Голубой бриз", "Brisa azul" },
        { Keys::kThemeDarkBlueGrey, "Dark Blue-Grey", "深蓝灰", "ダークブルーグレー", "Тёмно-синий серый", "Azul-gris oscuro" },
        { Keys::kThemeAurora, "Aurora Glass", "极光玻璃", "オーロラグラス", "Аврора", "Aurora cristal" },
        { Keys::kThemeOverdose, "Overdose", "升天", "オーバードーズ", "Передозировка", "Sobredosis" },
        { Keys::kMouseTrail, "Mouse Trail", "鼠标轨迹", "マウストレイル", "След мыши", "Ratón" },
        { Keys::kOff, "Off", "关闭", "オフ", "Выкл", "Apagado" },
        { Keys::kClassic, "Classic", "经典", "クラシック", "Классика", "Clásico" },
        { Keys::kNeon, "Neon", "霓虹", "ネオン", "Неон", "Neón" },
        { Keys::kFire, "Fire", "火焰", "ファイア", "Огонь", "Fuego" },
        { Keys::kOcean, "Ocean", "海洋", "オーシャン", "Океан", "Océano" },
        { Keys::kGalaxy, "Galaxy", "星河", "ギャラクシー", "Галактика", "Galaxia" },
        { Keys::kCherryBlossom, "Cherry Blossom", "樱花", "桜", "Сакура", "Flor de cerezo" },
        { Keys::kMatrix, "Matrix", "矩阵", "マトリックス", "Матрица", "Matriz" },

        { Keys::kMouseCursorStyle, "Mouse Cursor Style", "鼠标指针样式", "マウスカーソルスタイル", "Стиль указателя мыши", "Estilo del cursor" },
        { Keys::kCursorStyleSystem, "System", "系统", "システム", "Система", "Sistema" },
        { Keys::kCursorStyleAdwaita, "Adwaita", "Adwaita", "Adwaita", "Adwaita", "Adwaita" },
        { Keys::kCursorStyleCapitaine, "Capitaine", "Capitaine", "Capitaine", "Capitaine", "Capitaine" },
        { Keys::kCursorStyleBreeze, "Breeze", "Breeze", "Breeze", "Breeze", "Breeze" },
        
        { Keys::kAudio, "Audio", "音频", "オーディオ", "Аудио", "Audio" },
        { Keys::kAudioDeviceType, "Device Type", "设备类型", "デバイスタイプ", "Тип устройства", "Tipo de dispositivo" },
        { Keys::kAudioOutput, "Output", "输出", "出力", "Выход", "Salida" },
        { Keys::kAudioSampleRate, "Sample Rate", "采样率", "サンプルレート", "Частота дискретизации", "Frecuencia de muestreo" },
        { Keys::kAudioBufferSize, "Buffer Size", "缓冲区大小", "バッファサイズ", "Размер буфера", "Tamaño del búfer" },
        { Keys::kAudioBufferFormat, "{0} samples ({1} ms)", "{0} 采样（{1} 毫秒）", "{0} サンプル（{1} ミリ秒）", "{0} сэмплов ({1} мс)", "{0} muestras ({1} ms)" },
        { Keys::kAudioTest, "Test", "测试", "テスト", "Тест", "Probar" },
        { Keys::kAudioNoDevice, "No Device", "无设备", "デバイスなし", "Нет устройства", "Sin dispositivo" },
        { Keys::kAudioActiveOutputChannels, "Active Output Channels", "活动输出通道", "アクティブ出力チャンネル", "Активные выходные каналы", "Canales de salida activos" },
        { Keys::kAudioNoOutputChannels, "No output channels available", "无可用输出通道", "利用可能な出力チャンネルがありません", "Нет доступных выходных каналов", "No hay canales de salida disponibles" },
        { Keys::kAudioChannel, "Channel {0}", "通道 {0}", "チャンネル {0}", "Канал {0}", "Canal {0}" },
        { Keys::kAudioDeviceError, "Audio device error", "音频设备错误", "オーディオデバイスエラー", "Ошибка аудиоустройства", "Error del dispositivo de audio" },
        { Keys::kAudioDeviceSwitchFailed, "Failed to switch audio device", "音频设备切换失败", "オーディオデバイスの切り替えに失敗しました", "Не удалось переключить аудиоустройство", "Error al cambiar el dispositivo de audio" },
        { Keys::kEditing, "Editing", "编辑", "編集", "Редактирование", "Edicion" },
        { Keys::kMouse, "Mouse", "鼠标", "マウス", "Мышь", "Ratón" },
        { Keys::kShortcuts, "Shortcuts", "快捷键", "ショートカット", "Сочетания клавиш", "Atajos de teclado" },
        { Keys::kShortcutGeneral, "General", "通用", "一般", "Общие", "General" },
        { Keys::kShortcutOpenTuneMode, "OpenTune Mode", "OpenTune 模式", "OpenTune モード", "Режим OpenTune", "Modo OpenTune" },
        { Keys::kShortcutOpenDyneMode, "OpenDyne Mode", "OpenDyne 模式", "OpenDyne モード", "Режим OpenDyne", "Modo OpenDyne" },
        { Keys::kLanguage, "Language", "语言", "言語", "Язык", "Idioma" },
        { Keys::kLanguageLabel, "Interface Language", "界面语言", "インターフェース言語", "Язык", "Idioma" },
        { Keys::kAudioEditingScheme, "Audio Editing Scheme", "音频编辑方案", "音声編集方式", "Схема аудиоредактирования", "Esquema de edicion de audio" },
        { Keys::kSchemeOpenTune, "OpenTune", "OpenTune", "OpenTune", "OpenTune", "OpenTune" },
        { Keys::kSchemeOpenDyne, "OpenDyne", "OpenDyne", "OpenDyne", "OpenDyne", "OpenDyne" },
        { Keys::kGridStyle, "Grid Style", "网格样式", "グリッドスタイル", "Стиль сетки", "Estilo de cuadricula" },
        { Keys::kGridStylePianoLanes, "Piano Lanes", "钢琴键槽", "ピアノレーン", "Клавиши пианино", "Teclas de piano" },
        { Keys::kGridStyleEqualSpacing, "Equal Spacing", "等距", "等間隔", "Равный интервал", "Espaciado igual" },
        { Keys::kWholeNoteMoveSnap, "Move Snap", "移动吸附", "移動スナップ", "Привязка при сдвиге", "Ajuste al mover" },
        { Keys::kWholeNoteMoveSnapScale, "Follow Global Scale", "跟随全局调式", "グローバルスケールに従う", "Следовать глобальной гамме", "Seguir escala global" },
        { Keys::kWholeNoteMoveSnapStandardPitch, "Snap to Semitones", "半音吸附", "半音スナップ", "По полутонам", "Por semitonos" },
        
        { Keys::kHorizontalZoomSensitivity, "Horizontal Zoom Sensitivity", "水平缩放灵敏度", "水平ズーム感度", "Чувств. гориз. zoom", "Sensibilidad zoom horizontal" },
        { Keys::kVerticalZoomSensitivity, "Vertical Zoom Sensitivity", "垂直缩放灵敏度", "垂直ズーム感度", "Чувств. верт. zoom", "Sensibilidad zoom vertical" },
        { Keys::kScrollSpeed, "Scroll Speed", "滚动速度", "スクロール速度", "Скорость прокрутки", "Velocidad" },
        { Keys::kTuningHz, "Tuning Hz", "基准音高", "基準ピッチ", "Частота настройки", "Frecuencia de afinación" },
        { Keys::kResetToDefaults, "Reset to Defaults", "恢复默认设置", "デフォルトに戻す", "Сбросить", "Restablecer" },
        { Keys::kRenderingPriority, "Rendering Priority", "渲染优先级", "レンダリング優先度", "Приоритет рендеринга", "Prioridad de renderizado" },
        { Keys::kRendering, "Rendering", "渲染中", "レンダリング中", "Рендеринг", "Renderizando" },
        { Keys::kGpuFirst, "GPU First", "GPU 优先", "GPU 優先", "GPU приоритет", "GPU primero" },
        { Keys::kCpuFirst, "CPU First", "CPU 优先", "CPU 優先", "CPU приоритет", "CPU primero" },
        { Keys::kHybridMode, "Hybrid Mode: Small corrections use DSP, large corrections use vocoder", "混合模式:小修用dsp，大修用声码器", "ハイブリッドモード：小さな修正はDSP、大きな修正はボコーダー", "Гибридный режим: небольшие коррекции через DSP, большие через вокодер", "Modo híbrido: correcciones pequeñas con DSP, grandes con vocoder" },
        { Keys::kVocoderWeight, "Vocoder Model", "声码器模型", "ボコーダーモデル", "Модель вокодера", "Modelo de vocoder" },

        { Keys::kSetShortcut, "Set Shortcut", "设置快捷键", "ショートカットを設定", "Назначить сочетание", "Atajo" },
        { Keys::kPressNewKeyCombination, "Press the new key combination", "按下新的组合键", "新しいキーの組み合わせを押してください", "Нажмите сочетание", "Pulse combinación" },
        { Keys::kCurrent, "Current", "当前", "現在", "Текущий", "Actual" },
        { Keys::kCancel, "Cancel", "取消", "キャンセル", "Отмена", "Cancelar" },
        { Keys::kOK, "OK", "确定", "OK", "ОК", "Aceptar" },
        { Keys::kShortcutConflict, "Shortcut Conflict", "快捷键冲突", "ショートカットの競合", "Конфликт сочетаний", "Conflicto de atajo" },
        { Keys::kShortcutConflictMessage, "This shortcut is already assigned to \"{0}\".\n\nDo you want to reassign it?", "此快捷键已分配给\"{0}\"。\n\n是否重新分配？", "このショートカットは既に「{0}」に割り当てられています。\n\n再割り当てしますか？", "Это сочетание уже назначено для \"{0}\".\n\nПереназначить?", "Este atajo ya está asignado a \"{0}\".\n\n¿Reasignar?" },
        { Keys::kYes, "Yes", "是", "はい", "Да", "Sí" },
        { Keys::kNo, "No", "否", "いいえ", "Нет", "No" },
        { Keys::kResetAllToDefaults, "Reset All to Defaults", "全部恢复默认", "すべてデフォルトに戻す", "Сбросить все", "Restablecer todo" },
        
        { Keys::kShortcutCaptureClickHint, "Click here, then press a key", "点击此处后按下快捷键", "ここをクリックしてキーを押してください", "Нажмите здесь и нажмите клавишу", "Haga clic aquí y pulse una tecla" },
        { Keys::kShortcutCapturePressKey, "Press a key...", "请按下快捷键...", "キーを押してください...", "Нажмите клавишу...", "Pulse una tecla..." },
        { Keys::kShortcutCaptureCancelHint, "Click Cancel to abort", "点击取消按钮取消", "キャンセルをクリックして中止", "Нажмите Отмена для отмены", "Pulse Cancelar para abortar" },
        { Keys::kDiagnosticLogTail, "(Only the last 256 KiB are retained)", "(仅保留最后 256 KiB)", "(最後の 256 KiB のみ保持)", "(Сохранены только последние 256 КиБ)", "(Solo se conservan los últimos 256 KiB)" },

        { Keys::kPlayPause, "Play/Pause", "播放/暂停", "再生/一時停止", "Старт/Пауза", "Play/Pausa" },
        { Keys::kStop, "Stop", "停止", "停止", "Стоп", "Detener" },
        { Keys::kPlayFromStart, "Play from Start", "从头播放", "最初から再生", "Играть сначала", "Reprod. inicio" },
        { Keys::kCut, "Cut", "剪切", "切り取り", "Вырезать", "Cortar" },
        { Keys::kCopy, "Copy", "复制", "コピー", "Копия", "Copiar" },
        { Keys::kPaste, "Paste", "粘贴", "貼り付け", "Вставить", "Pegar" },
        { Keys::kSelectAll, "Select All", "全选", "すべて選択", "Выбрать всё", "Selec. todo" },
        { Keys::kDelete, "Delete", "删除", "削除", "Удалить", "Eliminar" },
        { Keys::kSplitClip, "Split Clip", "拆分片段", "クリップを分割", "Разрезать клип", "Dividir clip" },
        { Keys::kMergeClips, "Merge Clips", "合并片段", "クリップを結合", "Объединить клипы", "Unir clips" },
        { Keys::kDuplicateClip, "Duplicate Clip", "原地复制", "クリップを複製", "Дублировать клип", "Duplicar clip" },
        { Keys::kNudgeLeft, "Nudge Left", "左移", "左に微調整", "Сдвинуть влево", "Desplazar izq." },
        { Keys::kNudgeRight, "Nudge Right", "右移", "右に微調整", "Сдвинуть вправо", "Desplazar der." },
        { Keys::kToggleSnap, "Toggle Snap", "切换吸附", "スナップ切替", "Перекл. привязку", "Activar ajuste" },
        { Keys::kToolDrawNote, "Tool: Draw Note", "工具：绘制音符", "ツール：ノート描画", "Инструмент: рисование нот", "Herram: dibujar nota" },
        { Keys::kToolSelect, "Tool: Select", "工具：选择", "ツール：選択", "Инструмент: выбор", "Herram: seleccionar" },
        { Keys::kToolLineAnchor, "Tool: Line Anchor", "工具：锚点", "ツール：ラインアンカー", "Инструмент: якорь", "Herram: ancla línea" },
        { Keys::kToolHandDraw, "Tool: Hand Draw", "工具：手绘", "ツール：手描き", "Инструмент: рисование", "Herram: mano alzada" },
        { Keys::kToolAutoTune, "Tool: AutoTune", "工具：自动校正", "ツール：オートチューン", "Инструмент: автотюн", "Herram: autoajuste" },
        { Keys::kToolTimeTool, "Tool: Time", "工具：时间", "ツール：タイム", "Инструмент: время", "Herram: tiempo" },
        { Keys::kCancelSelection, "Cancel Selection", "取消选择", "選択解除", "Отменить выбор", "Cancelar selección" },
        { Keys::kToolODSelect, "Select", "选择", "選択", "выбор", "seleccionar" },
        { Keys::kToolODPitch, "Pitch", "音高", "ピッチ", "тон", "tono" },
        { Keys::kToolODPitchModulation, "Pitch Modulation", "音高调制", "ピッチ変調", "модуляция тона", "modulación tono" },
        { Keys::kToolODPitchDrift, "Pitch Drift", "音高漂移", "ピッチドリフト", "дрифт тона", "deriva tono" },
        { Keys::kToolODVolumeEnvelope, "Volume Envelope", "音量包络", "ボリュームエンベロープ", "огибающая громкости", "envol. volumen" },
        { Keys::kToolODScissors, "Scissors", "剪刀", "ハサミ", "ножницы", "tijeras" },
        { Keys::kToolEq, "Tool: EQ", "工具：均衡器", "ツール：イコライザー", "Инструмент: эквалайзер", "Herram: ecualizador" },
        
        { Keys::kPitchCorrection, "Pitch correction", "音高校正", "ピッチ補正", "Коррекция тона", "Corrección de tono" },
        { Keys::kRetuneSpeed, "Retune Speed", "校正速度", "チューン速度", "Скорость коррекции", "Vel. afinación" },
        { Keys::kVibratoDepth, "Vib. Depth", "颤音深度", "ビブラート深さ", "Глуб. вибрато", "Prof. vibrato" },
        { Keys::kVibratoRate, "Vib. Rate", "颤音速率", "ビブラート速度", "Скор. вибрато", "Tasa vibrato" },
        { Keys::kNoteSplit, "Note Split", "音符分割", "ノート分割", "Разд. нот", "Div. notas" },
        { Keys::kTools, "Tools", "工具", "ツール", "Инструменты", "Herram." },
        { Keys::kAuto, "Auto", "自动", "オート", "Авто", "Auto" },
        { Keys::kSelect, "Select", "选择", "選択", "Выбор", "Selec." },
        { Keys::kDrawNotes, "Draw notes", "绘制音符", "ノートを描画", "Рисовать ноты", "Dib. notas" },
        { Keys::kLineAnchor, "Line anchor", "锚点", "ラインアンカー", "Якорь", "Ancla línea" },
        { Keys::kHandDraw, "Hand draw pitch", "手绘音高", "手描きピッチ", "Рисование высоты", "Dib. tono" },
        
        { Keys::kPlay, "Play", "播放", "再生", "Старт", "Reprod." },
        { Keys::kPause, "Pause", "暂停", "一時停止", "Пауза", "Pausar" },
        { Keys::kLoop, "Loop", "循环", "ループ", "Цикл", "Bucle" },
        { Keys::kTapTempo, "Tap Tempo", "敲击节拍", "タップテンポ", "Тап темп", "Tap tempo" },
        { Keys::kRecord, "Record", "读取音频", "読み込み", "Загрузить", "Cargar" },
        { Keys::kTrackView, "Track View", "轨道视图", "トラックビュー", "Вид дорожки", "Vista pista" },
        { Keys::kPianoRollView, "Piano Roll View", "钢琴卷帘视图", "ピアノロールビュー", "Вид пиано-ролла", "Vista piano" },
        
        { Keys::kTracks, "Tracks", "轨道", "トラック", "Дорожки", "Pistas" },
        { Keys::kProps, "Props", "属性", "プロパティ", "Свойства", "Propiedades" },
        { Keys::kScale, "Scale", "调式", "スケール", "Гамма", "Escala" },
        { Keys::kRootNote, "Root", "根音", "ルート", "Тоника", "Raíz" },
        { Keys::kScrollContinuous, "Cont", "连续", "連続", "Непрерывно", "Continuo" },
        { Keys::kScrollPage, "Page", "分页", "ページ", "Страница", "Página" },
        { Keys::kTime, "Time", "时间", "時間", "Время", "Tiempo" },
        { Keys::kNoSnap, "No Snap", "不吸附", "スナップなし", "Без привязки", "Sin ajuste" },
        { Keys::kChromatic, "Chromatic", "半音阶", "クロマチック", "Хроматический", "Cromático" },
        { Keys::kKeyScale, "Key Scale", "调式音阶", "キースケール", "Гамма тональности", "Escala tonal" },
        { Keys::kUntitled, "Untitled", "未命名", "無題", "Без названия", "Sin título" },
        { Keys::kTrackPrefix, "Track ", "轨道 ", "トラック ", "Дорожка ", "Pista " },
        { Keys::kClip, "Clip", "片段", "クリップ", "Клип", "Clip" },
        { Keys::kImportAudioDialog, "Import Audio", "导入音频", "オーディオをインポート", "Импорт аудио", "Importar audio" },
        { Keys::kOpenProjectDialog, "Open Project", "打开工程", "プロジェクトを開く", "Открыть проект", "Abrir proyecto" },
        { Keys::kAutoDialog, "AUTO", "AUTO", "AUTO", "AUTO", "AUTO" },
        { Keys::kAudioImportInProgress, "Audio import is already in progress. Please try again later.", "音频导入正在进行中，请稍后再试。", "オーディオのインポートは既に進行中です。後でもう一度お試しください。", "Импорт аудио уже выполняется. Повторите попытку позже.", "La importación de audio ya está en curso. Inténtalo de nuevo más tarde." },
        { Keys::kUnsupportedFileType, "Unsupported file type.\nSupported extensions: {0}", "不支持的文件类型。\n支持的扩展名：{0}", "サポートされていないファイル形式です。\n対応する拡張子：{0}", "Неподдерживаемый тип файла.\nПоддерживаемые расширения: {0}", "Tipo de archivo no compatible.\nExtensiones compatibles: {0}" },
        { Keys::kMultipleFilesDetected, "Multiple files detected. Only the first file will be imported.", "检测到多个文件。只会导入第一个文件。", "複数のファイルが検出されました。最初のファイルのみインポートされます。", "Обнаружено несколько файлов. Будет импортирован только первый файл.", "Se detectaron varios archivos. Solo se importará el primero." },
        { Keys::kNoAvailableTracksAfterCurrent, "There are no available tracks after the current track.", "当前轨道之后没有可用轨道。", "現在のトラック以降に使用できるトラックがありません。", "После текущей дорожки нет доступных дорожек.", "No hay pistas disponibles después de la pista actual." },
        { Keys::kImportCountTrimmed, "Only {0} tracks are available after the current track. Extra files were not queued.", "当前轨道之后只有 {0} 条可用轨道，多余文件未加入队列。", "現在のトラック以降で使用できるトラックは {0} 個だけです。余分なファイルはキューに追加されませんでした。", "После текущей дорожки доступно только {0} дорожек. Лишние файлы не добавлены в очередь.", "Solo hay {0} pistas disponibles después de la pista actual. Los archivos adicionales no se añadieron a la cola." },
        { Keys::kAudioImportPreprocessingFailed, "Audio import preprocessing failed. Please try again.", "音频导入预处理失败，请重试。", "オーディオのインポート前処理に失敗しました。もう一度お試しください。", "Не удалось выполнить предварительную обработку импорта аудио. Повторите попытку.", "Falló el preprocesamiento de la importación de audio. Inténtalo de nuevo." },
        { Keys::kAudioImportCommitFailed, "Audio import commit failed. Please try again.", "音频导入提交失败，请重试。", "オーディオのインポートの確定に失敗しました。もう一度お試しください。", "Не удалось завершить импорт аудио. Повторите попытку.", "Falló la confirmación de la importación de audio. Inténtalo de nuevo." },
        { Keys::kImportCountTrimmedTitle, "Import Count Trimmed", "导入数量已截断", "インポート数を調整しました", "Количество импорта ограничено", "Cantidad de importación ajustada" },
        { Keys::kExportInProgress, "An export task is already in progress. Please try again later.", "导出任务正在进行中，请稍后再试。", "エクスポートタスクは既に進行中です。後でもう一度お試しください。", "Задача экспорта уже выполняется. Повторите попытку позже.", "Ya hay una tarea de exportación en curso. Inténtalo de nuevo más tarde." },
        { Keys::kNoAudioClipSelected, "No audio clip is selected. Select a clip on the track first.", "未选择音频片段。请先在轨道上选择一个片段。", "オーディオクリップが選択されていません。まずトラック上のクリップを選択してください。", "Аудиоклип не выбран. Сначала выберите клип на дорожке.", "No hay ningún clip de audio seleccionado. Primero selecciona un clip en la pista." },
        { Keys::kSelectedPlacement, "Selected Placement (Track {0}, Clip {1})", "已选片段（轨道 {0}，片段 {1}）", "選択中の配置（トラック {0}、クリップ {1}）", "Выбранное размещение (дорожка {0}, клип {1})", "Ubicación seleccionada (pista {0}, clip {1})" },
        { Keys::kTrackTarget, "Track {0}", "轨道 {0}", "トラック {0}", "Дорожка {0}", "Pista {0}" },
        { Keys::kBusMasterMix, "Bus (Master Mix)", "总线（主混音）", "バス（マスターミックス）", "Шина (мастер-микс)", "Bus (mezcla maestra)" },
        { Keys::kHelpFileNotFound, "Help file not found: {0}", "找不到帮助文件：{0}", "ヘルプファイルが見つかりません：{0}", "Файл справки не найден: {0}", "No se encontró el archivo de ayuda: {0}" },
        { Keys::kReferenceMenuEntry, "Track {0} - {1} (Mat#{2})", "轨道 {0} - {1}（Mat#{2}）", "トラック {0} - {1}（Mat#{2}）", "Дорожка {0} — {1} (Mat#{2})", "Pista {0} - {1} (Mat#{2})" },
        { Keys::kAutoRef, "AUTO Ref", "AUTO Ref", "AUTO Ref", "AUTO Ref", "AUTO Ref" },
        { Keys::kAutoRefAlignmentFailed, "AUTO Ref alignment failed.", "AUTO Ref 对齐失败。", "AUTO Ref のアライメントに失敗しました。", "Не удалось выполнить выравнивание AUTO Ref.", "Falló la alineación de AUTO Ref." },
        { Keys::kHostManagedActionPrefix, "In VST3 mode this action is managed by your DAW.\n\n", "在 VST3 模式下，此操作由您的 DAW 管理。\n\n", "VST3モードでは、この操作はDAWによって管理されます。\n\n", "В режиме VST3 это действие управляется вашей DAW.\n\n", "En modo VST3, esta acción la gestiona tu DAW.\n\n" },
        { Keys::kVst3ImportAudio, "Please import audio from your DAW in VST3 mode.", "在 VST3 模式下，请从 DAW 导入音频。", "VST3モードではDAWからオーディオをインポートしてください。", "В режиме VST3 импортируйте аудио из DAW.", "Importa audio desde tu DAW en modo VST3." },
        { Keys::kVst3ExportAudio, "Please render/export from your DAW in VST3 mode.", "在 VST3 模式下，请从 DAW 渲染/导出。", "VST3モードではDAWからレンダー/エクスポートしてください。", "В режиме VST3 выполняйте рендеринг/экспорт из DAW.", "Renderiza/exporta desde tu DAW en modo VST3." },
        { Keys::kProjectFileManagementStandalone, "Project file management is handled in the Standalone version.", "工程文件管理由 Standalone 版本处理。", "プロジェクトファイルの管理はスタンドアロン版で行います。", "Управление файлами проектов выполняется в версии Standalone.", "La gestión de archivos de proyecto se realiza en la versión Standalone." },
        { Keys::kOpenRecentProject, "Open Recent Project", "打开最近工程", "最近のプロジェクトを開く", "Открыть недавний проект", "Abrir proyecto reciente" },
        { Keys::kHelpDialog, "Help", "帮助", "ヘルプ", "Справка", "Ayuda" },
        { Keys::kVst3Help, "Open the host DAW plugin help/manual entry for VST3 usage guidance.", "请打开宿主 DAW 的插件帮助/手册条目，查看 VST3 使用指南。", "VST3の使用方法については、ホストDAWのプラグインヘルプ/マニュアルを開いてください。", "Откройте справку/руководство плагина в DAW, чтобы узнать об использовании VST3.", "Abre la ayuda o el manual del plugin en tu DAW para consultar las instrucciones de uso de VST3." },
        { Keys::kVst3ReadAudioNotReady, "This VST3 instance is not ready for audio capture or ARA reading.", "此 VST3 实例尚未准备好进行音频捕获或 ARA 读取。", "このVST3インスタンスはオーディオキャプチャまたはARA読み込みの準備ができていません。", "Этот экземпляр VST3 не готов к захвату аудио или чтению ARA.", "Esta instancia VST3 no está lista para capturar audio ni leer ARA." },
        { Keys::kVst3ReadAudioSelectionNotReady, "The selected item is not ready. Please re-select and try again.", "所选项目尚未准备好。请重新选择后再试。", "選択した項目の準備ができていません。もう一度選択してお試しください。", "Выбранный объект не готов. Выберите его снова и повторите попытку.", "El elemento seleccionado no está listo. Vuelve a seleccionarlo e inténtalo de nuevo." },
        { Keys::kVst3ReadAudioRegionsFailed, "Audio regions could not be processed.", "无法处理音频区域。", "オーディオリージョンを処理できませんでした。", "Не удалось обработать аудиорегионы.", "No se pudieron procesar las regiones de audio." },
        { Keys::kAutoNeedsActiveAra, "AUTO needs an active ARA audio modification.", "AUTO 需要活动的 ARA 音频修改。", "AUTOにはアクティブなARAオーディオモディフィケーションが必要です。", "Для AUTO требуется активная аудиомодификация ARA.", "AUTO necesita una modificación de audio ARA activa." },
        { Keys::kOriginalF0, "Original Pitch", "原始音高", "元のピッチ", "Исходная высота тона", "Tono original" },
        { Keys::kOriginalF0Extracting, "Original Pitch is being extracted. Please retry in a moment.", "正在提取原始音高，请稍后重试。", "元のピッチを抽出中です。しばらくしてからもう一度お試しください。", "Выполняется извлечение исходной высоты тона. Повторите попытку через некоторое время.", "Se está extrayendo el tono original. Inténtalo de nuevo en un momento." },
        { Keys::kOriginalF0ExtractionFailed, "Original Pitch extraction failed for this clip. Re-import the audio to regenerate the original pitch.", "此片段的原始音高提取失败。请重新导入音频以重新生成原始音高。", "このクリップの元のピッチの抽出に失敗しました。元のピッチを再生成するにはオーディオを再インポートしてください。", "Не удалось извлечь исходную высоту тона для этого клипа. Импортируйте аудио заново, чтобы восстановить исходную высоту тона.", "Falló la extracción del tono original para este clip. Vuelve a importar el audio para regenerar el tono original." },
        { Keys::kOriginalF0NotReadyForClip, "Original Pitch is not ready for this clip.", "此片段的原始音高尚未就绪。", "このクリップの元のピッチはまだ準備できていません。", "Исходная высота тона для этого клипа не готова.", "El tono original no está listo para este clip." },
        { Keys::kAutoQueued, "AUTO has been queued.", "AUTO 已加入队列。", "AUTOをキューに追加しました。", "AUTO добавлен в очередь.", "AUTO se ha añadido a la cola." },
        { Keys::kAutoNeedsPitchCurve, "AUTO needs an active pitch curve. Run audio analysis first.", "AUTO 需要活动的音高曲线。请先运行音频分析。", "AUTOにはアクティブなピッチカーブが必要です。先にオーディオ解析を実行してください。", "Для AUTO нужна активная кривая высоты тона. Сначала выполните анализ аудио.", "AUTO necesita una curva de tono activa. Primero ejecuta el análisis de audio." },
        { Keys::kAutoNoContentCommands, "AUTO cannot run because content edit commands are not attached.", "AUTO 无法运行，因为尚未连接内容编辑命令。", "コンテンツ編集コマンドが接続されていないため、AUTOを実行できません。", "AUTO не может работать: команды редактирования содержимого не подключены.", "AUTO no puede ejecutarse porque los comandos de edición de contenido no están conectados." },
        { Keys::kAutoNeedsEditableClip, "AUTO needs an active editable clip.", "AUTO 需要活动的可编辑片段。", "AUTOにはアクティブな編集可能クリップが必要です。", "Для AUTO нужен активный редактируемый клип.", "AUTO necesita un clip editable activo." },
        { Keys::kAutoCannotReadContentSnapshot, "AUTO cannot read the editable content snapshot.", "AUTO 无法读取可编辑内容快照。", "AUTOは編集可能なコンテンツスナップショットを読み込めません。", "AUTO не может прочитать снимок редактируемого содержимого.", "AUTO no puede leer la instantánea del contenido editable." },
        { Keys::kAutoOriginalF0NotReady, "AUTO needs the original pitch to be ready for this clip.", "AUTO 需要此片段的原始音高就绪。", "AUTOにはこのクリップの元のピッチの準備完了が必要です。", "Для AUTO необходимо, чтобы исходная высота тона этого клипа была готова.", "AUTO necesita que el tono original esté listo para este clip." },
        { Keys::kAutoCannotReadCurveSnapshot, "AUTO cannot read the current pitch-curve snapshot.", "AUTO 无法读取当前音高曲线快照。", "AUTOは現在のピッチカーブスナップショットを読み込めません。", "AUTO не может прочитать текущий снимок кривой высоты тона.", "AUTO no puede leer la instantánea actual de la curva de tono." },
        { Keys::kAutoNeedsOriginalF0, "AUTO needs non-empty original pitch data.", "AUTO 需要非空的原始音高数据。", "AUTOには空でない元のピッチデータが必要です。", "Для AUTO нужны непустые данные исходной высоты тона.", "AUTO necesita datos de tono original no vacíos." },
        { Keys::kAutoCannotMapF0Timeline, "AUTO cannot map this clip to an F0 timeline.", "AUTO 无法将此片段映射到 F0 时间线。", "AUTOはこのクリップをF0タイムラインにマッピングできません。", "AUTO не может сопоставить этот клип с временной шкалой F0.", "AUTO no puede asignar este clip a una línea de tiempo F0." },
        { Keys::kAutoNeedsSelection, "AUTO needs a selected note, F0 range, or selection area.", "AUTO 需要选中的音符、F0 范围或选区。", "AUTOには選択したノート、F0範囲、または選択領域が必要です。", "Для AUTO нужна выбранная нота, диапазон F0 или область выделения.", "AUTO necesita una nota, un rango F0 o un área seleccionada." },
        { Keys::kAutoTargetRangeEmpty, "AUTO target range is empty.", "AUTO 目标范围为空。", "AUTOの対象範囲が空です。", "Целевой диапазон AUTO пуст.", "El rango objetivo de AUTO está vacío." },
        { Keys::kAutoCouldNotApply, "AUTO could not be applied.", "无法应用 AUTO。", "AUTOを適用できませんでした。", "Не удалось применить AUTO.", "No se pudo aplicar AUTO." },
        
        { Keys::kClose, "Close", "关闭", "閉じる", "Закрыть", "Cerrar" },
        { Keys::kHelp, "Help...", "帮助...", "ヘルプ...", "Справка...", "Ayuda..." },
        { Keys::kOnboarding, "Onboarding...", "上手引导...", "オンボーディング...", "Введение...", "Introducción..." },
        { Keys::kReplayOnboarding, "Replay onboarding", "重新进入上手引导", "オンボーディングを再表示", "Повторить обучение", "Repetir introducción" },
        { Keys::kUserGuide, "User Guide...", "用户手册...", "ユーザーガイド...", "Руководство пользователя...", "Manual de usuario..." },
        
        { Keys::kMouseSelectTool, "Mouse Select Tool", "鼠标选择工具", "マウス選択ツール", "Инструмент выбора", "Herram. selec." },
        { Keys::kDrawNoteTool, "Draw Note Tool", "绘制音符工具", "ノート描画ツール", "Рисование нот", "Herram. dibujo" },
        { Keys::kLineAnchorTool, "Line Anchor Tool", "锚点工具", "ラインアンカーツール", "Инструмент якоря", "Herram. ancla" },
        { Keys::kHandDrawTool, "Hand Draw Tool", "手绘工具", "手描きツール", "Рисование", "Herram. libre" },

        { Keys::kTooltipPlay, "Play", "播放", "再生", "Воспроизведение", "Reproducir" },
        { Keys::kTooltipPause, "Pause", "暂停", "一時停止", "Пауза", "Pausar" },
        { Keys::kTooltipStop, "Stop", "停止", "停止", "Стоп", "Detener" },
        { Keys::kTooltipLoop, "Loop", "循环", "ループ", "Цикл", "Bucle" },
        { Keys::kTooltipRecord, "Read Audio", "读取音频", "オーディオ読み込み", "Загрузить аудио", "Leer audio" },
        { Keys::kTooltipTrackView, "Track View", "轨道视图", "トラックビュー", "Вид дорожек", "Vista de pistas" },
        { Keys::kTooltipPianoRollView, "Piano Roll View", "钢琴卷帘视图", "ピアノロールビュー", "Пианоролл", "Vista piano roll" },
        { Keys::kTooltipTapTempo, "Tap Tempo", "敲击节拍", "タップテンポ", "Тап-темп", "Tap tempo" },
        { Keys::kTooltipFile, "File Menu", "文件菜单", "ファイルメニュー", "Меню Файл", "Menú Archivo" },
        { Keys::kTooltipEdit, "Edit Menu", "编辑菜单", "編集メニュー", "Меню Правка", "Menú Editar" },
        { Keys::kTooltipView, "View Menu", "视图菜单", "表示メニュー", "Меню Вид", "Menú Ver" },
        { Keys::kTooltipAutoTune, "Auto Correction", "自动校正", "オート補正", "Автокоррекция", "Corrección auto" },
        { Keys::kTooltipSelect, "Selection Tool", "选择工具", "選択ツール", "Инструмент выбора", "Herramienta de selección" },
        { Keys::kTooltipDrawNote, "Draw Note", "绘制音符", "ノート描画", "Рисование нот", "Dibujar nota" },
        { Keys::kTooltipLineAnchor, "Line Anchor", "锚点工具", "ラインアンカー", "Линейный якорь", "Ancla de línea" },
        { Keys::kTooltipHandDraw, "Hand Draw Pitch", "手绘音高", "手描きピッチ", "Рисование тона", "Dibujar tono" },
        { Keys::kTooltipEraser, "Eraser - Remove notes and pitch", "橡皮擦 - 擦除音符和音高", "消しゴム - ノートとピッチを消去", "Ластик - удалить ноты и тон", "Borrador - eliminar notas y tono" },
        { Keys::kTooltipTimeTool, "Time Tool - Drag handles to retime audio", "时间工具 - 拖动手柄重定时", "タイムツール - ハンドルで時間調整", "Инструмент времени - перетягивайте маркеры", "Herramienta de tiempo - Arrastra anclajes" },
        { Keys::kTooltipTrackPanel, "Track Panel", "轨道面板", "トラックパネル", "Панель дорожек", "Panel de pistas" },
        { Keys::kTooltipParameterPanel, "Parameter Panel", "参数面板", "パラメータパネル", "Панель параметров", "Panel de parámetros" },
        { Keys::kTooltipBpm, "Tempo (BPM)", "节拍速度 (BPM)", "テンポ (BPM)", "Темп (BPM)", "Tempo (BPM)" },
        { Keys::kTooltipTimeline, "Playback Time", "播放时间", "再生時間", "Время воспроизведения", "Tiempo de reproducción" },
        { Keys::kTooltipTimeUnit, "Toggle Time/Bars Display", "切换时间/小节显示", "時間/小節表示切替", "Переключить время/такты", "Alternar tiempo/compases" },
        { Keys::kTooltipScrollMode, "Scroll Mode - Toggle between Continuous and Page scroll", "滚动模式 - 切换连续/翻页滚动", "スクロールモード - 連続/ページ切替", "Режим прокрутки - непрерывная/постраничная", "Modo desplazamiento - Continuo/Página" },
        { Keys::kTooltipRetuneSpeed, "Retune Speed - Controls how fast pitch is corrected", "校正速度 - 控制音高校正的速度", "チューン速度 - ピッチ補正の速度を制御", "Скорость коррекции - насколько быстро корректируется тон", "Vel. afinación - Controla la rapidez de corrección" },
        { Keys::kTooltipVibratoDepth, "Vibrato Depth - Controls vibrato amplitude", "颤音深度 - 控制颤音幅度", "ビブラート深さ - ビブラートの振幅を制御", "Глубина вибрато - амплитуда вибрато", "Prof. vibrato - Controla la amplitud" },
        { Keys::kTooltipVibratoRate, "Vibrato Rate - Controls vibrato speed", "颤音速率 - 控制颤音频率", "ビブラート速度 - ビブラートの速さを制御", "Скорость вибрато - частота вибрато", "Tasa vibrato - Controla la velocidad" },
        { Keys::kTooltipNoteSplit, "Note Split - Threshold for splitting notes", "音符分割 - 控制音符分割阈值", "ノート分割 - ノート分割の閾値を制御", "Разделение нот - порог разделения", "Div. notas - Umbral de división" },
        { Keys::kTooltipPitchModulation, "Modulation - Vibrato Depth", "颤音深度调制", "モジュレーション - ビブラート深度", "Модуляция - глубина вибрато", "Modulación - profundidad de vibrato" },
        { Keys::kTooltipPitchDrift, "Drift - Pitch Drift Correction", "漂移修正", "ドリフト - ピッチドリフト補正", "Дрейф - коррекция дрейфа", "Deriva - corrección de deriva" },
        { Keys::kTooltipEqMaximize, "Expand EQ Editor", "展开EQ编辑器", "EQエディタを展開", "Развернуть редактор EQ", "Expandir editor EQ" },
        { Keys::kTooltipEqMinimize, "Collapse to Preview", "收起为预览", "プレビューに折りたたむ", "Свернуть в предпросмотр", "Colapsar a vista previa" },
        { Keys::kTooltipEqBypass, "Toggle EQ Bypass", "切换EQ旁通", "EQバイパス切替", "Переключить обход EQ", "Alternar bypass EQ" },
        { Keys::kTooltipEqRemove, "Remove EQ from Note", "删除音符的EQ处理", "ノートからEQを削除", "Удалить EQ из ноты", "Eliminar EQ de la nota" },
        { Keys::kTooltipEqClose, "Close EQ Editor", "关闭EQ编辑器", "EQエディタを閉じる", "Закрыть редактор EQ", "Cerrar editor EQ" },
        { Keys::kToolPitch, "Pitch", "音高", "ピッチ", "Высота тона", "Tono" },
        { Keys::kToolModulation, "Modulation", "调制", "モジュレーション", "Модуляция", "Modulación" },
        { Keys::kToolDrift, "Drift", "漂移", "ドリフト", "Дрейф", "Deriva" },
        { Keys::kToolVolumeEnvelope, "Volume Envelope", "音量包络", "ボリュームエンベロープ", "Огибающая громкости", "Sobre volumen" },
        { Keys::kToolScissors, "Scissors", "剪刀", "ハサミ", "Ножницы", "Tijeras" },
        { "Choose your editing workflow", "Choose your editing workflow", "选择编辑工作流", "編集ワークフローを選択", "Выберите рабочий процесс редактирования", "Elige tu flujo de edición" },
        { "OpenTune edits notes; OpenDyne edits waveform blobs. The choice really switches the editor, and you can change it later in Options > Editing.", "OpenTune edits notes; OpenDyne edits waveform blobs. The choice really switches the editor, and you can change it later in Options > Editing.", "OpenTune 编辑音符；OpenDyne 编辑波形块。此选择会切换编辑器，之后可在“选项 > 编辑”中更改。", "OpenTuneはノートを編集し、OpenDyneは波形ブロブを編集します。この選択でエディターが実際に切り替わり、後から「オプション > 編集」で変更できます。", "OpenTune редактирует ноты, а OpenDyne — волновые блоки. Выбор действительно переключает редактор, и позже его можно изменить в разделе «Настройки > Редактирование».", "OpenTune edita notas; OpenDyne edita bloques de forma de onda. La elección cambia realmente el editor y puedes cambiarla después en Opciones > Edición." },
        { "Not now", "Not now", "暂不选择", "今は選択しない", "Не сейчас", "Ahora no" },
        { "Previous", "Previous", "上一步", "前へ", "Назад", "Anterior" },
        { "Next", "Next", "下一步", "次へ", "Далее", "Siguiente" },
        { "Finish", "Finish", "完成", "完了", "Готово", "Finalizar" },
        { "Exit guide", "Exit guide", "退出指南", "ガイドを終了", "Выйти из руководства", "Salir de la guía" },
        { "File menu", "File menu", "文件菜单", "ファイルメニュー", "Меню «Файл»", "Menú Archivo" },
        { "Edit menu", "Edit menu", "编辑菜单", "編集メニュー", "Меню «Правка»", "Menú Editar" },
        { "View menu", "View menu", "视图菜单", "表示メニュー", "Меню «Вид»", "Menú Ver" },
        { "Audio entry", "Audio entry", "音频入口", "オーディオ入力", "Источник аудио", "Entrada de audio" },
        { "Track view", "Track view", "轨道视图", "トラックビュー", "Вид дорожки", "Vista de pista" },
        { "Piano roll", "Piano roll", "钢琴卷帘", "ピアノロール", "Пианоролл", "Piano roll" },
        { "Hand Draw", "Hand Draw", "手绘", "手描き", "Ручное рисование", "Dibujo a mano" },
        { "Vibrato Depth", "Vibrato Depth", "颤音深度", "ビブラートの深さ", "Глубина вибрато", "Profundidad del vibrato" },
        { "Vibrato Rate", "Vibrato Rate", "颤音速率", "ビブラートの速度", "Скорость вибрато", "Velocidad del vibrato" },
        { "AUTO / SNAP", "AUTO / SNAP", "自动 / 吸附", "AUTO / SNAP", "АВТО / ПРИВЯЗКА", "AUTO / AJUSTE" },
        { "Pitch Grid", "Pitch Grid", "音高网格", "ピッチグリッド", "Сетка высоты тона", "Cuadrícula de tono" },
        { "Overview", "Overview", "概览", "概要", "Обзор", "Vista general" },
        { "Audition", "Audition", "试听", "試聴", "Прослушивание", "Audición" },
        { "Help and options", "Help and options", "帮助和选项", "ヘルプとオプション", "Справка и настройки", "Ayuda y opciones" },
        { "Import audio, save projects and export from File.", "Import audio, save projects and export from File.", "从“文件”导入音频、保存工程并导出。", "「ファイル」からオーディオをインポートし、プロジェクトを保存・エクスポートします。", "Импортируйте аудио, сохраняйте проекты и экспортируйте их через меню «Файл».", "Importa audio, guarda proyectos y exporta desde Archivo." },
        { "In a plugin, do not import, save or export here; choose audio in the host.", "In a plugin, do not import, save or export here; choose audio in the host.", "在插件中不要在此导入、保存或导出；请在宿主中选择音频。", "プラグインではここからインポート、保存、エクスポートせず、ホストでオーディオを選択してください。", "В плагине не импортируйте, не сохраняйте и не экспортируйте здесь; выберите аудио в хосте.", "En un plugin, no importes, guardes ni exportes aquí; elige el audio en el host." },
        { "Edit contains the editing history and undo or redo actions.", "Edit contains the editing history and undo or redo actions.", "“编辑”包含编辑历史以及撤销和重做操作。", "「編集」には編集履歴と元に戻す・やり直す操作があります。", "В меню «Правка» находятся история редактирования, а также отмена и повтор действий.", "Editar contiene el historial de edición y las acciones de deshacer y rehacer." },
        { "View controls the visible editor and display settings.", "View controls the visible editor and display settings.", "“视图”控制可见的编辑器和显示设置。", "「表示」では表示するエディターと表示設定を操作します。", "Меню «Вид» управляет отображаемым редактором и настройками отображения.", "Ver controla el editor visible y los ajustes de visualización." },
        { "Import a file, or double-click a clip. The audio appears in the track view.", "Import a file, or double-click a clip. The audio appears in the track view.", "导入文件，或双击片段。音频会显示在轨道视图中。", "ファイルをインポートするか、クリップをダブルクリックします。オーディオがトラックビューに表示されます。", "Импортируйте файл или дважды щёлкните клип. Аудио появится в представлении дорожек.", "Importa un archivo o haz doble clic en un clip. El audio aparecerá en la vista de pista." },
        { "Track view shows imported audio clips. Double-click a clip to enter the piano roll editor.", "Track view shows imported audio clips. Double-click a clip to enter the piano roll editor.", "轨道视图显示导入的音频片段。双击片段进入钢琴卷帘编辑器。", "トラックビューにはインポートしたオーディオクリップが表示されます。クリップをダブルクリックするとピアノロールエディターに入ります。", "В представлении дорожек отображаются импортированные аудиоклипы. Дважды щёлкните клип, чтобы открыть редактор пиано-ролла.", "La vista de pista muestra los clips de audio importados. Haz doble clic en un clip para abrir el editor de piano roll." },
        { "Select a host region to read its audio. Playback remains controlled by the host.", "Select a host region to read its audio. Playback remains controlled by the host.", "选择宿主区域以读取其音频。播放仍由宿主控制。", "ホストのリージョンを選択してオーディオを読み込みます。再生は引き続きホストが制御します。", "Выберите область хоста, чтобы считать её аудио. Воспроизведение по-прежнему управляется хостом.", "Selecciona una región del host para leer su audio. La reproducción sigue controlada por el host." },
        { "Click Read Audio, start host playback, then click again to end capture.", "Click Read Audio, start host playback, then click again to end capture.", "点击“读取音频”，开始宿主播放，然后再次点击结束捕获。", "「オーディオを読み込み」をクリックし、ホストの再生を開始してから、もう一度クリックして取り込みを終了します。", "Нажмите «Загрузить аудио», запустите воспроизведение в хосте, затем нажмите ещё раз, чтобы завершить захват.", "Haz clic en Leer audio, inicia la reproducción del host y vuelve a hacer clic para terminar la captura." },
        { "Waveform blobs, pitch, curves and the time ruler are shown here. Empty content reads: Import or read audio first.", "After importing audio, enter the editor view; a pitch curve should be visible.", "在导入音频后进入编辑器视图，应有音高曲线可见。", "オーディオをインポートしてエディター表示にすると、ピッチカーブが見えるはずです。", "После импорта аудио в редакторе должна отображаться кривая высоты тона.", "Tras importar el audio, la curva de tono debería verse en el editor." },
        { "Keys, notes, pitch curves and the time ruler are shown here. Empty content reads: Import or read audio first.", "Keys, notes, pitch curves and the time ruler are shown here.", "此处显示琴键、音符、音高曲线和时间标尺。", "ここには鍵盤、ノート、ピッチカーブ、時間ルーラーが表示されます。", "Здесь отображаются клавиши, ноты, кривые высоты тона и шкала времени.", "Aquí se muestran las teclas, las notas, las curvas de tono y la regla de tiempo." },
        { "Select notes or waveform blobs before editing them.", "The Select tool supports single, multiple and marquee selection.", "选择工具，可以单选、多选和框选。", "選択ツールでは、単体選択、複数選択、範囲選択ができます。", "Инструмент выбора поддерживает одиночный, множественный и рамный выбор.", "La herramienta de selección admite selección única, múltiple y por recuadro." },
        { "Draw Note creates and edits note objects.", "Draw Note creates and edits note objects.", "“绘制音符”用于创建和编辑音符对象。", "「ノート描画」はノートオブジェクトを作成・編集します。", "«Рисование нот» создаёт и редактирует объекты нот.", "Dibujar nota crea y edita objetos de nota." },
        { "Line Anchor places points that shape the pitch curve.", "Line Anchor places points that shape the pitch curve.", "“锚点”用于放置塑造音高曲线的点。", "「ラインアンカー」はピッチカーブの形状を決める点を配置します。", "«Линейный якорь» размещает точки, формирующие кривую высоты тона.", "Ancla de línea coloca puntos que dan forma a la curva de tono." },
        { "Hand Draw directly edits the pitch curve.", "Hand Draw directly edits the pitch curve.", "“手绘”直接编辑音高曲线。", "「手描き」はピッチカーブを直接編集します。", "«Ручное рисование» напрямую редактирует кривую высоты тона.", "Dibujo a mano edita directamente la curva de tono." },
        { "Pitch edits the selected waveform blob's pitch.", "Pitch edits the selected waveform blob's pitch.", "“音高”编辑所选波形块的音高。", "「ピッチ」は選択した波形ブロブのピッチを編集します。", "«Высота тона» редактирует высоту выбранного волнового блока.", "Tono edita el tono del bloque de forma de onda seleccionado." },
        { "Modulation edits expressive pitch movement in a blob.", "Modulation edits expressive pitch movement in a blob.", "“调制”编辑波形块中的表现性音高变化。", "「モジュレーション」はブロブ内の表情豊かなピッチ変化を編集します。", "«Модуляция» редактирует выразительное движение высоты тона в блоке.", "Modulación edita el movimiento expresivo del tono en un bloque." },
        { "Drift corrects slower pitch movement in a blob.", "Drift corrects slower pitch movement in a blob.", "“漂移”修正波形块中较慢的音高变化。", "「ドリフト」はブロブ内のゆっくりしたピッチ変化を補正します。", "«Дрейф» корректирует медленное движение высоты тона в блоке.", "Deriva corrige el movimiento lento del tono en un bloque." },
        { "Volume Envelope changes level over time.", "Volume Envelope changes level over time.", "“音量包络”改变音量随时间的变化。", "「ボリュームエンベロープ」は時間経過に伴うレベルを変更します。", "«Огибающая громкости» изменяет уровень во времени.", "La envolvente de volumen cambia el nivel con el tiempo." },
        { "Scissors splits waveform blobs into editable parts.", "Scissors splits waveform blobs into editable parts.", "“剪刀”将波形块拆分为可编辑部分。", "「ハサミ」は波形ブロブを編集可能な部分に分割します。", "«Ножницы» разделяют волновые блоки на редактируемые части.", "Tijeras divide los bloques de forma de onda en partes editables." },
        { "Retune Speed controls how quickly note correction follows the target.", "Retune Speed controls how quickly note correction follows the target.", "“校正速度”控制音符校正跟随目标的速度。", "「チューン速度」はノート補正がターゲットに追従する速さを制御します。", "«Скорость коррекции» управляет скоростью следования коррекции нот за целью.", "La velocidad de afinación controla la rapidez con que la corrección de notas sigue al objetivo." },
        { "Vibrato Depth controls the amount of vibrato.", "Vibrato Depth controls the amount of vibrato.", "“颤音深度”控制颤音量。", "「ビブラートの深さ」はビブラートの量を制御します。", "«Глубина вибрато» управляет величиной вибрато.", "La profundidad del vibrato controla la cantidad de vibrato." },
        { "Vibrato Rate controls how quickly vibrato cycles.", "Vibrato Rate controls how quickly vibrato cycles.", "“颤音速率”控制颤音循环的速度。", "「ビブラートの速度」はビブラートの周期の速さを制御します。", "«Скорость вибрато» управляет скоростью циклов вибрато.", "La velocidad del vibrato controla la rapidez de sus ciclos." },
        { "AUTO applies the current note correction settings.", "AUTO applies the current note correction settings.", "“自动”应用当前音符校正设置。", "「AUTO」は現在のノート補正設定を適用します。", "«АВТО» применяет текущие настройки коррекции нот.", "AUTO aplica los ajustes actuales de corrección de notas." },
        { "SNAP constrains blob edits to the selected grid.", "SNAP constrains blob edits to the selected grid.", "“吸附”将波形块编辑限制到所选网格。", "「SNAP」はブロブの編集を選択したグリッドに制限します。", "«ПРИВЯЗКА» ограничивает редактирование блоков выбранной сеткой.", "AJUSTE limita la edición de bloques a la cuadrícula seleccionada." },
        { "Pitch Grid chooses how OpenDyne pitch edits snap.", "Pitch Grid chooses how OpenDyne pitch edits snap.", "“音高网格”选择 OpenDyne 音高编辑的吸附方式。", "「ピッチグリッド」はOpenDyneのピッチ編集のスナップ方法を選択します。", "«Сетка высоты тона» задаёт привязку при редактировании высоты тона в OpenDyne.", "Cuadrícula de tono elige cómo se ajustan las ediciones de tono de OpenDyne." },
        { "Transport shows the current root and scale; More contains additional scale choices.", "Scale shows the current root and scale; click the dropdown for more scale options.", "调式显示当前的根音和音阶，点击下拉查看更多调式选项。", "スケールには現在のルートとスケールが表示されます。ドロップダウンをクリックすると、さらに多くのスケール候補が表示されます。", "Гамма показывает текущую тонику и гамму; нажмите на раскрывающийся список, чтобы увидеть другие варианты гамм.", "La escala muestra la raíz y la escala actuales; haz clic en el desplegable para ver más opciones de escala." },
        { "Overview moves through the whole clip while keeping the current zoom.", "Overview moves through the whole clip while keeping the current zoom.", "“概览”在保持当前缩放的同时浏览整个片段。", "「概要」は現在のズームを保ったままクリップ全体を移動します。", "«Обзор» перемещается по всему клипу, сохраняя текущий масштаб.", "Vista general recorre todo el clip manteniendo el zoom actual." },
        { "Play, pause, stop and loop here.", "Play, pause, stop and loop here.", "在此播放、暂停、停止和循环。", "ここで再生、一時停止、停止、ループを操作します。", "Здесь можно воспроизводить, ставить на паузу, останавливать и зацикливать.", "Reproduce, pausa, detén y repite aquí." },
        { "Audition with the plugin host transport.", "Audition with the plugin host transport.", "使用插件宿主走带试听。", "プラグインホストのトランスポートで試聴します。", "Прослушивайте с помощью транспорта хоста плагина.", "Escucha con el transporte del host del plugin." },
        { "Options can change the editing mode and enable EQ. Help opens the user guide.", "Options can change the editing mode and enable EQ. Help opens the user guide.", "“选项”可更改编辑模式并启用 EQ。“帮助”可打开用户指南。", "「オプション」では編集モードを変更してEQを有効にできます。「ヘルプ」からユーザーガイドを開けます。", "«Настройки» меняют режим редактирования и включают EQ. «Справка» открывает руководство пользователя.", "Opciones puede cambiar el modo de edición y activar EQ. Ayuda abre la guía del usuario." },
        { "Pitch Detection Model", "Pitch Detection Model", "音高检测模型", "ピッチ検出モデル", "Модель определения высоты тона", "Modelo de detección de tono" },
        { "Enable Experimental Features (Reference Track, Time Stretch Tool)", "Enable Experimental Features (Reference Track, Time Stretch Tool)", "启用实验性功能（参考轨、伸缩工具）", "実験的機能を有効にする（リファレンストラック、タイムストレッチツール）", "Включить экспериментальные функции (референсная дорожка, инструмент растяжения времени)", "Activar funciones experimentales (pista de referencia, herramienta de estiramiento temporal)" },
        { "Hint: Reference Track and Time Stretch Tool are still under development and may contain bugs.", "Hint: Reference Track and Time Stretch Tool are still under development and may contain bugs.", "提示：参考轨与伸缩工具目前仍不完善，属于实验性功能，可能存在 Bug。", "ヒント：リファレンストラックとタイムストレッチツールはまだ開発中の実験的機能であり、不具合がある場合があります。", "Примечание: референсная дорожка и инструмент растяжения времени всё ещё находятся в разработке и могут содержать ошибки.", "Aviso: La pista de referencia y la herramienta de estiramiento temporal siguen en desarrollo y pueden contener errores." },
        { "AUTO Ref Mode", "AUTO Ref Mode", "AUTO Ref 模式", "AUTO Refモード", "Режим AUTO Ref", "Modo AUTO Ref" },
        { "Standard AUTO", "Standard AUTO", "普通 AUTO", "通常のAUTO", "Обычный AUTO", "AUTO estándar" },
        { "Semitones", "Semitones", "半音", "半音", "Полутоны", "Semitonos" },
        { "Cents", "Cents", "音分", "セント", "Центы", "Cents" },
        { "Reset", "Reset", "重置", "リセット", "Сбросить", "Restablecer" },
        { "Confirm", "Confirm", "确认", "確定", "Подтвердить", "Confirmar" },
        { "Opening file...", "Opening file...", "正在打开文件...", "ファイルを開いています...", "Открытие файла...", "Abriendo archivo..." },
        { "Could not open this audio file.\n", "Could not open this audio file.\n", "无法打开该音频文件。\n", "このオーディオファイルを開けません。\n", "Не удалось открыть аудиофайл.\n", "No se pudo abrir este archivo de audio.\n" },
        { "File: ", "File: ", "文件：", "ファイル：", "Файл: ", "Archivo: " },
        { "Extension: ", "Extension: ", "扩展名：", "拡張子：", "Расширение: ", "Extensión: " },
        { "Exists: ", "Exists: ", "存在：", "存在：", "Существует: ", "Existe: " },
        { "File size: ", "File size: ", "文件大小：", "ファイルサイズ：", "Размер файла: ", "Tamaño del archivo: " },
        { "bytes", "bytes", "字节", "バイト", "байт", "bytes" },
        { "Stream accessible: ", "Stream accessible: ", "可开流：", "ストリームを開ける：", "Поток доступен: ", "Flujo accesible: " },
        { "Supported formats: ", "Supported formats: ", "当前支持：", "対応形式：", "Поддерживаемые форматы: ", "Formatos compatibles: " },
        { "No audio decoders are registered in this environment.", "No audio decoders are registered in this environment.", "当前环境未注册可用音频解码器。", "この環境には利用可能なオーディオデコーダーが登録されていません。", "В этой среде не зарегистрированы аудиодекодеры.", "No hay decodificadores de audio registrados en este entorno." },
        { "Registered decoders: ", "Registered decoders: ", "已注册解码器：", "登録済みデコーダー：", "Зарегистрированные декодеры: ", "Decodificadores registrados: " },
        { "Container diagnostics: ", "Container diagnostics: ", "容器诊断：", "コンテナ診断：", "Диагностика контейнера: ", "Diagnóstico del contenedor: " },
        { "Reading audio data...", "Reading audio data...", "正在读取音频数据...", "オーディオデータを読み込んでいます...", "Чтение аудиоданных...", "Leyendo datos de audio..." },
        { "Loading complete", "Loading complete", "加载完成", "読み込み完了", "Загрузка завершена", "Carga completada" },
        { "Failed to read audio data. The file may be corrupted or use an unsupported encoding.", "Failed to read audio data. The file may be corrupted or use an unsupported encoding.", "读取音频数据失败，文件可能损坏或编码不受支持。", "オーディオデータを読み込めませんでした。ファイルが破損しているか、未対応のエンコード形式の可能性があります。", "Не удалось прочитать аудиоданные. Файл может быть повреждён или иметь неподдерживаемую кодировку.", "No se pudieron leer los datos de audio. El archivo puede estar dañado o usar una codificación no compatible." },
        { "Create output file failed", "Could not create the output file", "无法创建输出文件", "出力ファイルを作成できませんでした", "Не удалось создать выходной файл", "No se pudo crear el archivo de salida" },
        { "Unable to create WAV writer", "Unable to create WAV writer", "无法创建 WAV 写入器", "WAVライターを作成できませんでした", "Не удалось создать средство записи WAV", "No se pudo crear el escritor de archivos WAV" },
        { "Placement audio is unavailable", "Placement audio is unavailable", "片段音频不可用", "クリップのオーディオを利用できません", "Аудио клипа недоступно", "El audio del clip no está disponible" },
        { "Invalid track ID: ", "Invalid track ID: ", "无效的轨道ID: ", "無効なトラックID：", "Недопустимый ID дорожки: ", "ID de pista no válido: " },
        { "Invalid clip index ", "Invalid clip index ", "无效的片段索引 ", "無効なクリップインデックス：", "Недопустимый индекс клипа: ", "Índice de clip no válido: " },
        { "Clip audio length is zero", "Clip audio length is zero", "片段音频长度为零", "クリップのオーディオ長がゼロです", "Длительность аудио клипа равна нулю", "La duración de audio del clip es cero" },
        { " has no audio clips", " has no audio clips", " 没有音频片段", "にオーディオクリップがありません", " не содержит аудиоклипов", " no tiene clips de audio" },
        { "Total audio length is zero or invalid", "Total audio length is zero or invalid", "音频总长度为零或无效", "オーディオの合計長がゼロまたは無効です", "Общая длительность аудио равна нулю или недопустима", "La duración total del audio es cero o no válida" },
        { Keys::kOriginalF0NotReady, "Original Pitch is not ready.", "原始音高尚未就绪。", "元のピッチの準備ができていません。", "Исходная высота тона не готова.", "El tono original no está listo." },
        { "Processing audio", "Processing audio", "正在处理音频", "オーディオを処理しています", "Обработка аудио", "Procesando audio" },
        { "Analyzing reference Clip", "Analyzing reference Clip", "正在分析参考 Clip", "リファレンスClipを解析しています", "Анализ референсного Clip", "Analizando el Clip de referencia" },
        { "Rendering (", "Rendering (", "渲染中 (", "レンダリング中 (", "Рендеринг (", "Renderizando (" },
        { "Failed to start OriginalF0 analysis.", "Failed to start Original Pitch analysis.", "未能启动原始音高分析。", "元のピッチの解析を開始できませんでした。", "Не удалось запустить анализ исходной высоты тона.", "No se pudo iniciar el análisis del tono original." },
        { Keys::kRenderFailure, "Render Failure", "渲染失败", "レンダリング失敗", "Ошибка рендеринга", "Error de renderizado" },
        { Keys::kRenderFailed, "Render failed", "渲染失败", "レンダリングに失敗しました", "Ошибка рендеринга", "Falló el renderizado" },
        { Keys::kRenderFailureDetail, "Render failed; dry audio may be used. Reason: {0}", "渲染失败，可能回退干声。原因：{0}", "レンダリングに失敗しました。ドライ音声に切り替わる場合があります。原因：{0}", "Ошибка рендеринга; может использоваться необработанный звук. Причина: {0}", "Falló el renderizado; puede usarse el audio sin procesar. Motivo: {0}" },
        { "Reference Clip AUTO aligns pitch and timing to the reference.", "Reference Clip AUTO aligns pitch and timing to the reference.", "按参考 Clip 自动修音并对齐节奏", "リファレンスClipに合わせてピッチとタイミングを自動調整", "AUTO по референсному Clip корректирует высоту тона и выравнивает ритм.", "AUTO con Clip de referencia ajusta el tono y alinea el ritmo." },
        { "A reference is bound, but GAME backend/models are unavailable; standard AUTO will run.", "A reference is bound, but GAME backend/models are unavailable; standard AUTO will run.", "已绑定参考源，但当前缺少 GAME backend / models，本次执行普通 AUTO。", "リファレンスは設定されていますが、GAME backend/modelsがないため通常のAUTOを実行します。", "Референс привязан, но GAME backend/models недоступны; будет выполнен обычный AUTO.", "Hay una referencia vinculada, pero no están disponibles GAME backend/models; se ejecutará AUTO estándar." },
        { "AUTO corrects pitch by snapping to nearby scale notes.", "AUTO corrects pitch by snapping to nearby scale notes.", "自动修音（吸附到临近音阶）", "AUTOで近くの音階にスナップしてピッチを補正", "AUTO корректирует высоту тона, привязывая её к ближайшим ступеням гаммы.", "AUTO corrige el tono ajustándolo a las notas cercanas de la escala." },
        { Keys::kPitchShift, "Pitch Shift", "整体移调", "全体のピッチを移調", "Транспонирование", "Transposición" },
        { Keys::kPreferences, "Preferences", "偏好设置", "環境設定", "Настройки", "Preferencias" },
        { Keys::kHostControlledTransport, "Host-controlled transport", "由宿主控制走带", "ホスト制御のトランスポート", "Транспорт под управлением хоста", "Transporte controlado por el host" },
        { Keys::kPitchEditing, "Pitch editing", "音高编辑", "ピッチ編集", "Редактирование высоты тона", "Edición de tono" },
        { Keys::kSplitNotes, "Split notes", "切割音符", "ノートを分割", "Разделить ноты", "Dividir notas" },
        { "EQ frequency equalizer", "EQ frequency equalizer", "EQ 频率均衡", "EQ周波数イコライザー", "Частотный эквалайзер EQ", "Ecualizador de frecuencia EQ" },
        { "Collapse to preview", "Collapse to preview", "收起为预览", "プレビューに折りたたむ", "Свернуть в предпросмотр", "Contraer a vista previa" },
        { "Expand EQ editor", "Expand EQ editor", "展开EQ编辑器", "EQエディターを展開", "Развернуть редактор EQ", "Expandir editor EQ" },
        { "Bypass EQ", "Bypass EQ", "旁通EQ", "EQをバイパス", "Обойти EQ", "Desactivar EQ" },
        { "Enable EQ", "Enable EQ", "启用EQ", "EQを有効化", "Включить EQ", "Activar EQ" },
        { "Remove EQ processing", "Remove EQ processing", "删除EQ处理", "EQ処理を削除", "Удалить обработку EQ", "Eliminar procesamiento EQ" },
        { "Hide EQ preview", "Hide EQ preview", "隐藏EQ预览", "EQプレビューを非表示", "Скрыть предпросмотр EQ", "Ocultar vista previa de EQ" },
        { "Minimize to preview", "Minimize to preview", "最小化为预览", "プレビューに最小化", "Свернуть в предпросмотр", "Minimizar a vista previa" },
        { "Enable this filter", "Enable this filter", "启用此滤波器", "このフィルターを有効化", "Включить этот фильтр", "Activar este filtro" },
        { "Bypass this filter", "Bypass this filter", "旁通此滤波器", "このフィルターをバイパス", "Обойти этот фильтр", "Desactivar este filtro" },
        { "Delete this filter", "Delete this filter", "删除此滤波器", "このフィルターを削除", "Удалить этот фильтр", "Eliminar este filtro" },
        { "Remove EQ?", "Remove EQ?", "确认移除 EQ?", "EQを削除しますか？", "Удалить EQ?", "¿Eliminar EQ?" },
        { "Don't show again", "Don't show again", "不再提示", "今後表示しない", "Больше не показывать", "No volver a mostrar" },
        { "Edit time grid", "Edit time grid", "编辑时间网格", "タイムグリッドを編集", "Изменить временную сетку", "Editar cuadrícula de tiempo" },
        { "Edit EQ", "Edit EQ", "编辑 EQ", "EQを編集", "Изменить EQ", "Editar EQ" },
        { "Remove EQ", "Remove EQ", "移除 EQ", "EQを削除", "Удалить EQ", "Eliminar EQ" },
        { "Edit note", "Edit note", "编辑音符", "ノートを編集", "Изменить ноту", "Editar nota" },
        { "Delete note", "Delete note", "删除音符", "ノートを削除", "Удалить ноту", "Eliminar nota" },
        { Keys::kEraser, "Eraser", "橡皮擦", "消しゴム", "Ластик", "Borrador" },
        { "Resize note", "Resize note", "调整音符长度", "ノートの長さを調整", "Изменить длину ноты", "Cambiar duración de nota" },
        { "Move note", "Move note", "移动音符", "ノートを移動", "Переместить ноту", "Mover nota" },
        { "Pitch snap", "Pitch snap", "音高吸附", "ピッチをスナップ", "Привязка высоты тона", "Ajuste magnético del tono" },
        { "Modulation depth", "Modulation depth", "调制深度", "モジュレーションの深さ", "Глубина модуляции", "Profundidad de modulación" },
        { "Drift correction", "Drift correction", "漂移修正", "ドリフト補正", "Коррекция дрейфа", "Corrección de deriva" },
        { "Split note", "Split note", "音符分割", "ノートを分割", "Разделить ноту", "Dividir nota" },
        { "Merge notes", "Merge notes", "音符合并", "ノートを結合", "Объединить ноты", "Unir notas" },
        { "Hand-drawn curve", "Hand-drawn curve", "手绘曲线", "手描きカーブ", "Кривая, нарисованная вручную", "Curva dibujada a mano" },
        { "Draw note", "Draw note", "绘制音符", "ノートを描画", "Нарисовать ноту", "Dibujar nota" },
        { "Anchor correction", "Anchor correction", "锚点修正", "アンカー補正", "Коррекция якоря", "Corrección de ancla" },
        { "AUTO correction", "AUTO correction", "自动调音", "AUTO補正", "Автокоррекция", "Corrección automática" },
        { "Drag time handle", "Drag time handle", "拖动时间手柄", "タイムハンドルをドラッグ", "Перетащить маркер времени", "Arrastrar el controlador de tiempo" },
        { "Insert time handle", "Insert time handle", "插入时间手柄", "タイムハンドルを挿入", "Вставить маркер времени", "Insertar el controlador de tiempo" },
        { "Delete time handle", "Delete time handle", "删除时间手柄", "タイムハンドルを削除", "Удалить маркер времени", "Eliminar el controlador de tiempo" },
        { "Delete Clip", "Delete Clip", "删除片段", "クリップを削除", "Удалить клип", "Eliminar clip" },
        { "Move Clip", "Move Clip", "移动片段", "クリップを移動", "Переместить клип", "Mover clip" },
        { "Adjust Gain", "Adjust Gain", "调整增益", "ゲインを調整", "Изменить усиление", "Ajustar ganancia" },
        { "Trim Clip", "Trim Clip", "裁剪片段", "クリップをトリミング", "Обрезать клип", "Recortar clip" },
        { "Adjust Fade", "Adjust Fade", "调整淡变", "フェードを調整", "Изменить затухание", "Ajustar fundido" },
        { "Change Reference Binding", "Change Reference Binding", "调整参考绑定", "リファレンスの割り当てを変更", "Изменить привязку референса", "Cambiar vínculo de referencia" },
        { "Project Operation", "Project Operation", "工程操作", "プロジェクト操作", "Операция с проектом", "Operación del proyecto" },
        { "Another project operation is already in progress.", "Another project operation is already in progress.", "另一个工程操作正在进行中。", "別のプロジェクト操作が進行中です。", "Другая операция с проектом уже выполняется.", "Ya hay otra operación del proyecto en curso." },
        { "Save Project Failed", "Save Project Failed", "保存工程失败", "プロジェクトを保存できませんでした", "Не удалось сохранить проект", "Error al guardar el proyecto" },
        { "Open Project Failed", "Open Project Failed", "打开工程失败", "プロジェクトを開けませんでした", "Не удалось открыть проект", "Error al abrir el proyecto" },
        { Keys::kSelectAudioFilesToImport, "Select audio files to import", "选择要导入的音频文件", "インポートするオーディオファイルを選択", "Выберите аудиофайлы для импорта", "Selecciona archivos de audio para importar" },
        { Keys::kChooseImportMode, "Choose Import Mode", "选择导入模式", "インポートモードを選択", "Выберите режим импорта", "Elegir modo de importación" },
        { Keys::kSelectedAudioFilesImportMode, "You selected {0} audio files. Choose an import mode.", "你选择了 {0} 个音频文件。请选择导入模式。", "{0}個のオーディオファイルを選択しました。インポートモードを選択してください。", "Вы выбрали аудиофайлов: {0}. Выберите режим импорта.", "Has seleccionado {0} archivos de audio. Elige un modo de importación." },
        { Keys::kImportSequentiallyToCurrentTrack, "Import Sequentially To Current Track", "按顺序导入到当前轨道", "現在のトラックに順番にインポート", "Импортировать последовательно в текущую дорожку", "Importar secuencialmente en la pista actual" },
        { Keys::kImportToSeparateTracks, "Import To Separate Tracks", "导入到独立轨道", "別々のトラックにインポート", "Импортировать в отдельные дорожки", "Importar en pistas separadas" },
        { Keys::kMaximumTrackCountReached, "Maximum track count reached ({0}). Cannot create more tracks.", "已达到最大轨道数（{0}），无法创建更多轨道。", "最大トラック数（{0}）に達したため、これ以上トラックを作成できません。", "Достигнуто максимальное число дорожек ({0}). Нельзя создать больше дорожек.", "Se alcanzó el máximo de pistas ({0}). No se pueden crear más pistas." },
        { Keys::kExportAudioFile, "Export Audio File", "导出音频文件", "オーディオファイルをエクスポート", "Экспорт аудиофайла", "Exportar archivo de audio" },
        { Keys::kOverwriteExistingFile, "Overwrite Existing File?", "覆盖现有文件？", "既存のファイルを上書きしますか？", "Перезаписать существующий файл?", "¿Sobrescribir el archivo existente?" },
        { Keys::kOverwriteExistingFileMessage, "The target file already exists. Overwrite it?", "目标文件已存在。要覆盖它吗？", "対象ファイルは既に存在します。上書きしますか？", "Целевой файл уже существует. Перезаписать его?", "El archivo de destino ya existe. ¿Sobrescribirlo?" },
        { Keys::kOverwriteExistingProject, "Overwrite Existing Project?", "覆盖现有工程？", "既存のプロジェクトを上書きしますか？", "Перезаписать существующий проект?", "¿Sobrescribir el proyecto existente?" },
        { Keys::kOverwriteExistingProjectMessage, "The target project file already exists. Overwrite it?", "目标工程文件已存在。要覆盖它吗？", "対象プロジェクトファイルは既に存在します。上書きしますか？", "Целевой файл проекта уже существует. Перезаписать его?", "El archivo de proyecto de destino ya existe. ¿Sobrescribirlo?" },
        { Keys::kProjectRootNodeInvalid, "Root node is not {0}", "根节点不是 {0}", "ルートノードが{0}ではありません", "Корневой узел не является {0}", "El nodo raíz no es {0}" },
        { Keys::kUnsupportedProjectFormat, "Unsupported project format version: {0} (supported {1}-{2})", "不支持的工程格式版本：{0}（支持 {1}-{2}）", "サポートされていないプロジェクト形式のバージョンです：{0}（サポート範囲 {1}-{2}）", "Неподдерживаемая версия формата проекта: {0} (поддерживается {1}-{2})", "Versión de formato de proyecto no compatible: {0} (compatible con {1}-{2})" },
        { Keys::kProjectInvalidDynamicEq, "Project contains invalid dynamic EqSettings", "工程包含无效的动态 EqSettings", "プロジェクトに無効な動的EqSettingsが含まれています", "Проект содержит недопустимые динамические EqSettings", "El proyecto contiene EqSettings dinámicos no válidos" },
        { Keys::kProjectMissingSettings, "Project is missing required ProjectSettings node", "工程缺少必需的 ProjectSettings 节点", "プロジェクトに必須のProjectSettingsノードがありません", "В проекте отсутствует обязательный узел ProjectSettings", "Falta el nodo ProjectSettings obligatorio del proyecto" },
        { Keys::kProjectSettingsMissingTimeSignature, "ProjectSettings is missing required timeSignatureNumerator or timeSignatureDenominator", "ProjectSettings 缺少必需的 timeSignatureNumerator 或 timeSignatureDenominator", "ProjectSettingsに必須のtimeSignatureNumeratorまたはtimeSignatureDenominatorがありません", "В ProjectSettings отсутствует обязательный timeSignatureNumerator или timeSignatureDenominator", "Falta timeSignatureNumerator o timeSignatureDenominator obligatorio en ProjectSettings" },
        { Keys::kProjectFileNotFound, "Project file not found: {0}", "找不到工程文件：{0}", "プロジェクトファイルが見つかりません：{0}", "Файл проекта не найден: {0}", "No se encontró el archivo de proyecto: {0}" },
        { Keys::kProjectParseFailed, "Failed to parse project file: {0} — {1}", "解析工程文件失败：{0} — {1}", "プロジェクトファイルの解析に失敗しました：{0} — {1}", "Не удалось разобрать файл проекта: {0} — {1}", "No se pudo analizar el archivo de proyecto: {0} — {1}" },
        { Keys::kProjectInvalidXml, "Invalid XML structure in: {0}", "文件中的 XML 结构无效：{0}", "次のファイルのXML構造が無効です：{0}", "Недопустимая структура XML в файле: {0}", "Estructura XML no válida en: {0}" },
        { Keys::kProjectCoreStoresUnavailable, "Cannot commit project: core stores unavailable", "无法提交工程：核心存储不可用", "プロジェクトを確定できません：コアストアを利用できません", "Нельзя применить проект: основные хранилища недоступны", "No se puede confirmar el proyecto: los almacenes principales no están disponibles" },
        { Keys::kProjectWriteFailed, "Failed to write project file: {0}", "写入工程文件失败：{0}", "プロジェクトファイルの書き込みに失敗しました：{0}", "Не удалось записать файл проекта: {0}", "No se pudo escribir el archivo de proyecto: {0}" },
        { Keys::kProjectNoMediaDirectory, "Cannot copy media: no media directory", "无法复制媒体：没有媒体目录", "メディアをコピーできません：メディアディレクトリがありません", "Нельзя скопировать медиа: каталог медиа отсутствует", "No se puede copiar el contenido multimedia: no hay directorio multimedia" },
        { Keys::kProjectMediaDirectoryCreateFailed, "Failed to create media directory: {0}", "创建媒体目录失败：{0}", "メディアディレクトリの作成に失敗しました：{0}", "Не удалось создать каталог медиа: {0}", "No se pudo crear el directorio multimedia: {0}" },
        { Keys::kProjectSourceFileNotFoundForCopy, "Source file not found for copy: {0}", "找不到要复制的源文件：{0}", "コピーするソースファイルが見つかりません：{0}", "Исходный файл для копирования не найден: {0}", "No se encontró el archivo de origen para copiar: {0}" },
        { Keys::kProjectMediaCopyFailed, "Failed to copy media file: {0} -> {1}", "复制媒体文件失败：{0} -> {1}", "メディアファイルのコピーに失敗しました：{0} -> {1}", "Не удалось скопировать медиафайл: {0} -> {1}", "No se pudo copiar el archivo multimedia: {0} -> {1}" },
        { Keys::kImportFailed, "Import Failed", "导入失败", "インポートに失敗しました", "Ошибка импорта", "Error de importación" },
        { "Export Complete", "Export Complete", "导出完成", "エクスポート完了", "Экспорт завершён", "Exportación completada" },
        { " has been exported to: ", " has been exported to: ", " 已导出到: ", " のエクスポート先：", " экспортирован в: ", " se ha exportado a: " },
        { "Could not export audio to ", "Could not export audio to ", "无法导出音频到 ", "オーディオを次の場所にエクスポートできませんでした：", "Не удалось экспортировать аудио в ", "No se pudo exportar el audio a " },
        { "\nReason: ", "\nReason: ", "\n原因: ", "\n原因：", "\nПричина: ", "\nMotivo: " },
        { Keys::kExportFailed, "Export Failed", "导出失败", "エクスポートに失敗しました", "Ошибка экспорта", "Error de exportación" },
        { "Overwrite", "Overwrite", "覆盖", "上書き", "Перезаписать", "Sobrescribir" },
        { "Current project has unsaved changes", "Current project has unsaved changes", "当前工程尚未保存", "現在のプロジェクトに未保存の変更があります", "В текущем проекте есть несохранённые изменения", "El proyecto actual tiene cambios sin guardar" },
        { "Save changes to the current project before opening another?", "Save changes to the current project before opening another?", "打开其他工程前，是否保存当前工程的更改？", "別のプロジェクトを開く前に、現在の変更を保存しますか？", "Сохранить изменения текущего проекта перед открытием другого?", "¿Guardar los cambios del proyecto actual antes de abrir otro?" },
        { "Save", "Save", "保存", "保存", "Сохранить", "Guardar" },
        { "Do Not Save", "Do Not Save", "不保存", "保存しない", "Не сохранять", "No guardar" },
        { "Don't use reference Clip", "Don't use reference Clip", "不使用参考Clip", "リファレンスClipを使用しない", "Не использовать референсный Clip", "No usar el Clip de referencia" },
        { "Reference Clip", "Reference Clip", "参考 Clip", "リファレンスClip", "Референсный Clip", "Clip de referencia" },
        { "Unable to clear the current reference Clip binding.", "Unable to clear the current reference Clip binding.", "无法清除当前参考 Clip 绑定。", "現在のリファレンスClipの割り当てを解除できません。", "Не удалось снять текущую привязку референсного Clip.", "No se pudo quitar el vínculo actual del Clip de referencia." },
        { "That Clip no longer meets the reference binding requirements.", "That Clip no longer meets the reference binding requirements.", "该 Clip 已不满足参考绑定条件。", "そのClipはリファレンスとして割り当てる条件を満たさなくなりました。", "Этот Clip больше не соответствует требованиям для привязки в качестве референса.", "Ese Clip ya no cumple los requisitos para vincularlo como referencia." },
        { "Select reference Clip", "Select reference Clip", "选择参考Clip", "リファレンスClipを選択", "Выбрать референсный Clip", "Seleccionar Clip de referencia" },
        { "(No available reference Clips)", "(No available reference Clips)", "（无可用的参考Clip）", "（利用可能なリファレンスClipはありません）", "(Нет доступных референсных Clip)", "(No hay Clips de referencia disponibles)" },
    };
    
    for (const auto& t : translations)
    {
        if (strcmp(t.key, key) == 0)
        {
            switch (lang)
            {
                case Language::English:  return juce::String::fromUTF8(t.en);
                case Language::Chinese:  return juce::String::fromUTF8(t.zh);
                case Language::Japanese: return juce::String::fromUTF8(t.ja);
                case Language::Russian:  return juce::String::fromUTF8(t.ru);
                case Language::Spanish:  return juce::String::fromUTF8(t.es);
                default: return juce::String::fromUTF8(t.en);
            }
        }
    }
    
    return juce::String::fromUTF8(key);
}

inline juce::String get(const char* key)
{
    return get(LocalizationManager::getInstance().resolveLanguage(), key);
}

inline juce::String format(const juce::String& pattern, const juce::String& arg0)
{
    return pattern.replace("{0}", arg0);
}

inline juce::String format(const juce::String& pattern, const juce::String& arg0, const juce::String& arg1)
{
    return pattern.replace("{0}", arg0).replace("{1}", arg1);
}

inline juce::String format(const juce::String& pattern,
                           const juce::String& arg0,
                           const juce::String& arg1,
                           const juce::String& arg2)
{
    return pattern.replace("{0}", arg0).replace("{1}", arg1).replace("{2}", arg2);
}

}

#define LOC(key) OpenTune::Loc::get(OpenTune::Loc::Keys::key)
#define LOC_KEY(key) OpenTune::Loc::get(key)
#define LOC_RAW(key) OpenTune::Loc::get(OpenTune::LocalizationManager::getInstance().resolveLanguage(), key)

}
