#pragma once

/**
 * 菜单栏组件
 * 
 * 实现 JUCE MenuBarModel 接口，提供应用程序菜单：
 * - File（导入、导出、预设等）
 * - Edit（撤销、重做等）
 * - View（波形显示、调式、主题等）
 * - 鼠标轨迹特效设置
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include "ThemeTokens.h"
#include "../Utils/MouseTrailConfig.h"

namespace OpenTune {

class OpenTuneAudioProcessor;

class MenuBarComponent : public juce::Component,
                         public juce::MenuBarModel
{
public:
    enum class ExportType
    {
        SelectedClip,
        Track,
        Bus
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void importAudioRequested() = 0;
        virtual void exportAudioRequested(ExportType exportType) = 0;
        virtual void savePresetRequested() = 0;
        virtual void loadPresetRequested() = 0;
        virtual void preferencesRequested() = 0;
        virtual void helpRequested() = 0;
        virtual void showWaveformToggled(bool shouldShow) = 0;
        virtual void showLanesToggled(bool shouldShow) = 0;
        virtual void themeChanged(ThemeId themeId) = 0;
        virtual void undoRequested() = 0;
        virtual void redoRequested() = 0;
        virtual void mouseTrailThemeChanged(MouseTrailConfig::TrailTheme theme) = 0;
        virtual void noteNameModeChanged(int mode) {}
    };

    explicit MenuBarComponent(OpenTuneAudioProcessor& processor);
    ~MenuBarComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    void refreshLocalizedText();  // 刷新本地化文本

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

private:
    OpenTuneAudioProcessor& processor_;
    juce::MenuBarComponent menuBar_;
    juce::ListenerList<Listener> listeners_;
    int currentNoteNameMode_ = 1; // 0=ShowAll, 1=COnly, 2=Hide

    enum MenuItemIDs
    {
        ImportAudio = 1,
        ExportSelectedClip,
        ExportTrack,
        ExportBus,
        SavePreset,
        LoadPreset,

        EditUndo = 50,
        EditRedo,

        ShowWaveform = 100,
        ShowLanes,
        ThemeBlueBreeze,
        ThemeDarkBlueGrey,
        ThemeAurora,

        NoteNamesAll = 110,
        NoteNamesCOnly,
        NoteNamesHide,

        MouseTrailNone = 150,
        MouseTrailClassic,
        MouseTrailNeon,
        MouseTrailFire,
        MouseTrailOcean,
        MouseTrailGalaxy,
        MouseTrailCherryBlossom,
        MouseTrailMatrix,

        OpenPreferences = 200,
        OpenHelp
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MenuBarComponent)
};

} // namespace OpenTune
