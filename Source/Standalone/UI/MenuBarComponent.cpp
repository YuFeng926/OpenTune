#include "MenuBarComponent.h"
#include "../PluginProcessor.h"
#include "UIColors.h"
#include "../../Utils/LocalizationManager.h"

namespace OpenTune {

MenuBarComponent::MenuBarComponent(OpenTuneAudioProcessor& processor, Profile profile)
    : processor_(processor)
    , profile_(profile)
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

void MenuBarComponent::setNoteNameMode(NoteNameMode noteNameMode)
{
    if (noteNameMode_ == noteNameMode) {
        return;
    }

    noteNameMode_ = noteNameMode;
    menuItemsChanged();
}

void MenuBarComponent::setShowChunkBoundaries(bool shouldShow)
{
    if (showChunkBoundaries_ == shouldShow) {
        return;
    }

    showChunkBoundaries_ = shouldShow;
    menuItemsChanged();
}

void MenuBarComponent::setShowUnvoicedFrames(bool shouldShow)
{
    if (showUnvoicedFrames_ == shouldShow) {
        return;
    }

    showUnvoicedFrames_ = shouldShow;
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
            if (profile_ == Profile::Standalone) {
                menu.addItem(ImportAudio, LOC(kImportAudio));

                juce::PopupMenu exportMenu;
                exportMenu.addItem(ExportSelectedClip, LOC(kExportSelectedClip));
                exportMenu.addItem(ExportTrack, LOC(kExportTrack));
                exportMenu.addItem(ExportBus, LOC(kExportBus));
                menu.addSubMenu(LOC(kExportAudio), exportMenu);

                menu.addSeparator();
                menu.addItem(SaveProject, LOC(kSaveProject));
                menu.addItem(SaveProjectAs, LOC(kSaveProjectAs));
                menu.addSeparator();
                menu.addItem(OpenProject, LOC(kOpenProject));
                if (!recentProjects_.empty()) {
                    juce::PopupMenu recentMenu;
                    int baseId = RecentProjectBase;
                    for (const auto& f : recentProjects_) {
                        recentMenu.addItem(baseId, f.getFileName());
                        ++baseId;
                    }
                    recentMenu.addSeparator();
                    recentMenu.addItem(ClearRecentProjects, LOC(kClearRecentProjects));
                    menu.addSubMenu(LOC(kRecentProjects), recentMenu);
                }
                menu.addSeparator();
            }

            menu.addItem(OpenPreferences, LOC(kOptions));
            menu.addSeparator();
            menu.addItem(OpenHelp, LOC(kHelp));
            break;
        }
        case 1:  // Edit
        {
            menu.addItem(EditUndo, LOC(kUndo) + "  (Ctrl+Z)", true);
            menu.addItem(EditRedo, LOC(kRedo) + "  (Ctrl+Shift+Z)", true);
            break;
        }
        case 2:  // View
        {
            menu.addItem(ShowWaveform, LOC(kShowWaveform), true, processor_.getShowWaveform());
            menu.addItem(ShowLanes, LOC(kShowLanes), true, processor_.getShowLanes());

            juce::PopupMenu noteLabelMenu;
            noteLabelMenu.addItem(NoteNameModeShowAll, LOC(kNoteLabelsShowAll), true, noteNameMode_ == NoteNameMode::ShowAll);
            noteLabelMenu.addItem(NoteNameModeCOnly, LOC(kNoteLabelsCOnly), true, noteNameMode_ == NoteNameMode::COnly);
            noteLabelMenu.addItem(NoteNameModeHide, LOC(kNoteLabelsHide), true, noteNameMode_ == NoteNameMode::Hide);

            menu.addSeparator();
            menu.addSubMenu(LOC(kNoteLabels), noteLabelMenu);
            menu.addItem(ShowChunkBoundaries, LOC(kShowChunkBoundaries), true, showChunkBoundaries_);
            menu.addItem(ShowUnvoicedFrames, LOC(kShowUnvoicedFrames), true, showUnvoicedFrames_);

            juce::PopupMenu themeMenu;
            themeMenu.addItem(ThemeBlueBreeze, LOC(kThemeBlueBreeze), true, UIColors::currentThemeId() == ThemeId::BlueBreeze);
            themeMenu.addItem(ThemeDarkBlueGrey, LOC(kThemeDarkBlueGrey), true, UIColors::currentThemeId() == ThemeId::DarkBlueGrey);
            themeMenu.addItem(ThemeAurora, LOC(kThemeAurora), true, UIColors::currentThemeId() == ThemeId::Aurora);
            // themeMenu.addItem(ThemeOverdose, LOC(kThemeOverdose), true, UIColors::currentThemeId() == ThemeId::Overdose);  // "升天" 暂时隐藏
            menu.addSeparator();
            menu.addSubMenu(LOC(kTheme), themeMenu);
            
            if (profile_ == Profile::Standalone) {
                const auto currentTrailTheme = mouseTrailTheme_;
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

                juce::PopupMenu trackColorMenu;
                trackColorMenu.addItem(TrackColorsRandom, LOC(kTrackColorsRandom), true, trackColorMode_ == TrackColorMode::Random);
                trackColorMenu.addItem(TrackColorsCustom, LOC(kTrackColorsCustom), true, trackColorMode_ == TrackColorMode::Custom);
                menu.addSubMenu(LOC(kTrackColors), trackColorMenu);
            }
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
    switch (menuItemID)
    {
        case ImportAudio:
            // 触发导入音频，由PluginEditor处理文件选择、多选支持和导入模式询问
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

        case SaveProject:
            listeners_.call([](Listener& l) { l.saveProjectRequested(); });
            break;
        case SaveProjectAs:
            listeners_.call([](Listener& l) { l.saveProjectAsRequested(); });
            break;
        case OpenProject:
            listeners_.call([](Listener& l) { l.openProjectRequested(); });
            break;

        case ClearRecentProjects:
            listeners_.call([](Listener& l) { l.clearRecentProjectsRequested(); });
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
        case NoteNameModeShowAll:
            listeners_.call([](Listener& l) { l.noteNameModeChanged(NoteNameMode::ShowAll); });
            menuItemsChanged();
            break;
        case NoteNameModeCOnly:
            listeners_.call([](Listener& l) { l.noteNameModeChanged(NoteNameMode::COnly); });
            menuItemsChanged();
            break;
        case NoteNameModeHide:
            listeners_.call([](Listener& l) { l.noteNameModeChanged(NoteNameMode::Hide); });
            menuItemsChanged();
            break;
        case ShowChunkBoundaries:
        {
            const bool newState = !showChunkBoundaries_;
            listeners_.call([newState](Listener& l) { l.showChunkBoundariesToggled(newState); });
            menuItemsChanged();
            break;
        }
        case ShowUnvoicedFrames:
        {
            const bool newState = !showUnvoicedFrames_;
            listeners_.call([newState](Listener& l) { l.showUnvoicedFramesToggled(newState); });
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
        // case ThemeOverdose:  // "升天" 暂时隐藏
        //     listeners_.call([](Listener& l) { l.themeChanged(ThemeId::Overdose); });
        //     menuItemsChanged();
        //     break;
            
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

        case TrackColorsRandom:
            listeners_.call([](Listener& l) { l.trackColorModeChanged(TrackColorMode::Random); });
            menuItemsChanged();
            break;
        case TrackColorsCustom:
            listeners_.call([](Listener& l) { l.trackColorModeChanged(TrackColorMode::Custom); });
            menuItemsChanged();
            break;

        case EditUndo:
            listeners_.call([](Listener& l) { l.undoRequested(); });
            break;

        case EditRedo:
            listeners_.call([](Listener& l) { l.redoRequested(); });
            break;

        default:
        {
            // Handle recent project items
            if (menuItemID >= RecentProjectBase && menuItemID < RecentProjectBase + static_cast<int>(recentProjects_.size())) {
                const auto index = static_cast<size_t>(menuItemID - RecentProjectBase);
                if (index < recentProjects_.size()) {
                    listeners_.call([&recentProjects = recentProjects_, index](Listener& l) {
                        l.openRecentProjectRequested(recentProjects[index]);
                    });
                }
            }
            break;
        }
    }
}

void MenuBarComponent::setTrackColorMode(TrackColorMode mode)
{
    if (trackColorMode_ == mode) return;
    trackColorMode_ = mode;
    menuItemsChanged();
}

void MenuBarComponent::setRecentProjects(const std::vector<juce::File>& recentFiles)
{
    recentProjects_ = recentFiles;
}

} // namespace OpenTune
