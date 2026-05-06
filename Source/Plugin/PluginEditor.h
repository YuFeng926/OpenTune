#pragma once

/**
 * VST3 插件编辑器（Plugin Editor）
 *
 * VST3/ARA 格式专属的 UI 壳层。通过 Timer 心跳轮询 Processor 状态，
 * 将 ARA preferred region 的 Materialization 投射到 PianoRoll 进行编辑。
 * 与 Standalone Editor 共享 PianoRollComponent 和 ParameterPanel，
 * 但不包含多轨 Arrangement 视图。
 *
 * 编译隔离：整个文件由 JucePlugin_Build_VST3 守卫。
 */
#if JucePlugin_Build_VST3

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

#if JucePlugin_Enable_ARA
#include "ARA/VST3AraSession.h"
#endif

#include "PluginProcessor.h"
#include "Utils/AppPreferences.h"
#include "Utils/MaterializationTimelineProjection.h"
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
    void savePresetRequested() override;
    void loadPresetRequested() override;
    void preferencesRequested() override;
    void helpRequested() override;
    void showWaveformToggled(bool shouldShow) override;
    void showLanesToggled(bool shouldShow) override;
    void noteNameModeChanged(NoteNameMode noteNameMode) override;
    void showChunkBoundariesToggled(bool shouldShow) override;
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

    void playheadPositionChangeRequested(double timeSeconds) override;
    void playPauseToggleRequested() override;
    void stopPlaybackRequested() override;
    void autoTuneRequested() override;
    void pitchCurveEdited(int startFrame, int endFrame) override;
    void escapeKeyPressed() override;

private:
    void timerCallback() override;
    void syncSharedAppPreferences();
    void applyThemeToEditor(ThemeId themeId);
    enum class MaterializationSource { None, Ara, Capture };
    uint64_t resolveCurrentMaterializationId();
    MaterializationSource resolveCurrentMaterializationProjection(uint64_t& materializationId,
                                                                  MaterializationTimelineProjection& projection);
    void syncParameterPanelFromSelection();
    void syncMaterializationProjectionToPianoRoll();
    void syncImportedAraClipIfNeeded();
    void showPreferencesDialog();

    OpenTuneAudioProcessor& processorRef_;
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
    bool rmvpeOverlayLatched_{false};
    uint64_t rmvpeOverlayTargetMaterializationId_{0};
    bool araClipImportArmed_{false};
    bool showingSingleNoteParams_{false};
    bool initialFocusGrabbed_{false};

#if JucePlugin_Enable_ARA
    uint64_t lastConsumedAraSnapshotEpoch_{0};
    VST3AraSession::RegionIdentity lastConsumedPreferredAraRegion_{};
#endif

    static constexpr int TOP_BAR_HEIGHT = 96;
    static constexpr int PARAMETER_PANEL_WIDTH = 240;
    static constexpr int kHeartbeatHz = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneAudioProcessorEditor)
};

} // namespace OpenTune::PluginUI

#endif // JucePlugin_Build_VST3
