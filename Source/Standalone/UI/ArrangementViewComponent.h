#pragma once

/**
 * 排列视图组件
 * 
 * 显示多轨道的音频片段排列视图，支持：
 * - 片段显示与拖拽
 * - 波形可视化（通过 WaveformMipmapCache）
 * - 时间标尺和网格
 * - 播放头位置显示（通过 paint() 直接绘制）
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>
#include <map>
#include <optional>
#include "../PluginProcessor.h"
#include "UIColors.h"
#include "SmallButton.h"
#include "WaveformMipmap.h"
#include "ViewMapper.h"
#include "TimelineViewportCamera.h"
#include "TimelineViewportPolicy.h"
#include "TimelineCompositeCache.h"
#include "../Utils/ZoomSensitivityConfig.h"
#include "../Utils/KeyShortcutConfig.h"

namespace OpenTune {

// ============================================================================
// Arrangement Vertical Window — defines the vertical viewport state
// ============================================================================

struct ArrangementVerticalWindow {
    int trackHeight = 0;
    int worldTopY = 0;                 // world-space tile origin y
    int viewportContentHeight = 0;     // visible content area height (excluding ruler)

    int firstTrack() const {
        return trackHeight > 0 ? worldTopY / trackHeight : 0;
    }

    int lastTrackExclusive() const {
        if (trackHeight <= 0) return 0;
        const int totalPx = worldTopY + viewportContentHeight;
        return (totalPx + trackHeight - 1) / trackHeight;  // ceil division
    }

};

// ============================================================================
// Import Drop Preview — transient UI-only state for drag-drop visual feedback
// ============================================================================

struct ImportDropPreview {
    bool active = false;
    int targetTrackId = -1;        // -1 = none, track index for existing-track drop
    bool isNewTrack = false;       // true = blank-area drop; render a "new track" indicator
    int visibleTrackCount = 0;     // current visible track count (for positioning new-track indicator)
    int trackHeight = 100;         // track lane height in pixels (for positioning; avoids paint() processor read)
};

class ArrangementViewComponent : public juce::Component,
                                 public juce::ScrollBar::Listener
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void placementSelectionChanged(int trackId, uint64_t placementId) = 0;
        virtual void placementTimingChanged(int trackId, int placementIndex) = 0;
        virtual void placementDoubleClicked(int /*trackId*/, int /*placementIndex*/) {}
        virtual void referenceButtonClicked(int /*trackId*/, uint64_t /*placementId*/,
                                              juce::Rectangle<int> /*buttonScreenArea*/ = {}) {}
        // Y轴缩放回调 - 通知外部轨道高度变化（用于同步TrackPanel）
        virtual void trackHeightChanged(int newHeight) { juce::ignoreUnused(newHeight); }
        // Y轴滚动回调 - 通知外部垂直滚动偏移变化（用于同步TrackPanel）
        virtual void verticalScrollChanged(int newOffset) { juce::ignoreUnused(newOffset); }
        virtual void scrollModeChanged(bool isContinuous) { juce::ignoreUnused(isContinuous); }
        // Transport requests: the view only emits user intent, the editor decides
        // whether to drive processor setters (Standalone) or ARA HostPlaybackController
        // requests (ARA). The view never writes processor transport directly.
        virtual bool playheadPositionChangeRequested(double /*timeSeconds*/) { return false; }
        virtual void playPauseToggleRequested() {}
    };

    ArrangementViewComponent(OpenTuneAudioProcessor& processor);
    ~ArrangementViewComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void onHeartbeatTick();

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

    void scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) override;

    void setPlayheadColour(juce::Colour colour) {
        playheadColour_ = colour;
        repaint();
    }

    void commitViewportRequest(TimelineViewportRequest req);
    TimelineViewportCamera timelineCamera() const noexcept { return camera_; }
    void activateTimelineCamera(TimelineViewportCamera camera);
    int timelinePolicyViewportWidth() const noexcept { return getVisibleViewportWidth(); }
    void setVerticalScrollOffset(int offset);
    void setVisibleTrackCount(int count);
    void setInferenceActive(bool active) { inferenceActive_ = active; }
    void fitToContent();
    void setExperimentalReferenceControlsEnabled(bool enabled);
    void setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& settings) { zoomSensitivity_ = settings; }
    void setShortcutSettings(const KeyShortcutConfig::KeyShortcutSettings& settings) { shortcutSettings_ = settings; }

    // 缩放状态管理
    void resetUserZoomFlag() { userHasManuallyZoomed_ = false; }
    bool hasUserManuallyZoomed() const { return userHasManuallyZoomed_; }

    // Analysis animation 状态管理
    void setClipAnalysisInProgress(uint64_t placementId, bool inProgress);

    void addListener(Listener* listener);
    void removeListener(Listener* listener);
    void requestContentRedraw();
    void requestThemeRedraw();

    // Import drop preview (transient, UI-only)
    void setImportDropPreview(const ImportDropPreview& preview);
    void clearImportDropPreview();

    // Public geometry queries for import target resolution
    int trackIdForViewportY(int y) const noexcept;
    double viewportXToAbsoluteTime(int x) const;
    int getRulerHeight() const noexcept { return rulerHeight_; }

