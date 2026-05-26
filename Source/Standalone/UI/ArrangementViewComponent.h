#pragma once

/**
 * 排列视图组件
 * 
 * 显示多轨道的音频片段排列视图，支持：
 * - 片段显示与拖拽
 * - 波形可视化（通过 WaveformTileCache 和 ArrangementRenderModelCache）
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
#include "WaveformTileCache.h"
#include "ArrangementRenderModelCache.h"
#include "TimelineViewportState.h"
#include "../Utils/ZoomSensitivityConfig.h"
#include "../Utils/KeyShortcutConfig.h"

namespace OpenTune {

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
    void setInferenceActive(bool active) { inferenceActive_ = active; }
    void fitToContent();
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

#if JUCE_DEBUG
    static bool runDebugSelfTest();
#endif

private:
    struct HitTestResult {
        int trackId{-1};
        int placementIndex{-1};
        juce::Rectangle<int> placementBounds;
        bool isTopEdge{false};
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
    double viewportXToAbsoluteTime(int x) const;
    int getTotalContentWidth() const;
    void updateAutoScroll();
    void performPageScroll(double playheadTime);
    void onScrollVBlankCallback(double timestampSec);
    double readPlayheadSeconds() const;
    void syncPlayheadOverlay();
    void syncPlayheadOverlayToAbsoluteTime(double absoluteSeconds, bool repaintOverlay = false);
    void updateScrollBars();
    void drawTimeRuler(juce::Graphics& g);
    void drawGridLines(juce::Graphics& g);
    void drawPlacementClips(juce::Graphics& g,
                            const ArrangementRenderModelCache::RenderModel& model,
                            const TimelineViewportState& viewport);

    /** Request render model rebuild from current state. */
    void requestRenderModelUpdate();
    void refreshRenderModel();

    OpenTuneAudioProcessor& processor_;
    juce::ListenerList<Listener> listeners_;

    bool buildWaveformCaches(double timeBudgetMs);

    // ---- Timeline rendering pipeline ----
    TimelineViewportState viewportState_;
    ArrangementRenderModelCache renderModelCache_;
    WaveformTileCache waveformTileCache_;
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

    std::atomic<bool> isPlaying_{false};  // atomic 确保 VBlank 线程安全
    double zoomLevel_{1.0};
    int scrollOffset_{0};
    int verticalScrollOffset_{0};
    
    // Smooth scrolling
    float smoothScrollCurrent_{0.0f}; // To track sub-pixel position for smoothness
    bool isSmoothScrolling_{false};
    double lastPaintedPlayheadTime_{-1.0};

    // 用户是否手动调整过缩放（用于避免自动缩放覆盖用户设置）
    bool userHasManuallyZoomed_ = false;
    ZoomSensitivityConfig::ZoomSensitivitySettings zoomSensitivity_ = ZoomSensitivityConfig::ZoomSensitivitySettings::getDefault();
    KeyShortcutConfig::KeyShortcutSettings shortcutSettings_ = KeyShortcutConfig::KeyShortcutSettings::getDefault();

    int waveformBuildTickCounter_{ 0 }; // 播放状态下的限频计数器
    bool inferenceActive_{ false };

    int selectedTrack_{0};
    int selectedPlacementIndex_{0};
    uint64_t selectedPlacementId_{0};

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

    // 高性能播放头覆盖层（VBlank同步，独立于主组件重绘）
    PlayheadOverlayComponent playheadOverlay_;

    // 滚动跟随独立 VBlank 附件（仅负责滚动，不影响 Overlay 的 VBlank）
    std::unique_ptr<juce::VBlankAttachment> scrollVBlankAttachment_;

    // 播放头位置源（来自 Processor 的原子位置）
    std::weak_ptr<std::atomic<double>> positionSource_;

    static constexpr int rulerHeight_ = 30;
};

} // namespace OpenTune
