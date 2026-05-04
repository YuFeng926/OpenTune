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
constexpr const char* kSavePreset = "Save Preset...";
constexpr const char* kLoadPreset = "Load Preset...";
constexpr const char* kOptions = "Options";

constexpr const char* kUndo = "Undo";
constexpr const char* kRedo = "Redo";

constexpr const char* kShowWaveform = "Show Waveform";
constexpr const char* kShowLanes = "Show Lanes";
constexpr const char* kNoteLabels = "Note Labels";
constexpr const char* kNoteLabelsShowAll = "Show All";
constexpr const char* kNoteLabelsCOnly = "C Only";
constexpr const char* kNoteLabelsHide = "Hide";
constexpr const char* kShowChunkBoundaries = "Show Chunk Boundaries";
constexpr const char* kShowUnvoicedFrames = "Show Unvoiced Frames";
constexpr const char* kTheme = "Theme";
constexpr const char* kThemeBlueBreeze = "Blue Breeze";
constexpr const char* kThemeDarkBlueGrey = "Dark Blue-Grey";
constexpr const char* kThemeAurora = "Aurora Glass";
constexpr const char* kMouseTrail = "Mouse Trail";
constexpr const char* kOff = "Off";
constexpr const char* kClassic = "Classic";
constexpr const char* kNeon = "Neon";
constexpr const char* kFire = "Fire";
constexpr const char* kOcean = "Ocean";
constexpr const char* kGalaxy = "Galaxy";
constexpr const char* kCherryBlossom = "Cherry Blossom";
constexpr const char* kMatrix = "Matrix";

constexpr const char* kAudio = "Audio";
constexpr const char* kEditing = "Editing";
constexpr const char* kMouse = "Mouse";
constexpr const char* kKeyswitch = "Keyswitch";
constexpr const char* kLanguage = "Language";
constexpr const char* kLanguageLabel = "Interface Language";
constexpr const char* kAudioEditingScheme = "Audio Editing Scheme";
constexpr const char* kCorrectedF0First = "Corrected F0 First";
constexpr const char* kNotesFirst = "Notes First";

constexpr const char* kHorizontalZoomSensitivity = "Horizontal Zoom Sensitivity";
constexpr const char* kVerticalZoomSensitivity = "Vertical Zoom Sensitivity";
constexpr const char* kScrollSpeed = "Scroll Speed";
constexpr const char* kResetToDefaults = "Reset to Defaults";
constexpr const char* kRenderingPriority = "Rendering Priority";
constexpr const char* kGpuFirst = "GPU First";
constexpr const char* kCpuFirst = "CPU First";

constexpr const char* kSetShortcut = "Set Shortcut";
constexpr const char* kPressNewKeyCombination = "Press the new key combination";
constexpr const char* kCurrent = "Current";
constexpr const char* kCancel = "Cancel";
constexpr const char* kShortcutConflict = "Shortcut Conflict";
constexpr const char* kShortcutConflictMessage = "This shortcut is already assigned to \"{0}\".\n\nDo you want to reassign it?";
constexpr const char* kYes = "Yes";
constexpr const char* kNo = "No";
constexpr const char* kResetAllToDefaults = "Reset All to Defaults";