private:
    enum class DragOperation { None, Move, Gain, TrimLeft, TrimRight, FadeIn, FadeOut };



    // Camera-derived state
    ViewMapper makeViewMapper() const noexcept;

    struct HitTestResult {
        int trackId{-1};
        int placementIndex{-1};
        juce::Rectangle<int> placementBounds;
        bool isTopEdge{false};
        bool isLeftEdge{false};
        bool isRightEdge{false};
        bool isFadeInHandle{false};
        bool isFadeOutHandle{false};
    };

    HitTestResult hitTestPlacement(juce::Point<int> p) const;

    // Analysis state（reference binding 状态由 placement.referencePlacementId 驱动，不再缓存）
    struct ClipAnalysisState {
        bool isAnalysisInProgress{false};      // true: 显示描边动画
    };

    juce::Rectangle<int> getTrackLaneBounds(int trackId) const;
    juce::Rectangle<int> buildProjectedPlacementBounds(int trackId, int placementIndex) const;
    juce::Rectangle<int> getPlacementBounds(int trackId, int placementIndex) const;

    // 时间 ↔ 像素坐标（委托给 ViewMapper）
    int absoluteTimeToViewportX(double seconds) const;
    int getTotalContentWidth() const;
    int getVisibleViewportWidth() const;
    juce::Rectangle<int> getContentViewportBounds() const;
    void rebuildContentMetrics();
    TimelineViewportRequest makeViewportRequest(
        TimelineViewportRequest::Kind kind,
        double targetTime,
        double anchorViewportX,
        double pps) const;

    void onScrollVBlankCallback(double timestampSec);
    double readPlayheadSeconds() const;

