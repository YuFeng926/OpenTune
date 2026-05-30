#pragma once

/**
 * 排列视图组件
 * 
 * 显示多轨道的音频片段排列视图，支持：
 * - 片段显示与拖拽
 * - 波形可视化（通过 WaveformMipmapCache 和 ArrangementRenderModelCache）
 * - 时间标尺和网格
 * - 播放头位置显示（通过 PlayheadOverlayComponent）
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>
#include <map>
#include "../PluginProcessor.h"
#include "UIColors.h"
#include "SmallButton.h"
#include "PlayheadOverlayComponent.h"
#include "WaveformMipmap.h"
#include "ArrangementRenderModelCache.h"
#include "TimelineViewportState.h"
#include "../Utils/ZoomSensitivityConfig.h"
#include "../Utils/KeyShortcutConfig.h"

namespace OpenTune {

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

class ArrangementCachedSurface : public juce::Component
{
public:
    ArrangementCachedSurface()
    {
        setOpaque(false);
        setInterceptsMouseClicks(false, false);
    }

    void setSurfaceImage(juce::Image image)
    {
        surfaceImage_ = std::move(image);
        repaint();
    }

    void setImageOffsetX(int offsetX)
    {
        if (imageOffsetX_ == offsetX)
            return;

        imageOffsetX_ = offsetX;
    }

    void clearSurfaceImage()
    {
        if (!surfaceImage_.isValid())
            return;

        surfaceImage_ = {};
        imageOffsetX_ = 0;
        repaint();
    }

private:
    void paint(juce::Graphics& g) override
    {
        if (surfaceImage_.isValid())
            g.drawImageAt(surfaceImage_, imageOffsetX_, 0);
    }

    juce::Image surfaceImage_;
    int imageOffsetX_ = 0;
};

class ArrangementViewComponent : public juce::Component,
                                 public juce::ScrollBar::Listener,
                                 public juce::Timer
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
        // 水平时间轴同步回调 - 用于同步 PianoRoll
        virtual void horizontalScrollChanged(int newOffset) { juce::ignoreUnused(newOffset); }
        virtual void zoomLevelChanged(double newZoom) { juce::ignoreUnused(newZoom); }
        virtual void scrollModeChanged(bool isContinuous) { juce::ignoreUnused(isContinuous); }
    };

    ArrangementViewComponent(OpenTuneAudioProcessor& processor);
    ~ArrangementViewComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;
    void onHeartbeatTick();

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

    void scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) override;

    void setIsPlaying(bool playing) {
        const bool stateChanged = (isPlaying_.load(std::memory_order_relaxed) != playing);
        isPlaying_.store(playing, std::memory_order_relaxed);
        playheadOverlay_.setPlaying(playing);
        if (stateChanged) {
            resetPresentationClock(readPlayheadSeconds());
            syncPlayheadOverlayToAbsoluteTime(readPlayheadSeconds(), true);
        }
    }
    // 设置播放头颜色（主题切换时调用）
    void setPlayheadColour(juce::Colour colour) {
        playheadOverlay_.setPlayheadColour(colour);
    }
    
    // 设置播放头位置源（由组件内部读取）
    void setPlayheadPositionSource(std::weak_ptr<std::atomic<double>> source) {
        positionSource_ = source;
    }
    void setZoomLevel(double zoom);
    void setScrollOffset(int pixels);
    void setVerticalScrollOffset(int offset);
    void setVisibleTrackCount(int count);
    void setInferenceActive(bool active) { inferenceActive_ = active; }
    void fitToContent();
    void setExperimentalReferenceControlsEnabled(bool enabled);
    void setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& settings) { zoomSensitivity_ = settings; }
    void setShortcutSettings(const KeyShortcutConfig::KeyShortcutSettings& settings) { shortcutSettings_ = settings; }
    bool isWaveformCacheCompleteForMaterialization(int trackId, uint64_t materializationId) const;

    // 缩放状态管理
    void resetUserZoomFlag() { userHasManuallyZoomed_ = false; }
    bool hasUserManuallyZoomed() const { return userHasManuallyZoomed_; }

    // Analysis animation 状态管理
    void setClipAnalysisInProgress(uint64_t placementId, bool inProgress);

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    // Import drop preview (transient, UI-only)
    void setImportDropPreview(const ImportDropPreview& preview);
    void clearImportDropPreview();

    // Public geometry queries for import target resolution
    int trackIdForViewportY(int y) const noexcept;
    double viewportXToAbsoluteTime(int x) const;
    int getRulerHeight() const noexcept { return rulerHeight_; }

#if JUCE_DEBUG
    static bool runDebugSelfTest();
#endif

private:
    enum class DragOperation { None, Move, Gain, TrimLeft, TrimRight, FadeIn, FadeOut };

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

    // 时间 ↔ 像素坐标（委托给 TimelineViewportState）
    int absoluteTimeToContentX(double seconds) const;
    int absoluteTimeToViewportX(double seconds) const;
    int absoluteTimeToViewportX(double seconds, double projectedScrollOffset) const;
    int getTotalContentWidth() const;
    int getVisibleViewportWidth() const;
    juce::Rectangle<int> getContentViewportBounds() const;
    bool isPinnedContinuousFollowActive() const;
    double getPinnedPlayheadViewportX() const;
    double getContinuousFollowTargetScroll(double displayPlayheadTime) const;
    double getDisplayPlayheadTime(double timestampSec) const;
    void updatePresentationClock(double authoritativeTime, double timestampSec);
    void resetPresentationClock(double authoritativeTime);
    void rebuildContentMetrics();
    bool renderBandNeedsRebuild() const;
    bool ensureRenderBandCoversCurrentViewport(bool forceRebuild = false);
    void rebuildContentSurface();
    void updateContentSurfaceBounds();
    void drawTimeRulerBackdrop(juce::Graphics& g);
    void rebuildRulerSurface();
    void updateRulerSurfaceBounds();
    void updateOverlayPresentation(double displayPlayheadTime);
    void updateAutoScroll();
    void performPageScroll(double playheadTime);
    void onScrollVBlankCallback(double timestampSec);
    double readPlayheadSeconds() const;
public:
    void syncPlayheadOverlay();
    void syncPlayheadOverlayToAbsoluteTime(double absoluteSeconds, bool repaintOverlay = false);

private:
    void updateScrollBars();
    void drawTimeRuler(juce::Graphics& g);
    void drawGridLines(juce::Graphics& g);
    void drawPlacementClips(juce::Graphics& g,
                            const ArrangementRenderModelCache::RenderModel& model,
                            const TimelineViewportState& viewport);

    /** Request render model rebuild from current state. */
    void requestRenderModelUpdate();
    void refreshRenderModel();
    void updateMoveDragPreview(const juce::MouseEvent& e);
    void clearMoveDragPreview();

    OpenTuneAudioProcessor& processor_;
    juce::ListenerList<Listener> listeners_;

    bool buildWaveformCaches(double timeBudgetMs);

    // ---- Timeline rendering pipeline ----
    TimelineViewportState viewportState_;
    ArrangementRenderModelCache renderModelCache_;
    WaveformMipmapCache waveformMipmapCache_;
    ArrangementCachedSurface contentSurface_;
    ArrangementCachedSurface rulerSurface_;
    juce::Image contentSurfaceImage_;
    juce::Image rulerSurfaceImage_;
    juce::Rectangle<int> contentSurfaceBounds_;
    juce::Rectangle<int> rulerSurfaceBounds_;

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

    std::atomic<bool> isPlaying_{false};  // atomic 确保 VBlank 线程安全
    double zoomLevel_{1.0};
    int scrollOffset_{0};
    int verticalScrollOffset_{0};
    int visibleTrackCount_{2};  // synced from TrackPanel via PluginEditor
    double lastAuthoritativePlayheadTime_{0.0};
    double presentationClockAnchorTime_{0.0};
    double presentationClockAnchorTimestampSec_{0.0};
    double presentationClockLastObservationTimestampSec_{0.0};
    bool presentationClockPrimed_{false};

    struct ContentMetrics {
        uint64_t revision = 0;
        double maxEndTimeSeconds = 60.0 * 5.0;
        int totalContentWidthPx = 0;
    };

    struct RenderBandState {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        int startContentX = 0;
        int widthPx = 0;
        int heightPx = 0;
        uint64_t revision = 0;
        bool valid = false;
    };

    struct RulerSurfaceState {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        double zoomLevel = 0.0;
        double bpm = 0.0;
        int startContentX = 0;
        int widthPx = 0;
        int timeSigNum = 0;
        int timeSigDenom = 0;
        TimeUnit timeUnit = TimeUnit::Seconds;
        ThemeId themeId = ThemeId::DarkBlueGrey;
        bool valid = false;
    };

    ContentMetrics contentMetrics_;
    RenderBandState renderBand_;
    RulerSurfaceState rulerSurfaceState_;
    
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
    uint64_t hoveredPlacementId_{0};       // 当前鼠标悬停的 placement
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

    // === 多选拖拽状态 ===
    struct DragStartState {
        int trackId;
        uint64_t placementId;
        double startSeconds;
    };
    std::vector<DragStartState> multiDragStartStates_;
    ArrangementRenderModelCache::MoveDragPreviewState moveDragPreview_;
    uint64_t nextMoveDragPreviewRevision_{ 1 };

    bool isDraggingPlacement_{false};
    bool isAdjustingGain_{false};
    bool isDraggingPlayhead_{false};
    bool isPanning_{false};
    juce::Point<int> dragStartPos_;
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

    // 高性能播放头覆盖层（VBlank同步，独立于主组件重绘）
    PlayheadOverlayComponent playheadOverlay_;

    // 滚动跟随独立 VBlank 附件（仅负责滚动，不影响 Overlay 的 VBlank）
    std::unique_ptr<juce::VBlankAttachment> scrollVBlankAttachment_;

    // 播放头位置源（来自 Processor 的原子位置）
    std::weak_ptr<std::atomic<double>> positionSource_;

    // Import drop preview state (transient, cleared on drop/cancel)
    ImportDropPreview importDropPreview_;

    static constexpr int rulerHeight_ = 30;
};

} // namespace OpenTune