constexpr const char* kPlayPause = "Play/Pause";
constexpr const char* kStop = "Stop";
constexpr const char* kPlayFromStart = "Play from Start";
constexpr const char* kCut = "Cut";
constexpr const char* kCopy = "Copy";
constexpr const char* kPaste = "Paste";
constexpr const char* kSelectAll = "Select All";
constexpr const char* kDelete = "Delete";

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

        constexpr const char* kClose = "Close";
        constexpr const char* kHelp = "Help...";

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
constexpr const char* kTooltipTrackPanel = "Track Panel";
constexpr const char* kTooltipParameterPanel = "Parameter Panel";
constexpr const char* kTooltipBpm = "Tempo (BPM)";
constexpr const char* kTooltipTimeline = "Playback Time";
constexpr const char* kTooltipScrollMode = "Scroll Mode";
constexpr const char* kTooltipTimeUnit = "Time Unit";
constexpr const char* kTooltipRetuneSpeed = "Retune Speed - Controls how fast pitch is corrected";
constexpr const char* kTooltipVibratoDepth = "Vibrato Depth - Controls vibrato amplitude";
constexpr const char* kTooltipVibratoRate = "Vibrato Rate - Controls vibrato speed";
constexpr const char* kTooltipNoteSplit = "Note Split - Threshold for splitting notes";

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
        { Keys::kSavePreset, "Save Preset...", "保存预设...", "プリセットを保存...", "Сохранить...", "Guardar preset..." },
        { Keys::kLoadPreset, "Load Preset...", "加载预设...", "プリセットを読み込む...", "Загрузить...", "Cargar preset..." },
        { Keys::kOptions, "Options", "选项", "オプション", "Настройки", "Opciones" },
        
        { Keys::kUndo, "Undo", "撤销", "元に戻す", "Отменить", "Deshacer" },
        { Keys::kRedo, "Redo", "重做", "やり直す", "Повтор", "Rehacer" },
        
        { Keys::kShowWaveform, "Show Waveform", "显示波形", "波形を表示", "Волновая форма", "Ver forma de onda" },
        { Keys::kShowLanes, "Show Lanes", "显示琴键", "レーンを表示", "Дорожки", "Ver carriles" },
        { Keys::kNoteLabels, "Note Labels", "音名标签", "音名ラベル", "Названия нот", "Etiquetas de notas" },
        { Keys::kNoteLabelsShowAll, "Show All", "全部显示", "全表示", "Показывать все", "Mostrar todo" },
        { Keys::kNoteLabelsCOnly, "C Only", "仅 C", "C のみ", "Только C", "Solo C" },
        { Keys::kNoteLabelsHide, "Hide", "隐藏", "非表示", "Скрыть", "Ocultar" },
        { Keys::kShowChunkBoundaries, "Show Chunk Boundaries", "显示分块边界", "チャンク境界を表示", "Показывать границы чанков", "Mostrar limites de bloques" },
        { Keys::kShowUnvoicedFrames, "Show Unvoiced Frames", "显示无声音帧", "無声音フレームを表示", "Показывать глухие кадры", "Mostrar cuadros sordos" },
        { Keys::kTheme, "Theme", "主题", "テーマ", "Тема", "Tema" },
        { Keys::kThemeBlueBreeze, "Blue Breeze", "蓝色清风", "ブルーブリーズ", "Голубой бриз", "Brisa azul" },
        { Keys::kThemeDarkBlueGrey, "Dark Blue-Grey", "深蓝灰", "ダークブルーグレー", "Тёмно-синий серый", "Azul-gris oscuro" },
        { Keys::kThemeAurora, "Aurora Glass", "极光玻璃", "オーロラグラス", "Аврора", "Aurora cristal" },
        { Keys::kMouseTrail, "Mouse Trail", "鼠标轨迹", "マウストレイル", "След мыши", "Ratón" },
        { Keys::kOff, "Off", "关闭", "オフ", "Выкл", "Apagado" },
        { Keys::kClassic, "Classic", "经典", "クラシック", "Классика", "Clásico" },
        { Keys::kNeon, "Neon", "霓虹", "ネオン", "Неон", "Neón" },
        { Keys::kFire, "Fire", "火焰", "ファイア", "Огонь", "Fuego" },
        { Keys::kOcean, "Ocean", "海洋", "オーシャン", "Океан", "Océano" },
        { Keys::kGalaxy, "Galaxy", "星河", "ギャラクシー", "Галактика", "Galaxia" },
        { Keys::kCherryBlossom, "Cherry Blossom", "樱花", "桜", "Сакура", "Flor de cerezo" },
        { Keys::kMatrix, "Matrix", "矩阵", "マトリックス", "Матрица", "Matriz" },
        
        { Keys::kAudio, "Audio", "音频", "オーディオ", "Аудио", "Audio" },
        { Keys::kEditing, "Editing", "编辑", "編集", "Редактирование", "Edicion" },
        { Keys::kMouse, "Mouse", "鼠标", "マウス", "Мышь", "Ratón" },
        { Keys::kKeyswitch, "Keyswitch", "快捷键", "キースイッチ", "Клавиши", "Atajos" },
        { Keys::kLanguage, "Language", "语言", "言語", "Язык", "Idioma" },
        { Keys::kLanguageLabel, "Interface Language", "界面语言", "インターフェース言語", "Язык", "Idioma" },
        { Keys::kAudioEditingScheme, "Audio Editing Scheme", "音频编辑方案", "音声編集方式", "Схема аудиоредактирования", "Esquema de edicion de audio" },
        { Keys::kCorrectedF0First, "曲线优先编辑", "曲线优先编辑", "曲线优先编辑", "曲线优先编辑", "曲线优先编辑" },
        { Keys::kNotesFirst, "音符优先编辑", "音符优先编辑", "音符优先编辑", "音符优先编辑", "音符优先编辑" },
        
        { Keys::kHorizontalZoomSensitivity, "Horizontal Zoom Sensitivity", "水平缩放灵敏度", "水平ズーム感度", "Чувств. гориз. zoom", "Sensibilidad zoom horizontal" },
        { Keys::kVerticalZoomSensitivity, "Vertical Zoom Sensitivity", "垂直缩放灵敏度", "垂直ズーム感度", "Чувств. верт. zoom", "Sensibilidad zoom vertical" },
        { Keys::kScrollSpeed, "Scroll Speed", "滚动速度", "スクロール速度", "Скорость прокрутки", "Velocidad" },
        { Keys::kResetToDefaults, "Reset to Defaults", "恢复默认设置", "デフォルトに戻す", "Сбросить", "Restablecer" },
        { Keys::kRenderingPriority, "Rendering Priority", "渲染优先级", "レンダリング優先度", "Приоритет рендеринга", "Prioridad de renderizado" },
        { Keys::kGpuFirst, "GPU First", "GPU 优先", "GPU 優先", "GPU приоритет", "GPU primero" },
        { Keys::kCpuFirst, "CPU First", "CPU 优先", "CPU 優先", "CPU приоритет", "CPU primero" },
        
        { Keys::kSetShortcut, "Set Shortcut", "设置快捷键", "ショートカットを設定", "Назначить сочетание", "Atajo" },
        { Keys::kPressNewKeyCombination, "Press the new key combination", "按下新的组合键", "新しいキーの組み合わせを押してください", "Нажмите сочетание", "Pulse combinación" },
        { Keys::kCurrent, "Current", "当前", "現在", "Текущий", "Actual" },
        { Keys::kCancel, "Cancel", "取消", "キャンセル", "Отмена", "Cancelar" },
        { Keys::kShortcutConflict, "Shortcut Conflict", "快捷键冲突", "ショートカットの競合", "Конфликт сочетаний", "Conflicto de atajo" },
        { Keys::kShortcutConflictMessage, "This shortcut is already assigned to \"{0}\".\n\nDo you want to reassign it?", "此快捷键已分配给\"{0}\"。\n\n是否重新分配？", "このショートカットは既に「{0}」に割り当てられています。\n\n再割り当てしますか？", "Это сочетание уже назначено для \"{0}\".\n\nПереназначить?", "Este atajo ya está asignado a \"{0}\".\n\n¿Reasignar?" },
        { Keys::kYes, "Yes", "是", "はい", "Да", "Sí" },
        { Keys::kNo, "No", "否", "いいえ", "Нет", "No" },
        { Keys::kResetAllToDefaults, "Reset All to Defaults", "全部恢复默认", "すべてデフォルトに戻す", "Сбросить все", "Restablecer todo" },
        
        { Keys::kPlayPause, "Play/Pause", "播放/暂停", "再生/一時停止", "Старт/Пауза", "Play/Pausa" },
        { Keys::kStop, "Stop", "停止", "停止", "Стоп", "Detener" },
        { Keys::kPlayFromStart, "Play from Start", "从头播放", "最初から再生", "Играть сначала", "Reprod. inicio" },
        { Keys::kCut, "Cut", "剪切", "切り取り", "Вырезать", "Cortar" },
        { Keys::kCopy, "Copy", "复制", "コピー", "Копия", "Copiar" },
        { Keys::kPaste, "Paste", "粘贴", "貼り付け", "Вставить", "Pegar" },
        { Keys::kSelectAll, "Select All", "全选", "すべて選択", "Выбрать всё", "Selec. todo" },
        { Keys::kDelete, "Delete", "删除", "削除", "Удалить", "Eliminar" },
        
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
        
        { Keys::kClose, "Close", "关闭", "閉じる", "Закрыть", "Cerrar" },
        { Keys::kHelp, "Help...", "帮助...", "ヘルプ...", "Справка...", "Ayuda..." },
        
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
        { Keys::kTooltipTrackPanel, "Track Panel", "轨道面板", "トラックパネル", "Панель дорожек", "Panel de pistas" },
        { Keys::kTooltipParameterPanel, "Parameter Panel", "参数面板", "パラメータパネル", "Панель параметров", "Panel de parámetros" },
        { Keys::kTooltipBpm, "Tempo (BPM)", "节拍速度 (BPM)", "テンポ (BPM)", "Темп (BPM)", "Tempo (BPM)" },
        { Keys::kTooltipTimeline, "Playback Time", "播放时间", "再生時間", "Время воспроизведения", "Tiempo de reproducción" },
        { Keys::kTooltipScrollMode, "Scroll Mode - Toggle between Continuous and Page scroll", "滚动模式 - 切换连续/翻页滚动", "スクロールモード - 連続/ページ切替", "Режим прокрутки - непрерывная/постраничная", "Modo desplazamiento - Continuo/Página" },
        { Keys::kTooltipTimeUnit, "Time Unit - Toggle between Seconds and Bars", "时间单位 - 切换秒/小节显示", "時間単位 - 秒/小節を切替", "Единица времени - секунды/такты", "Unidad de tiempo - Segundos/Compases" },
        { Keys::kTooltipRetuneSpeed, "Retune Speed - Controls how fast pitch is corrected", "校正速度 - 控制音高校正的速度", "チューン速度 - ピッチ補正の速度を制御", "Скорость коррекции - насколько быстро корректируется тон", "Vel. afinación - Controla la rapidez de corrección" },
        { Keys::kTooltipVibratoDepth, "Vibrato Depth - Controls vibrato amplitude", "颤音深度 - 控制颤音幅度", "ビブラート深さ - ビブラートの振幅を制御", "Глубина вибрато - амплитуда вибрато", "Prof. vibrato - Controla la amplitud" },
        { Keys::kTooltipVibratoRate, "Vibrato Rate - Controls vibrato speed", "颤音速率 - 控制颤音频率", "ビブラート速度 - ビブラートの速さを制御", "Скорость вибрато - частота вибрато", "Tasa vibrato - Controla la velocidad" },
        { Keys::kTooltipNoteSplit, "Note Split - Threshold for splitting notes", "音符分割 - 控制音符分割阈值", "ノート分割 - ノート分割の閾値を制御", "Разделение нот - порог разделения", "Div. notas - Umbral de división" },
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
