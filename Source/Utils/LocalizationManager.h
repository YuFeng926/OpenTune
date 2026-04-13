#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <functional>

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
    static LocalizationManager& getInstance()
    {
        static LocalizationManager instance;
        return instance;
    }

    void setLanguage(Language lang)
    {
        if (currentLanguage_ != lang)
        {
            currentLanguage_ = lang;
            // 广播给所有监听者
            listeners_.call(&LanguageChangeListener::languageChanged, lang);
        }
    }

    Language getLanguage() const { return currentLanguage_; }

    void addListener(LanguageChangeListener* listener) { listeners_.add(listener); }
    void removeListener(LanguageChangeListener* listener) { listeners_.remove(listener); }

private:
    LocalizationManager() = default;
    Language currentLanguage_ = Language::Chinese;
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
constexpr const char* kNoteNames = "Note Names";
constexpr const char* kShowAllNotes = "Show All Notes";
constexpr const char* kShowCOnly = "Show C Only";
constexpr const char* kHideNoteNames = "Hide Note Names";
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
constexpr const char* kMouse = "Mouse";
constexpr const char* kKeyswitch = "Keyswitch";
constexpr const char* kLanguage = "Language";
constexpr const char* kLanguageLabel = "Interface Language";

constexpr const char* kHorizontalZoomSensitivity = "Horizontal Zoom Sensitivity";
constexpr const char* kVerticalZoomSensitivity = "Vertical Zoom Sensitivity";
constexpr const char* kScrollSpeed = "Scroll Speed";
constexpr const char* kResetToDefaults = "Reset to Defaults";

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
        { Keys::kShowLanes, "Show Lanes", "显示音道", "レーンを表示", "Дорожки", "Ver carriles" },
        { Keys::kNoteNames, "Note Names", "音名", "ノート名", "Названия нот", "Nombres de notas" },
        { Keys::kShowAllNotes, "Show All Notes", "显示全部音名", "すべてのノート名を表示", "Все ноты", "Mostrar todas" },
        { Keys::kShowCOnly, "Show C Only", "仅显示C", "Cのみ表示", "Только C", "Solo C" },
        { Keys::kHideNoteNames, "Hide Note Names", "不显示音名", "ノート名を非表示", "Скрыть", "Ocultar nombres" },
        { Keys::kShowChunkBoundaries, "Show Chunk Boundaries", "显示Chunk边界", "チャンク境界を表示", "Границы чанков", "Límites de chunk" },
        { Keys::kShowUnvoicedFrames, "Show Unvoiced Frames", "显示无声帧", "無声フレームを表示", "Безголосые кадры", "Marcos sin voz" },
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
        { Keys::kMouse, "Mouse", "鼠标", "マウス", "Мышь", "Ratón" },
        { Keys::kKeyswitch, "Keyswitch", "快捷键", "キースイッチ", "Клавиши", "Atajos" },
        { Keys::kLanguage, "Language", "语言", "言語", "Язык", "Idioma" },
        { Keys::kLanguageLabel, "Interface Language", "界面语言", "インターフェース言語", "Язык", "Idioma" },
        
        { Keys::kHorizontalZoomSensitivity, "Horizontal Zoom Sensitivity", "水平缩放灵敏度", "水平ズーム感度", "Чувств. гориз. zoom", "Sensibilidad zoom horizontal" },
        { Keys::kVerticalZoomSensitivity, "Vertical Zoom Sensitivity", "垂直缩放灵敏度", "垂直ズーム感度", "Чувств. верт. zoom", "Sensibilidad zoom vertical" },
        { Keys::kScrollSpeed, "Scroll Speed", "滚动速度", "スクロール速度", "Скорость прокрутки", "Velocidad" },
        { Keys::kResetToDefaults, "Reset to Defaults", "恢复默认设置", "デフォルトに戻す", "Сбросить", "Restablecer" },
        
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
    return get(LocalizationManager::getInstance().getLanguage(), key);
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
#define LOC_RAW(key) OpenTune::Loc::get(OpenTune::LocalizationManager::getInstance().getLanguage(), key)

}
