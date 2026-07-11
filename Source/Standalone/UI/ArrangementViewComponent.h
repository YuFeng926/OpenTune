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
#include "TimelinePatternCache.h"
#include "TimelineViewportPolicy.h"
#include "../../TimelineContentCache.h"
#include "../Utils/ZoomSensitivityConfig.h"
#include "../Utils/KeyShortcutConfig.h"

namespace OpenTune {

// ============================================================================
// Arrangement Vertical Window — defines the vertical viewport state
// ============================================================================

struct ArrangementVerticalWindow {
    int rulerHeight = 0;
    int trackHeight = 0;
    int scrollTopPx = 0;               // content-area track-space y (pixels scrolled past ruler)
    int viewportContentHeight = 0;     // visible content area height (excluding ruler)

    int firstTrack() const {
        return trackHeight > 0 ? scrollTopPx / trackHeight : 0;
    }

    int lastTrackExclusive() const {
        if (trackHeight <= 0) return 0;
        const int totalPx = scrollTopPx + viewportContentHeight;
        return (totalPx + trackHeight - 1) / trackHeight;  // ceil division
    }

    // Encode all state that affects tile content into a single hash
    uint64_t encode() const {
        // Pack into 64 bits: rulerHeight(16) | trackHeight(16) | scrollTopPx(16) | viewportContentHeight(16)
        uint64_t h = 0;
        h |= static_cast<uint64_t>(rulerHeight & 0xFFFF);
        h |= static_cast<uint64_t>(trackHeight & 0xFFFF) << 16;
        h |= static_cast<uint64_t>(scrollTopPx & 0xFFFF) << 32;
        h |= static_cast<uint64_t>(viewportContentHeight & 0xFFFF) << 48;
        return h;
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
        virtual void scrollModeChanged(bool isContinuous) { juce::ignoreUnused(isContinuous); }
    };

    ArrangementViewComponent(OpenTuneAudioProcessor& processor);
    ~ArrangementViewComponent() override;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
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
        if (stateChanged) {
            updateOverlayPresentation();
        }
    }
    void setPlayheadColour(juce::Colour colour) {
        playheadColour_ = colour;
    }
    
    // 设置播放头位置源（由组件内部读取）
    void setPlayheadPositionSource(std::weak_ptr<std::atomic<double>> source) {
        positionSource_ = source;
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



    // Camera-derived state
    ViewMapper makeViewMapper() const noexcept;
    ArrangementVerticalWindow makeArrangementVerticalWindow() const noexcept;

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
    void updateOverlayPresentation();
    TimelineViewportRequest makeViewportRequest(
        TimelineViewportRequest::Kind kind,
        double targetTime,
        double anchorViewportX,
        double pps) const;

    void onScrollVBlankCallback(double timestampSec);
    double readPlayheadSeconds() const;

public:
    void syncFixedPlayhead();

private:
    void updateScrollBars();
    void requestVisualRefresh();
    void rebuildTimelineCoverage();
    void updateMoveDragOverlay(const juce::MouseEvent& e);
    void clearMoveDragOverlay();
    void drawTransientOverlay(juce::Graphics& g);
    void drawFixedChrome(juce::Graphics& g);
    void drawPlayhead(juce::Graphics& g);
    void drawImportDropPreview(juce::Graphics& g);
    void drawMoveDragOverlay(juce::Graphics& g);

    OpenTuneAudioProcessor& processor_;
    juce::ListenerList<Listener> listeners_;

    bool buildWaveformCaches(double timeBudgetMs);

    // ---- Timeline rendering pipeline ----
    TimelineViewportCamera camera_{0.0, TimelineViewportCamera::kDefaultPixelsPerSecond};
    double tileCoverageStartSeconds_ = 0.0;
    double tileCoverageEndSeconds_ = 0.0;
    WaveformMipmapCache waveformMipmapCache_;

    // New: Pattern and content tile caches for infinite timeline
    mutable TimelinePatternCache patternCache_;
    mutable TimelineContentCache contentCache_;

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
    juce::Colour playheadColour_{UIColors::playhead};
    int verticalScrollOffset_{0};
    int visibleTrackCount_{2};  // synced from TrackPanel via PluginEditor

    struct ContentMetrics {
        uint64_t revision = 0;
        double maxEndTimeSeconds = 60.0 * 5.0;
        int totalContentWidthPx = 0;
    };

    ContentMetrics contentMetrics_;

    // Pre-built pattern tiles for paint consumption
    struct PreparedPatternTile {
        PatternTileKey key;
        const juce::Image* image = nullptr;
    };
    std::vector<PreparedPatternTile> preparedPatternTiles_;
    void prepareCoveragePatternTiles();

    // Pre-built content tiles for paint consumption (P0-1)
    struct PreparedContentTile {
        ContentTileKey key;
        const juce::Image* image = nullptr;
    };
    std::vector<PreparedContentTile> preparedContentTiles_;
    void prepareCoverageContentTiles();
    // Waveform visual refresh is deferred during playback via waveformVisualRefreshPending_

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

    // 播放头位置源（来自 Processor 的原子位置）
    std::weak_ptr<std::atomic<double>> positionSource_;

    // Import drop preview state (transient, cleared on drop/cancel)
    ImportDropPreview importDropPreview_;

    static constexpr int rulerHeight_ = 30;
};

} // namespace OpenTune
