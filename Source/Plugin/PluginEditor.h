#pragma once

/**
 * VST3 插件编辑器（Plugin Editor）
 *
 * VST3/ARA 格式专属的 UI 壳层。通过 Timer 心跳轮询 Processor 状态，
 * 将 ARA EditorView selection 的 Content 投射到 PianoRoll 进行编辑。
 * 与 Standalone Editor 共享 PianoRollComponent 和 ParameterPanel，
 * 但不包含多轨 Arrangement 视图。
 *
 * 编译隔离：整个文件由 JucePlugin_Build_VST3 守卫。
 */
#if JucePlugin_Build_VST3

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <map>
#include <vector>

#include "Content/ContentKey.h"
#include "PluginProcessor.h"
#include "Utils/AppPreferences.h"
#include "Utils/ContentTimelineProjection.h"
#include "Utils/LocalizationManager.h"
#include "UI/ToolIds.h"
#include "UI/ParameterPanel.h"
#include "UI/PianoRollComponent.h"
#include "UI/MenuBarComponent.h"
#include "UI/TransportBarComponent.h"
#include "UI/TopBarComponent.h"
#include "UI/OpenTuneLookAndFeel.h"
#include "UI/OpenTuneTooltipWindow.h"
#include "UI/AuroraLookAndFeel.h"
#include "UI/UIColors.h"
#include "Editor/AutoRenderOverlayComponent.h"
#include "../Editor/RenderBadgeComponent.h"

namespace OpenTune::PluginUI {

class OpenTuneAudioProcessorEditor : public juce::AudioProcessorEditor,
#if JucePlugin_Enable_ARA
                                     public juce::AudioProcessorEditorARAExtension,
#endif
                                     public ParameterPanel::Listener,
                                     public MenuBarComponent::Listener,
                                     public TransportBarComponent::Listener,
                                     public PianoRollComponent::Listener,
                                     public LanguageChangeListener,
                                     private juce::Timer
{
public:
    explicit OpenTuneAudioProcessorEditor(OpenTuneAudioProcessor& processor);
    ~OpenTuneAudioProcessorEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;

    void retuneSpeedChanged(float speed) override;
    void vibratoDepthChanged(float value) override;
    void vibratoRateChanged(float value) override;
    void noteSplitChanged(float value) override;
    void toolSelected(int toolId) override;

    void importAudioRequested() override;
    void exportAudioRequested(MenuBarComponent::ExportType exportType) override;
    void openProjectRequested() override;
    void saveProjectRequested() override;
    void saveProjectAsRequested() override;
    void openRecentProjectRequested(const juce::File& file) override;
    void clearRecentProjectsRequested() override;
    void preferencesRequested() override;
    void helpRequested() override;
    void showWaveformToggled(bool shouldShow) override;
    void showLanesToggled(bool shouldShow) override;
    void noteNameModeChanged(NoteNameMode noteNameMode) override;
    void showUnvoicedFramesToggled(bool shouldShow) override;
    void themeChanged(ThemeId themeId) override;
    void undoRequested() override;
    void redoRequested() override;
    void mouseTrailThemeChanged(MouseTrailConfig::TrailTheme theme) override;
    void languageChanged(Language newLanguage) override;

    void playRequested() override;
    void pauseRequested() override;
    void stopRequested() override;
    void loopToggled(bool enabled) override;
    void bpmChanged(double newBpm) override;
    void scaleChanged(int rootNote, int scaleType) override;
    void viewToggled(bool workspaceView) override;
    void recordRequested() override;

    bool playheadPositionChangeRequested(double timeSeconds) override;
    void playPauseToggleRequested() override;
    void stopPlaybackRequested() override;
    void autoTuneRequested() override;
    void pitchShiftRequested() override;
    void pitchCurveEdited(int startFrame, int endFrame) override;
    void escapeKeyPressed() override;
    void currentToolChanged(ToolId tool) override;

private:
    struct PianoRollContentSync
    {
        std::vector<TimelineContentPlacement> placements;
        ContentKey activeContentKey;
        double timelineViewStartSeconds = 0.0;
        double timelineViewEndSeconds = 0.0;

        bool hasPlacements() const noexcept
        {
            return !placements.empty();
        }

        bool hasActiveContent() const noexcept
        {
            return activeContentKey.isValid();
        }
    };

    void timerCallback() override;
    void syncSharedAppPreferences();
    void applyThemeToEditor(ThemeId themeId);
    ContentKey resolveCurrentContentKey();
    PianoRollContentSync resolveCurrentContentSync();
    void syncParameterPanelFromSelection();
    void syncContentProjectionToPianoRoll();
    void showPreferencesDialog();
    bool handleEditorShortcut(const juce::KeyPress& key);
    void surfaceRegularVst3HostControlledTransport(const char* actionName);

    OpenTuneAudioProcessor& processorRef_;

    std::shared_ptr<ContentEditCommands> contentCommands_;

    ContentEditCommands& getContentCommands() const { return *contentCommands_; }
    std::shared_ptr<ContentEditCommands> getContentCommandsShared() const { return contentCommands_; }

    AppPreferences appPreferences_;
    std::shared_ptr<LocalizationManager::LanguageState> languageState_;
    LocalizationManager::ScopedLanguageBinding languageBinding_;
    ThemeId appliedThemeId_ = ThemeId::Aurora;
    Language appliedLanguage_ = Language::Chinese;

    OpenTuneLookAndFeel openTuneLookAndFeel_;
    AuroraLookAndFeel auroraLookAndFeel_;

    MenuBarComponent menuBar_;
    TransportBarComponent transportBar_;
    TopBarComponent topBar_;
    ParameterPanel parameterPanel_;
    PianoRollComponent pianoRoll_;
    AutoRenderOverlayComponent autoRenderOverlay_;
    RenderBadgeComponent renderBadge_;
    OpenTuneTooltipWindow tooltipWindow_{ this, 600 };

    bool suppressScaleChangedCallback_ = false;
    double lastSyncedBpm_ = 120.0;
    int lastSyncedTimeSigNum_ = 4;
    int lastSyncedTimeSigDenom_ = 4;

    bool showingSingleNoteParams_{false};
    bool initialFocusGrabbed_{false};
    // Tracks last-seen notesRevision per active content so the timer
    // can pull fresh notes when an async note generator (GAME) commits late.
    ContentKey lastActiveContentKey_;
    std::map<ContentKey, OriginalF0State> lastObservedOriginalF0States_;
    uint64_t lastPianoRollNotesRevision_{0};
    uint64_t lastPianoRollTimeGridRevision_{0};
    uint64_t lastPianoRollPitchRevision_{0};

    // When true, the blocking overlay is shown until ARA content birth completes.
    bool waitingForAraContent_ = false;
    juce::uint32 araWaitStartMs_ = 0;

    static constexpr int TOP_BAR_HEIGHT = 96;
    static constexpr int PARAMETER_PANEL_WIDTH = 240;
    static constexpr int kHeartbeatHz = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneAudioProcessorEditor)
};

} // namespace OpenTune::PluginUI

#endif // JucePlugin_Build_VST3
