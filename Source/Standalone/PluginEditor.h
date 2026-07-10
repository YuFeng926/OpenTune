#pragma once

/**
 * OpenTune 主编辑器窗口
 * 
 * 实现 JUCE AudioProcessorEditor 接口，作为插件的主 UI 界面。
 * 负责协调各个 UI 组件、处理用户交互、管理异步任务等。
 */

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include "PluginProcessor.h"
#include "UI/ToolIds.h"
#include "UI/ParameterPanel.h"
#include "UI/PianoRollComponent.h"
#include "UI/MenuBarComponent.h"
#include "UI/TransportBarComponent.h"
#include "UI/TopBarComponent.h"
#include "UI/TrackPanelComponent.h"
#include "UI/ArrangementViewComponent.h"
#include "UI/OpenTuneLookAndFeel.h"
#include "UI/OpenTuneTooltipWindow.h"
#include "UI/AuroraLookAndFeel.h"
#include "UI/UIColors.h"
#include "UI/RippleOverlayComponent.h"
#include "Editor/AutoRenderOverlayComponent.h"
#include "../Editor/RenderBadgeComponent.h"
#include "Utils/AppPreferences.h"
#include "Utils/ProjectSession.h"
#include "Utils/LocalizationManager.h"
#include "Utils/ScaleUiMapping.h"
#include "Audio/AsyncAudioLoader.h"

namespace OpenTune {

class ProjectSession;

// ============================================================================
// Import Drop Target — resolved by the Standalone editor from drag-drop geometry
// ============================================================================

struct ImportDropTarget {
    enum class Kind { ExistingTrack, NewTrack, FallbackActiveTrack, Reject };

    Kind kind = Kind::FallbackActiveTrack;
    int trackId = 0;
    double timelineStartSeconds = 0.0;
    juce::String rejectReason;  // Only for Kind::Reject

