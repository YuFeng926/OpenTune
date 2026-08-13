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
#include <optional>
#include "UIColors.h"
#include "SmallButton.h"
#include "WaveformMipmap.h"
#include "ViewMapper.h"
#include "TimelineViewportCamera.h"
#include "TimelineViewportPolicy.h"
#include "TimelineCompositeCache.h"
#include "../Utils/ZoomSensitivityConfig.h"
#include "../Utils/KeyShortcutConfig.h"
#include "../../Utils/TimelineDisplayMode.h"

namespace OpenTune {

class OpenTuneAudioProcessor;
struct PlayHeadState;

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
        virtual void timelineDisplayModeChanged(TimelineDisplayMode mode) { juce::ignoreUnused(mode); }
        // View emits playhead position requests only (ruler seek / empty-space seek).
        // The editor owns transport decisions; the view never drives transport state.
        virtual bool playheadPositionChangeRequested(double /*timeSeconds*/) { return false; }
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
    void setTimelineDisplayMode(TimelineDisplayMode mode);
    TimelineDisplayMode getTimelineDisplayMode() const noexcept { return displayMode_; }

    // 缩放状态管理
    void resetUserZoomFlag() { userHasManuallyZoomed_ = false; }
    bool hasUserManuallyZoomed() const { return userHasManuallyZoomed_; }

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
    void drawMoveDragOverlay(juce::Graphics& g);
    void drawReferenceHoverOverlay(juce::Graphics& g);

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

    // Last render background signature — only used to detect BPM/time sig/display mode changes
    BackgroundGenerationSignature lastBgSignature_{};
    // Last render foreground signature — only used to detect content/selection revision changes
    ForegroundGenerationSignature lastFgSignature_{};

    juce::ScrollBar horizontalScrollBar_{ false };
    juce::ScrollBar verticalScrollBar_{ true };
    juce::TextButton scrollModeToggleButton_;
    SmallButton timeUnitToggleButton_;
    SmallButtonLookAndFeel smallButtonLookAndFeel_;

    enum class ScrollMode { Page, Continuous };
    ScrollMode scrollMode_{ ScrollMode::Continuous };

    TimelineDisplayMode displayMode_ = TimelineDisplayMode::Time;

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

    BackgroundGenerationSignature makeBackgroundSignature() const;
    ForegroundGenerationSignature makeForegroundSignature() const;
    uint64_t computeSelectionRevision() const noexcept;
    void buildCompositeBackground(juce::Graphics& g, juce::Rectangle<int> tileBounds,
                                  TimelineCompositeCache::TileKey key);
    void buildCompositeForeground(juce::Graphics& g, juce::Rectangle<int> tileBounds,
                                  TimelineCompositeCache::TileKey key);
    void prepareCoverageCompositeTiles();

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

    uint64_t hoveredReferencePlacementId_{0};
    juce::Rectangle<int> hoveredReferenceButtonBounds_{};

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
    // 轴锁定状态（与 InteractionState::AxisLockState 同构，避免跨模块依赖）
    struct AxisLockState {
        enum class Axis { None, Horizontal, Vertical };
        Axis lockedAxis = Axis::None;
        bool decided = false;
        void reset() { lockedAxis = Axis::None; decided = false; }
        Axis resolve(int dx, int dy, int threshold) {
            if (decided) return lockedAxis;
            if (std::abs(dx) > threshold || std::abs(dy) > threshold) {
                lockedAxis = std::abs(dx) >= std::abs(dy) ? Axis::Horizontal : Axis::Vertical;
                decided = true;
                return lockedAxis;
            }
            return Axis::None;
        }
    };
    AxisLockState panAxisLock_;
    juce::Point<int> panStartPos_;
    double panStartVisibleStartSeconds_ = 0.0;
    int panStartVerticalScrollOffset_ = 0;
    juce::Point<int> dragStartPos_;
    juce::Point<int> dragCurrentPos_;
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