private:
    void updateScrollBars();
    void rebuildTimelineCoverage();
    void invalidateStableScene();
    void updateMoveDragOverlay(const juce::MouseEvent& e);
    void clearMoveDragOverlay();
    void drawPlayhead(juce::Graphics& g);
    void drawImportDropPreview(juce::Graphics& g);
    void drawSelectionOverlay(juce::Graphics& g);
    void drawMoveDragOverlay(juce::Graphics& g);

    OpenTuneAudioProcessor& processor_;
    const PlayHeadState& playHeadState_;
    juce::ListenerList<Listener> listeners_;

    bool buildWaveformCaches(double timeBudgetMs);
    void preparePlaybackCoverage();

    // ===== view retained surface =====
    juce::Image viewportSurface_{};
    juce::Image themeBackdrop_{};
    void rebuildThemeBackdrop();
    int64_t surfaceOriginPx_ = 0;   // llround(visibleStartSeconds * pps) at last surface render
    double  surfacePps_ = 0.0;      // pps at last surface render; change → full rebuild
    juce::Rectangle<int> lastPlayheadRect_{};  // previous frame playhead presentation rect
    int64_t lastDpiMilli_ = 1000;

    // Presentation-only bounded ease-out transition for Continuous follow
    // return-to-centre. Not transport truth; not shared; cleared on mode switch.
    // Driven by the VBlank timestamp, not a per-frame low-pass.
    // requestTransition_ is an explicit one-shot arm signal: set by user
    // ruler/empty seek, playhead drag, or the stop→play edge. VBlank consumes
    // it once. Normal continuous playback never arms it — camera follows the
    // target directly with no subpixel-threshold auto-trigger.
    bool transitionActive_ = false;
    double transitionStartTimestamp_ = 0.0;
    double transitionStartVisibleSeconds_ = 0.0;
    bool requestTransition_ = false;
    static constexpr double kContinuousTransitionDurationSec = 0.18;

    // ---- View retained surface helper methods ----
    juce::Rectangle<int> timeAxisRect() const noexcept;
    void surfaceInvalidate();
    void surfaceRebuildFromReadyTiles(int64_t firstTile, int64_t lastTile);
    void surfaceScrollAndFillExposed(int64_t newOriginPx, int64_t firstTile, int64_t lastTile);
    juce::Rectangle<int> playheadDirtyRect() const;
    // ---- Timeline rendering pipeline ----
    TimelineViewportCamera camera_{0.0, TimelineViewportCamera::kDefaultPixelsPerSecond};
    double tileCoverageStartSeconds_ = 0.0;
    double tileCoverageEndSeconds_ = 0.0;
    WaveformMipmapCache waveformMipmapCache_;

    double lastContextBpm_{ 0.0 };
    int lastContextTimeSigNum_{ 0 };
    int lastContextTimeSigDenom_{ 0 };

    juce::ScrollBar horizontalScrollBar_{ false };
    juce::ScrollBar verticalScrollBar_{ true };
    juce::TextButton scrollModeToggleButton_;
    juce::TextButton timeUnitToggleButton_;
    SmallButtonLookAndFeel smallButtonLookAndFeel_;

    enum class ScrollMode { Page, Continuous };
    ScrollMode scrollMode_{ ScrollMode::Continuous };

    enum class TimeUnit { Seconds, Bars };
    TimeUnit timeUnit_{ TimeUnit::Seconds };

    bool lastObservedPlayHeadPlaying_{false};

    juce::Colour playheadColour_{UIColors::playhead};
    int verticalScrollOffset_{0};
    int visibleTrackCount_{2};  // synced from TrackPanel via PluginEditor

    struct ContentMetrics {
        uint64_t revision = 0;
        double maxEndTimeSeconds = 60.0 * 5.0;
        int totalContentWidthPx = 0;
    };

    ContentMetrics contentMetrics_;
    uint64_t waveformRevision_ = 0;
    uint64_t lastWaveformSyncRevision_ = 0;

    // ---- Composite cache (new tile pipeline) ----
    mutable TimelineCompositeCache compositeCache_;

    GenerationSignature makeGenerationSignature() const;
    void buildCompositeTile(juce::Graphics& g, juce::Rectangle<int> tileBounds,
                           TimelineCompositeCache::TileKey key);
    void prepareCoverageCompositeTilesNew();

    // Smooth scrolling
    // 用户是否手动调整过缩放（用于避免自动缩放覆盖用户设置）
    bool userHasManuallyZoomed_ = false;
    ZoomSensitivityConfig::ZoomSensitivitySettings zoomSensitivity_ = ZoomSensitivityConfig::ZoomSensitivitySettings::getDefault();
    KeyShortcutConfig::KeyShortcutSettings shortcutSettings_ = KeyShortcutConfig::KeyShortcutSettings::getDefault();

    int waveformBuildTickCounter_{ 0 }; // 播放状态下的限频计数器
    bool inferenceActive_{ false };
    bool waveformVisualRefreshPending_{ false };

    int selectedTrack_{0};
    int selectedPlacementIndex_{0};
    uint64_t selectedPlacementId_{0};
    bool experimentalReferenceControlsEnabled_{false};

    // reference binding 状态（placementId → state）
    std::map<uint64_t, ClipAnalysisState> clipAnalysisStates_;
    bool mouseOverReferenceButton_{false}; // 鼠标在参考按钮区域内

    // === 多选支持 ===
    struct PlacementSelectionKey {
        int trackId;
        uint64_t placementId;
        bool operator<(const PlacementSelectionKey& other) const {
            return std::tie(trackId, placementId) < std::tie(other.trackId, other.placementId);
        }
        bool operator==(const PlacementSelectionKey& other) const {
            return trackId == other.trackId && placementId == other.placementId;
        }
    };
    std::set<PlacementSelectionKey> selectedPlacements_;
    bool isMultiSelectMode_{false};
    PlacementSelectionKey shiftAnchor_;
    bool hasShiftAnchor_{false};

    bool isPlacementSelected(int trackId, uint64_t placementId) const;
    void togglePlacementSelection(int trackId, uint64_t placementId);
    void clearPlacementSelection();
    void selectPlacementsInRange(const PlacementSelectionKey& from, const PlacementSelectionKey& to);
    void selectAllPlacementsInTrack(int trackId);
    void commitPlacementSelection(PlacementSelectionKey primary);
    void commitEmptyPlacementSelection();

    // === 多选拖拽状态 ===
    struct MoveDragStartState {
        int trackId = -1;
        uint64_t placementId = 0;
        double startSeconds = 0.0;
        double durationSeconds = 0.0;
        juce::String name;
    };
    std::vector<MoveDragStartState> moveDragStartStates_;
    PlacementSelectionKey moveDragPrimaryStart_{-1, 0};

    struct MoveDragResolvedTarget {
        int trackId = 0;
        double startSeconds = 0.0;
    };

    MoveDragResolvedTarget resolveMoveDragTarget(const MoveDragStartState& state,
                                                 double deltaSeconds,
                                                 int trackDelta) const;

    std::vector<MoveDragStartState> resolveMoveDragParticipants(const HitTestResult& hit) const;
    void beginMoveDrag(const HitTestResult& hit, juce::Point<int> mousePos);
    void finishMoveDrag(const juce::MouseEvent& e);

    bool isDraggingPlacement_{false};
    bool isAdjustingGain_{false};
    bool isDraggingPlayhead_{false};
    bool isPanning_{false};
    juce::Point<int> dragStartPos_;
    juce::Point<int> dragCurrentPos_;
    juce::Point<int> lastMousePos_;
    double dragStartPlacementSeconds_{0.0};
    float dragStartPlacementGain_{1.0f};
    uint64_t dragStartPlacementId_{0};
    int dragStartTrackId_{-1};  // 拖拽开始时的轨道ID（用于跨轨道移动）

    DragOperation currentDragOp_{DragOperation::None};
    double trimStartClipInSeconds_{0.0};
    double trimStartDurationSeconds_{0.0};
    double fadeStartInDuration_{0.0};
    double fadeStartOutDuration_{0.0};
    uint64_t dragOperationPlacementId_{0};

    // 滚动跟随独立 VBlank 附件（仅负责滚动，不影响 Overlay 的 VBlank）
    std::unique_ptr<juce::VBlankAttachment> scrollVBlankAttachment_;

    // 播放头展示缓存（retained drawing），由 processor-owned PlayHeadState 驱动
    double playheadTimeForPaint_ = 0.0;

    // Import drop preview state (transient, cleared on drop/cancel)
    ImportDropPreview importDropPreview_;

    static constexpr int rulerHeight_ = 30;
};

} // namespace OpenTune