    bool isActionable() const { return kind != Kind::Reject; }
};

class OpenTuneAudioProcessorEditor : public juce::AudioProcessorEditor,
                                      public ParameterPanel::Listener,
                                      public MenuBarComponent::Listener,
                                      public TransportBarComponent::Listener,
                                      public TrackPanelComponent::Listener,
                                      public ArrangementViewComponent::Listener,
                                      public PianoRollComponent::Listener,
                                      public juce::FileDragAndDropTarget,
                                      public LanguageChangeListener,  // 语言变化监听
                                      private juce::Timer
{
public:
    explicit OpenTuneAudioProcessorEditor(OpenTuneAudioProcessor&);
    ~OpenTuneAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;

    // ParameterPanel::Listener
    void retuneSpeedChanged(float speed) override;
    void vibratoDepthChanged(float value) override;
    void vibratoRateChanged(float value) override;
    void noteSplitChanged(float value) override;
    void toolSelected(int toolId) override;
    void parameterDragEnded(int paramId, float oldValue, float newValue) override;
    void pitchShiftRequested() override;

    // MenuBarComponent::Listener
    void importAudioRequested() override;  // 新版本：不再需要trackId参数
    void exportAudioRequested(MenuBarComponent::ExportType exportType) override;  // 使用ExportType枚举
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
    void trackColorModeChanged(TrackColorMode mode) override;

    // TransportBarComponent::Listener
    void playRequested() override;
    void pauseRequested() override;
    void stopRequested() override;
    void loopToggled(bool enabled) override;
    void bpmChanged(double newBpm) override;
    void scaleChanged(int rootNote, int scaleType) override;
    void viewToggled(bool workspaceView) override;

    // TrackPanelComponent::Listener
    void trackSelected(int trackId) override;
    void trackMuteToggled(int trackId, bool muted) override;
    void trackSoloToggled(int trackId, bool solo) override;
    void trackVolumeChanged(int trackId, float volume) override;
    void trackHeightChanged(int newHeight) override;  // 与 ArrangementView 的 trackHeight 同步
    void trackColorChangeRequested(int trackId) override;
    void trackAddRequested() override;
    void trackDuplicateRequested(int trackId) override;
    void trackDeleteRequested(int trackId) override;
    void trackColorRandomizeRequested(int trackId) override;
    void visibleTrackCountChanged(int newCount) override;

    // ArrangementViewComponent::Listener
    void placementSelectionChanged(int trackId, uint64_t placementId) override;
    void placementTimingChanged(int trackId, int placementIndex) override;
    void placementDoubleClicked(int trackId, int placementIndex) override;
    void verticalScrollChanged(int newOffset) override;
    void referenceButtonClicked(int trackId, uint64_t placementId, juce::Rectangle<int> buttonScreenArea) override;

    void scrollModeChanged(bool isContinuous) override;
    // trackHeightChanged已在TrackPanelComponent::Listener中声明

    // PianoRollComponent::Listener
    void playheadPositionChangeRequested(double timeSeconds) override;
    void playPauseToggleRequested() override;
    void stopPlaybackRequested() override;
    void autoTuneRequested() override;
    void pitchCurveEdited(int startFrame, int endFrame) override;
    void escapeKeyPressed() override;
    void currentToolChanged(ToolId tool) override;

    // Keyboard handling
    bool keyPressed(const juce::KeyPress& key) override;

    // LanguageChangeListener
    void languageChanged(Language newLanguage) override;

private:
    struct PendingImport;
    struct AutoRefUiState
    {
        OpenTuneAudioProcessor::AutoRefAvailability availability;
        ParameterPanel::AutoButtonPresentation presentation;

        bool shouldRunReferenceAuto() const noexcept
        {
            return availability.canRunAutoRef();
        }
    };

    bool shouldAcceptUndoRedoShortcut();
    void performUndoRedoAction(bool isUndo);

    // 调式状态辅助
    DetectedKey resolveScaleForPlacementContent(int trackId, int placementIndex, juce::String* sourceOut = nullptr) const;
    void applyScaleToUi(int rootNote, int scaleType);
    void applyResolvedScaleForPlacementContent(int trackId, int placementIndex);
    void syncPianoRollFromPlacementSelection(int trackId, int placementIndex);
    void applyPlacementSelectionContext(int trackId, uint64_t placementId);

    // Reference auto-align methods
    AutoRefUiState evaluateAutoRefUiState() const;
    void refreshReferenceContext();
    void resolveReferenceBindingMenu(int trackId, uint64_t placementId, juce::Rectangle<int> buttonScreenArea = {});
    bool handleAutoRefExecute();

    void timerCallback() override;
    void showPreferencesDialog();
    void syncSharedAppPreferences();
    void syncTrackColorsToPanel();
    void applyThemeToEditor(ThemeId themeId);
    RenderStatusSnapshot getRenderStatusSnapshot() const;
    void setInferenceActive(bool active);
    void syncParameterPanelFromSelection();
    void updateTitleWithProjectPath();
    void syncRecentProjectsToMenu();
    void refreshAllUIFromProject();
    void launchOpenProjectChooser();
    void saveProjectAsThenOpenProject();
    void playFromStartToggleRequested();  // 播放/暂停并回到起始位置
    void importAudioFileToTrack(int trackId, const juce::File& file,
                                double timelineStartSeconds = -1.0);  // -1 = auto-append
    ImportDropTarget resolveImportDropTarget(int globalX, int globalY) const;
    void applyImportDropTarget(ImportDropTarget target, const juce::File& file);
    void updateImportDropPreview(ImportDropTarget target);
    void clearImportDropPreview();
    void queuePendingImport(PendingImport pendingImport);
    void startPendingImport(PendingImport pendingImport);
    void processNextImportInQueue();  // 处理导入队列中的下一个文件
    void launchBackgroundUiTask(std::function<void()> task);
    void waitForBackgroundUiTasks();
    double computeTrackAppendStartSeconds(int trackId) const;
    void releaseImportBatchSlot(int batchId);

    OpenTuneAudioProcessor& processorRef_;
    AppPreferences appPreferences_;
    std::shared_ptr<LocalizationManager::LanguageState> languageState_;
    LocalizationManager::ScopedLanguageBinding languageBinding_;
    KeyShortcutConfig::KeyShortcutSettings shortcutSettings_ = KeyShortcutConfig::KeyShortcutSettings::getDefault();

    // Custom LookAndFeel
    OpenTuneLookAndFeel openTuneLookAndFeel_;
    AuroraLookAndFeel auroraLookAndFeel_;
    juce::MouseCursor techCursor_;

    // Main GUI Components
    MenuBarComponent menuBar_;
    TransportBarComponent transportBar_;
    TopBarComponent topBar_;
    TrackPanelComponent trackPanel_;
    ParameterPanel parameterPanel_;
    ArrangementViewComponent arrangementView_;
    PianoRollComponent pianoRoll_;
    RippleOverlayComponent rippleOverlay_;
    AutoRenderOverlayComponent autoRenderOverlay_;
    RenderBadgeComponent renderBadge_;
    OpenTuneTooltipWindow tooltipWindow_{ this, 600 };

    // Project Session
    ProjectSession projectSession_;

    // Async loaders
    AsyncAudioLoader asyncAudioLoader_;
    bool isImportInProgress_ = false;
    
    // 多文件导入队列（解决并发导入问题）
    struct PendingImport {
        OpenTuneAudioProcessor::ImportPlacement placement;
        juce::File file;
        int batchId{0};
        bool appendSequentially{false};
    };
    std::vector<PendingImport> importQueue_;
    std::unordered_map<int, double> importBatchNextStartSeconds_;
    std::unordered_map<int, int> importBatchRemainingItems_;
    int nextImportBatchId_{1};

    bool isWorkspaceView_ = true;

    // 现代布局：左右面板可折叠（用于“沉浸主画布”模式）
    bool isTrackPanelVisible_ = true;
    bool isParameterPanelVisible_ = true;

    bool f0ParamsSyncedFromInference_ = false;
    ThemeId appliedThemeId_ = ThemeId::Aurora;
    Language appliedLanguage_ = Language::Chinese;
    bool inferenceActive_ = false;
    int inferenceActiveTickCounter_ = 0;
    ContentKey lastPianoRollContentKey_;
    int lastPianoRollSampleRate_ = 0;
    std::shared_ptr<PitchCurve> lastPianoRollCurve_;
    std::shared_ptr<const juce::AudioBuffer<float>> lastPianoRollBuffer_;
    // Notes-revision tracking so the timer can pull fresh notes when an
    // async note generator (GAME) commits to the active content
    // without changing ContentKey / curve / buffer.
    uint64_t lastPianoRollNotesRevision_ = 0;
    uint64_t lastPianoRollTimeGridRevision_ = 0;
    uint64_t lastPianoRollPitchRevision_ = 0;
    double lastSyncedBpm_ = 0.0;
    int lastSyncedTimeSigNum_ = 0;
    int lastSyncedTimeSigDenom_ = 0;
#if JUCE_DEBUG
    int diagnosticHeartbeatCounter_ = 0;
#endif
    bool showingSingleNoteParams_ = false;

    // Undo 状态追踪
    int lastScaleRootNote_ = 0;
    int lastScaleType_ = 1;  // 1=Major
    uint32_t lastUndoRedoShortcutMs_ = 0;
    bool suppressScaleChangedCallback_ = false;
    std::array<float, OpenTuneAudioProcessor::MAX_TRACKS> lastTrackVolumes_;
    
    // RMVPE OriginalF0 阻塞事务锁：提取开始后 latch，直到"提取成功且当前钢琴卷帘可见"才释放
    bool rmvpeOverlayLatched_ = false;
    ContentKey rmvpeOverlayTargetContentKey_;


    // Export worker thread management
    std::thread exportWorker_;
    // Save worker thread (joinable, same pattern as exportWorker_)
    std::thread saveWorker_;
    // Open worker thread (for async project loading)
    std::thread openWorker_;
    std::atomic<bool> exportInProgress_{false};

    // Detached-safe background tasks (import/deferred post-process)
    std::vector<std::future<void>> backgroundTasks_;

    // Layout constants
    static constexpr int MENU_BAR_HEIGHT = 25;
    static constexpr int TOP_PANEL_HEIGHT = 45;  // Single row: Scale and transport controls
    static constexpr int TRANSPORT_BAR_HEIGHT = 64; // Increased for larger buttons (was 60)
    static constexpr int TRACK_PANEL_WIDTH = 180;      // 左侧 Track Inspector (Reduced from 220)
    static constexpr int PARAMETER_PANEL_WIDTH = 240;  // 右侧 Properties Panel

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneAudioProcessorEditor)
};

} // namespace OpenTune
