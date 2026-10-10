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
        case Language::Spanish:  return juce::String::fromUTF8("Espa\xcf\x81ol");  // Español
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
constexpr const char* kKeyswitch = "Keyswitch";
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
constexpr const char* kToolODSelect = "OD: Select";
constexpr const char* kToolODPitch = "OD: Pitch";
constexpr const char* kToolODPitchModulation = "OD: Pitch Modulation";
constexpr const char* kToolODPitchDrift = "OD: Pitch Drift";
constexpr const char* kToolODVolumeEnvelope = "OD: Volume Envelope";
constexpr const char* kToolODScissors = "OD: Scissors";
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
        { Keys::kKeyswitch, "Keyswitch", "快捷键", "キースイッチ", "Клавиши", "Atajos" },
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
        { Keys::kToolODSelect, "OD: Select", "OD: 选择", "OD: 選択", "OD: выбор", "OD: seleccionar" },
        { Keys::kToolODPitch, "OD: Pitch", "OD: 音高", "OD: ピッチ", "OD: тон", "OD: tono" },
        { Keys::kToolODPitchModulation, "OD: Pitch Modulation", "OD: 音高调制", "OD: ピッチ変調", "OD: модуляция тона", "OD: modulación tono" },
        { Keys::kToolODPitchDrift, "OD: Pitch Drift", "OD: 音高漂移", "OD: ピッチドリフト", "OD: дрифт тона", "OD: deriva tono" },
        { Keys::kToolODVolumeEnvelope, "OD: Volume Envelope", "OD: 音量包络", "OD: ボリュームエンベロープ", "OD: огибающая громкости", "OD: envol. volumen" },
        { Keys::kToolODScissors, "OD: Scissors", "OD: 剪刀", "OD: ハサミ", "OD: ножницы", "OD: tijeras" },
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
        { Keys::kProps, "Props", "属性", "プロパティ", "Свойства", "Props" },
        { Keys::kScale, "Scale", "调式", "スケール", "Гамма", "Escala" },
        { Keys::kRootNote, "Root", "根音", "ルート", "Тоника", "Raíz" },
        
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
        { "Vibrato Rate", "Vibrato Rate", "颤音速率", "速度 вибрато", "Скорость вибрато", "Velocidad del vibrato" },
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
        { "Vibrato Rate controls how quickly vibrato cycles.", "Vibrato Rate controls how quickly vibrato cycles.", "“颤音速率”控制颤音循环的速度。", "「速度 вибрато」はビブラートの周期の速さを制御します。", "«Скорость вибрато» управляет скоростью циклов вибрато.", "La velocidad del vibrato controla la rapidez de sus ciclos." },
        { "AUTO applies the current note correction settings.", "AUTO applies the current note correction settings.", "“自动”应用当前音符校正设置。", "「AUTO」は現在のノート補正設定を適用します。", "«АВТО» применяет текущие настройки коррекции нот.", "AUTO aplica los ajustes actuales de corrección de notas." },
        { "SNAP constrains blob edits to the selected grid.", "SNAP constrains blob edits to the selected grid.", "“吸附”将波形块编辑限制到所选网格。", "「SNAP」はブロブの編集を選択したグリッドに制限します。", "«ПРИВЯЗКА» ограничивает редактирование блоков выбранной сеткой.", "AJUSTE limita la edición de bloques a la cuadrícula seleccionada." },
        { "Pitch Grid chooses how OpenDyne pitch edits snap.", "Pitch Grid chooses how OpenDyne pitch edits snap.", "“音高网格”选择 OpenDyne 音高编辑的吸附方式。", "「ピッチグリッド」はOpenDyneのピッチ編集のスナップ方法を選択します。", "«Сетка высоты тона» задаёт привязку при редактировании высоты тона в OpenDyne.", "Cuadrícula de tono elige cómo se ajustan las ediciones de tono de OpenDyne." },
        { "Transport shows the current root and scale; More contains additional scale choices.", "Scale shows the current root and scale; click the dropdown for more scale options.", "调式显示当前的根音和音阶，点击下拉查看更多调式选项。", "スケールには現在のルートとスケールが表示されます。ドロップダウンをクリックすると、さらに多くのスケール候補が表示されます。", "Гамма показывает текущую тонику и гамму; нажмите на раскрывающийся список, чтобы увидеть другие варианты гамм.", "La escala muestra la raíz y la escala actuales; haz clic en el desplegable para ver más opciones de escala." },
        { "Overview moves through the whole clip while keeping the current zoom.", "Overview moves through the whole clip while keeping the current zoom.", "“概览”在保持当前缩放的同时浏览整个片段。", "「概要」は現在のズームを保ったままクリップ全体を移動します。", "«Обзор» перемещается по всему клипу, сохраняя текущий масштаб.", "Vista general recorre todo el clip manteniendo el zoom actual." },
        { "Play, pause, stop and loop here.", "Play, pause, stop and loop here.", "在此播放、暂停、停止和循环。", "ここで再生、一時停止、停止、ループを操作します。", "Здесь можно воспроизводить, ставить на паузу, останавливать и зацикливать.", "Reproduce, pausa, detén y repite aquí." },
        { "Audition with the plugin host transport.", "Audition with the plugin host transport.", "使用插件宿主走带试听。", "プラグインホストのトランスポートで試聴します。", "Прослушивайте с помощью транспорта хоста плагина.", "Escucha con el transporte del host del plugin." },
        { "Options can change the editing mode and enable EQ. Help opens the user guide.", "Options can change the editing mode and enable EQ. Help opens the user guide.", "“选项”可更改编辑模式并启用 EQ。“帮助”可打开用户指南。", "「オプション」では編集モードを変更してEQを有効にできます。「ヘルプ」からユーザーガイドを開けます。", "«Настройки» меняют режим редактирования и включают EQ. «Справка» открывает руководство пользователя.", "Opciones puede cambiar el modo de edición y activar EQ. Ayuda abre la guía del usuario." },
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

}

#define LOC(key) OpenTune::Loc::get(OpenTune::Loc::Keys::key)
#define LOC_KEY(key) OpenTune::Loc::get(key)
#define LOC_RAW(key) OpenTune::Loc::get(OpenTune::LocalizationManager::getInstance().resolveLanguage(), key)

}
