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
#include <optional>
#include <vector>

#include "Content/ContentKey.h"
#include "PluginProcessor.h"
#include "Utils/AppPreferences.h"
#include "Utils/CursorTheme.h"
#include "Utils/ContentTimelineProjection.h"
#include "Utils/PitchShiftSettings.h"
#include "Utils/LocalizationManager.h"
#include "UI/ToolIds.h"
#include "UI/ParameterPanel.h"
#include "UI/PianoRollComponent.h"
#include "UI/MenuBarComponent.h"
#include "UI/TransportBarComponent.h"
#include "UI/TopBarComponent.h"
#include "Utils/TimelineDisplayMode.h"
#include "UI/OpenTuneLookAndFeel.h"
#include "UI/OpenTuneTooltipWindow.h"
#include "UI/AuroraLookAndFeel.h"
#include "UI/UIColors.h"
#include "UI/TimelineOverviewComponent.h"
#include "Editor/AutoRenderOverlayComponent.h"
#include "../Editor/RenderBadgeComponent.h"

namespace OpenTune::PluginUI {

class OpenTuneAudioProcessorEditor : public juce::AudioProcessorEditor,
#if JucePlugin_Enable_ARA
                                     public juce::AudioProcessorEditorARAExtension,
#endif
                                     public TimelineOverviewComponent::Listener,
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
    void cursorStyleChanged(CursorStyleId style) override;
    void languageChanged(Language newLanguage) override;

    void playRequested() override;
    void pauseRequested() override;
    void stopRequested() override;
    void loopToggled(bool enabled) override;
    void bpmChanged(double newBpm) override;
    void timeSignatureChanged(int numerator, int denominator) override;
    void scaleChanged(int rootNote, int scaleType) override;
    void viewToggled(bool workspaceView) override;
    void recordRequested() override;
    void timelineDisplayModeChanged(TimelineDisplayMode mode) override;

    bool playheadPositionChangeRequested(double timeSeconds) override;
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
        std::optional<PianoRollPlacementIdentity> activePlacementIdentity;
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
    void overviewNavigateRequested(double visibleStartSeconds,
                                   double pixelsPerSecond) override;
    void syncSharedAppPreferences();
    void applyThemeToEditor(ThemeId themeId);
    ContentKey resolveCurrentContentKey();
    PianoRollContentSync resolveCurrentContentSync();
    void syncParameterPanelFromSelection();
    PianoRollContentSync syncContentProjectionToPianoRoll();
    void rememberPresentedPianoRollViewport();
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
    TimelineOverviewComponent overviewStrip_;
    AutoRenderOverlayComponent autoRenderOverlay_;
    RenderBadgeComponent renderBadge_;
    OpenTuneTooltipWindow tooltipWindow_{ this, 600 };

    bool suppressScaleChangedCallback_ = false;
    double lastSyncedBpm_ = 120.0;
    int lastSyncedTimeSigNum_ = 4;
    int lastSyncedTimeSigDenom_ = 4;
    TimelineDisplayMode timelineDisplayMode_ = TimelineDisplayMode::Time;

    bool showingSingleNoteParams_{false};
    // 读取音频后 latch：F0 提取 + note 生成全部完成前保持"正在处理音频"遮罩
    bool rmvpeOverlayLatched_ = false;
    std::vector<ContentKey> rmvpeOverlayTargetContentKeys_;
    // Tracks last-seen notesRevision per active content so the timer
    // can pull fresh notes when an async note generator (GAME) commits late.
    // Tracks last-seen ContentKey for revision baseline only (not session last-active).
    ContentKey lastRevisionObservedContentKey_;
    std::optional<PianoRollPlacementIdentity> presentedPlacementIdentity_;
    std::map<ContentKey, OriginalF0State> lastObservedOriginalF0States_;
    // Read 读取音频时捕获的 OpenDyne 一次性音符生成意图（F0 Ready 后消费）
    std::map<ContentKey, NoteGeneratorParams> pendingNoteGenerationOnReady_;
    uint64_t lastPianoRollNotesRevision_{0};
    uint64_t lastPianoRollTimeGridRevision_{0};
    uint64_t lastPianoRollPitchRevision_{0};
    PitchShiftSettings lastPitchShiftIndicatorSettings_;

    static constexpr int TOP_BAR_HEIGHT = 96;
    static constexpr int PARAMETER_PANEL_WIDTH = 240;
    static constexpr int OVERVIEW_STRIP_HEIGHT = 60;
    static constexpr int kHeartbeatHz = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneAudioProcessorEditor)
};

} // namespace OpenTune::PluginUI

#endif // JucePlugin_Build_VST3
