#include "MenuBarComponent.h"
#include "../PluginProcessor.h"
#include "UIColors.h"

namespace OpenTune {

MenuBarComponent::MenuBarComponent(OpenTuneAudioProcessor& processor)
    : processor_(processor)
{
    menuBar_.setModel(this);
    addAndMakeVisible(menuBar_);
}

MenuBarComponent::~MenuBarComponent()
{
    menuBar_.setModel(nullptr);
}

void MenuBarComponent::paint(juce::Graphics& g)
{
    juce::ignoreUnused(g);
}

void MenuBarComponent::resized()
{
    menuBar_.setBounds(getLocalBounds());
}

void MenuBarComponent::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void MenuBarComponent::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

juce::StringArray MenuBarComponent::getMenuBarNames()
{
    return { "File", "Edit", "View" };
}

juce::PopupMenu MenuBarComponent::getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName)
{
    juce::ignoreUnused(topLevelMenuIndex);
    juce::PopupMenu menu;

    if (menuName == "File")
    {
        // 直接添加"导入音频"菜单项，不再使用二级菜单
        // 导入到哪个轨道由当前选中的轨道决定，支持多选
        menu.addItem(ImportAudio, "Import Audio...");

        // 导出菜单
        juce::PopupMenu exportMenu;
        exportMenu.addItem(ExportSelectedClip, "Export Selected Clip");   // 导出选中的Clip
        exportMenu.addItem(ExportTrack, "Export Track");                  // 导出当前轨道
        exportMenu.addItem(ExportBus, "Export Bus (Master Mix)");         // 导出总线混音
        menu.addSubMenu("Export Audio", exportMenu);

        menu.addSeparator();
        menu.addItem(SavePreset, "Save Preset...");
        menu.addItem(LoadPreset, "Load Preset...");

        menu.addSeparator();
        menu.addItem(OpenPreferences, "Options");
    }
    else if (menuName == "Edit")
    {
        menu.addItem(EditUndo, "Undo  (Ctrl+Z)", true);
        menu.addItem(EditRedo, "Redo  (Ctrl+Shift+Z)", true);
    }
    else if (menuName == "View")
    {
        menu.addItem(ShowWaveform, "Show Waveform", true, processor_.getShowWaveform());
        menu.addItem(ShowLanes, "Show Lanes", true, processor_.getShowLanes());

        juce::PopupMenu themeMenu;
        themeMenu.addItem(ThemeBlueBreeze, "Blue Breeze", true, Theme::getActiveTheme() == ThemeId::BlueBreeze);
        themeMenu.addItem(ThemeDarkBlueGrey, "Dark Blue-Grey", true, Theme::getActiveTheme() == ThemeId::DarkBlueGrey);
        themeMenu.addItem(ThemeAurora, "Aurora Glass", true, Theme::getActiveTheme() == ThemeId::Aurora);
        menu.addSeparator();
        menu.addSubMenu("Theme", themeMenu);
        
        auto currentTrailTheme = MouseTrailConfig::getTheme();
        juce::PopupMenu mouseTrailMenu;
        mouseTrailMenu.addItem(MouseTrailNone, "Off", true, currentTrailTheme == MouseTrailConfig::TrailTheme::None);
        mouseTrailMenu.addSeparator();
        mouseTrailMenu.addItem(MouseTrailClassic, "Classic", true, currentTrailTheme == MouseTrailConfig::TrailTheme::Classic);
        mouseTrailMenu.addItem(MouseTrailNeon, "Neon", true, currentTrailTheme == MouseTrailConfig::TrailTheme::Neon);
        mouseTrailMenu.addItem(MouseTrailFire, "Fire", true, currentTrailTheme == MouseTrailConfig::TrailTheme::Fire);
        mouseTrailMenu.addItem(MouseTrailOcean, "Ocean", true, currentTrailTheme == MouseTrailConfig::TrailTheme::Ocean);
        mouseTrailMenu.addItem(MouseTrailGalaxy, "Galaxy", true, currentTrailTheme == MouseTrailConfig::TrailTheme::Galaxy);
        mouseTrailMenu.addItem(MouseTrailCherryBlossom, "Cherry Blossom", true, currentTrailTheme == MouseTrailConfig::TrailTheme::CherryBlossom);
        mouseTrailMenu.addItem(MouseTrailMatrix, "Matrix", true, currentTrailTheme == MouseTrailConfig::TrailTheme::Matrix);
        menu.addSubMenu("Mouse Trail", mouseTrailMenu);
    }

    return menu;
}

void MenuBarComponent::menuItemSelected(int menuItemID, int topLevelMenuIndex)
{
    juce::ignoreUnused(topLevelMenuIndex);
    DBG("MenuBarComponent::menuItemSelected called with ID: " + juce::String(menuItemID));

    switch (menuItemID)
    {
        case ImportAudio:
            // 触发导入音频，由PluginEditor处理文件选择、多选支持和导入模式询问
            DBG("ImportAudio selected, calling listeners");
            listeners_.call([](Listener& l) { l.importAudioRequested(); });
            break;

        // 导出选项 - 使用ExportType枚举
        case ExportSelectedClip:
            listeners_.call([](Listener& l) { l.exportAudioRequested(ExportType::SelectedClip); });
            break;
        case ExportTrack:
            listeners_.call([](Listener& l) { l.exportAudioRequested(ExportType::Track); });
            break;
        case ExportBus:
            listeners_.call([](Listener& l) { l.exportAudioRequested(ExportType::Bus); });
            break;

        case SavePreset:
            listeners_.call([](Listener& l) { l.savePresetRequested(); });
            break;
        case LoadPreset:
            listeners_.call([](Listener& l) { l.loadPresetRequested(); });
            break;

        case OpenPreferences:
            listeners_.call([](Listener& l) { l.preferencesRequested(); });
            break;

        case ShowWaveform:
        {
            bool newState = !processor_.getShowWaveform();
            processor_.setShowWaveform(newState);
            listeners_.call([newState](Listener& l) { l.showWaveformToggled(newState); });
            break;
        }
        case ShowLanes:
        {
            bool newState = !processor_.getShowLanes();
            processor_.setShowLanes(newState);
            listeners_.call([newState](Listener& l) { l.showLanesToggled(newState); });
            break;
        }
        case ThemeBlueBreeze:
            listeners_.call([](Listener& l) { l.themeChanged(ThemeId::BlueBreeze); });
            break;
        case ThemeDarkBlueGrey:
            listeners_.call([](Listener& l) { l.themeChanged(ThemeId::DarkBlueGrey); });
            break;
        case ThemeAurora:
            listeners_.call([](Listener& l) { l.themeChanged(ThemeId::Aurora); });
            break;
            
        case MouseTrailNone:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::None); });
            break;
        case MouseTrailClassic:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Classic); });
            break;
        case MouseTrailNeon:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Neon); });
            break;
        case MouseTrailFire:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Fire); });
            break;
        case MouseTrailOcean:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Ocean); });
            break;
        case MouseTrailGalaxy:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Galaxy); });
            break;
        case MouseTrailCherryBlossom:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::CherryBlossom); });
            break;
        case MouseTrailMatrix:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Matrix); });
            break;

        case EditUndo:
            listeners_.call([](Listener& l) { l.undoRequested(); });
            break;

        case EditRedo:
            listeners_.call([](Listener& l) { l.redoRequested(); });
            break;

        default:
            break;
    }
}

} // namespace OpenTune
