#include "MenuBarComponent.h"
#include "../PluginProcessor.h"
#include "UIColors.h"
#include "../../Utils/LocalizationManager.h"
#include "../../Utils/KeyShortcutConfig.h"

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

void MenuBarComponent::refreshLocalizedText()
{
    // JUCE 会自动重新调用 getMenuBarNames() 和 getMenuForIndex()
    menuItemsChanged();
}

juce::StringArray MenuBarComponent::getMenuBarNames()
{
    return { LOC(kFile), LOC(kEdit), LOC(kView) };
}

juce::PopupMenu MenuBarComponent::getMenuForIndex(int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;

    switch (topLevelMenuIndex)
    {
        case 0:  // File
        {
            menu.addItem(ImportAudio, LOC(kImportAudio));

            juce::PopupMenu exportMenu;
            exportMenu.addItem(ExportSelectedClip, LOC(kExportSelectedClip));
            exportMenu.addItem(ExportTrack, LOC(kExportTrack));
            exportMenu.addItem(ExportBus, LOC(kExportBus));
            menu.addSubMenu(LOC(kExportAudio), exportMenu);

            menu.addSeparator();
            menu.addItem(SavePreset, LOC(kSavePreset));
            menu.addItem(LoadPreset, LOC(kLoadPreset));

            menu.addSeparator();
            menu.addItem(OpenPreferences, LOC(kOptions));
            menu.addSeparator();
            menu.addItem(OpenHelp, LOC(kHelp));
            break;
        }
        case 1:  // Edit
        {
            {
                auto undoBinding = KeyShortcutConfig::getShortcutBinding(KeyShortcutConfig::ShortcutId::Undo);
                menu.addItem(EditUndo, LOC(kUndo) + "  " + undoBinding.getDisplayNames(), true);
            }
            {
                auto redoBinding = KeyShortcutConfig::getShortcutBinding(KeyShortcutConfig::ShortcutId::Redo);
                menu.addItem(EditRedo, LOC(kRedo) + "  " + redoBinding.getDisplayNames(), true);
            }
            break;
        }
        case 2:  // View
        {
            menu.addItem(ShowWaveform, LOC(kShowWaveform), true, processor_.getShowWaveform());
            menu.addItem(ShowLanes, LOC(kShowLanes), true, processor_.getShowLanes());

            juce::PopupMenu themeMenu;
            themeMenu.addItem(ThemeBlueBreeze, LOC(kThemeBlueBreeze), true, Theme::getActiveTheme() == ThemeId::BlueBreeze);
            themeMenu.addItem(ThemeDarkBlueGrey, LOC(kThemeDarkBlueGrey), true, Theme::getActiveTheme() == ThemeId::DarkBlueGrey);
            themeMenu.addItem(ThemeAurora, LOC(kThemeAurora), true, Theme::getActiveTheme() == ThemeId::Aurora);
            menu.addSeparator();
            menu.addSubMenu(LOC(kTheme), themeMenu);
            
            auto currentTrailTheme = MouseTrailConfig::getTheme();
            juce::PopupMenu mouseTrailMenu;
            mouseTrailMenu.addItem(MouseTrailNone, LOC(kOff), true, currentTrailTheme == MouseTrailConfig::TrailTheme::None);
            mouseTrailMenu.addSeparator();
            mouseTrailMenu.addItem(MouseTrailClassic, LOC(kClassic), true, currentTrailTheme == MouseTrailConfig::TrailTheme::Classic);
            mouseTrailMenu.addItem(MouseTrailNeon, LOC(kNeon), true, currentTrailTheme == MouseTrailConfig::TrailTheme::Neon);
            mouseTrailMenu.addItem(MouseTrailFire, LOC(kFire), true, currentTrailTheme == MouseTrailConfig::TrailTheme::Fire);
            mouseTrailMenu.addItem(MouseTrailOcean, LOC(kOcean), true, currentTrailTheme == MouseTrailConfig::TrailTheme::Ocean);
            mouseTrailMenu.addItem(MouseTrailGalaxy, LOC(kGalaxy), true, currentTrailTheme == MouseTrailConfig::TrailTheme::Galaxy);
            mouseTrailMenu.addItem(MouseTrailCherryBlossom, LOC(kCherryBlossom), true, currentTrailTheme == MouseTrailConfig::TrailTheme::CherryBlossom);
            mouseTrailMenu.addItem(MouseTrailMatrix, LOC(kMatrix), true, currentTrailTheme == MouseTrailConfig::TrailTheme::Matrix);
            menu.addSubMenu(LOC(kMouseTrail), mouseTrailMenu);
            break;
        }
        default:
            break;
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

        case OpenHelp:
            listeners_.call([](Listener& l) { l.helpRequested(); });
            break;

        case ShowWaveform:
        {
            bool newState = !processor_.getShowWaveform();
            processor_.setShowWaveform(newState);
            listeners_.call([newState](Listener& l) { l.showWaveformToggled(newState); });
            menuItemsChanged();
            break;
        }
        case ShowLanes:
        {
            bool newState = !processor_.getShowLanes();
            processor_.setShowLanes(newState);
            listeners_.call([newState](Listener& l) { l.showLanesToggled(newState); });
            menuItemsChanged();
            break;
        }
        case ThemeBlueBreeze:
            listeners_.call([](Listener& l) { l.themeChanged(ThemeId::BlueBreeze); });
            menuItemsChanged();
            break;
        case ThemeDarkBlueGrey:
            listeners_.call([](Listener& l) { l.themeChanged(ThemeId::DarkBlueGrey); });
            menuItemsChanged();
            break;
        case ThemeAurora:
            listeners_.call([](Listener& l) { l.themeChanged(ThemeId::Aurora); });
            menuItemsChanged();
            break;
            
        case MouseTrailNone:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::None); });
            menuItemsChanged();
            break;
        case MouseTrailClassic:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Classic); });
            menuItemsChanged();
            break;
        case MouseTrailNeon:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Neon); });
            menuItemsChanged();
            break;
        case MouseTrailFire:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Fire); });
            menuItemsChanged();
            break;
        case MouseTrailOcean:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Ocean); });
            menuItemsChanged();
            break;
        case MouseTrailGalaxy:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Galaxy); });
            menuItemsChanged();
            break;
        case MouseTrailCherryBlossom:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::CherryBlossom); });
            menuItemsChanged();
            break;
        case MouseTrailMatrix:
            listeners_.call([](Listener& l) { l.mouseTrailThemeChanged(MouseTrailConfig::TrailTheme::Matrix); });
            menuItemsChanged();
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
