#pragma once

/**
 * 钢琴卷帘组件
 *
 * 显示和编辑音高曲线、音符序列的组件，支持：
 * - F0 曲线显示（原始音高和校正后音高）
 * - 音符绘制和编辑
 * - 多种工具（选择、绘制、音高线锚点等）
 * - 缩放和滚动
 *
 * 渲染架构：保留 Image 表面 + owner paint 分层合成
 * - staticSurface_  ：主题背景、标尺、lane、网格、琴键
 * - waveformSurface_：背景波形（时间参考层）
 * - contentSurface_ ：无声帧、notes、F0、TimeGrid 锚点、ghost（由 drawContent 共享）
 * - labelsSurface_  ：键盘隐藏时固定的音名
 * - overlay 层      ：播放头、选中高亮、框选、绘制预览、TimeGrid 把手（paint 内滚动条之后绘制）
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "ViewMapper.h"
#include "ToolIds.h"
#include "UIColors.h"
#include "Utils/F0Timeline.h"
#include "Utils/AudioEditingScheme.h"
#include "Utils/ContentTimelineProjection.h"
#include "Utils/PianoRollVisualPreferences.h"
#include "Utils/PitchCurve.h"
#include "Utils/Note.h"
#include "Utils/NoteGeneratorTypes.h"
#include "Utils/PitchControlConfig.h"
#include "Utils/KeyShortcutConfig.h"
#include "Utils/ContextMenuKey.h"
#include "Utils/ScreenReaderAnnouncer.h"
#include "Utils/ZoomSensitivityConfig.h"
#include <cmath>
#include <memory>
#include <algorithm>
#include <vector>
#include <map>
#include <optional>
#include <utility>
#include <atomic>
#include <array>
#include "SmallButton.h"
#include "PianoRoll/PianoRollRenderer.h"
#include "PianoRoll/PianoRollTimeMap.h"

#include "PianoRoll/PianoRollToolHandler.h"
#include "PianoRoll/InteractionState.h"
#include "PianoRoll/EqPopupComponent.h"
#include "TimelineViewportCamera.h"
#include "TimelineViewportPolicy.h"
#include "WaveformMipmap.h"
#include "../../Utils/UndoManager.h"
#include "../../Utils/SpectrumDisplayData.h"
#include "../../Content/ContentEditCommands.h"
#include "../../Utils/TimelineDisplayMode.h"
namespace OpenTune {

class PianoKeyAudition;
class NoteAudition;
class RenderCache;
class AppPreferences;
class OpenTuneAudioProcessor;
struct PlayHeadState;

struct PianoRollComponentTestProbe;

// ============================================================================
// PianoRollComponent — piano roll editor with retained-image rendering.
// ============================================================================

class PianoRollComponent : public juce::Component,
                           public juce::ScrollBar::Listener {
public:
    /** 钢琴卷帘完整镜头状态：横向 camera + 纵向缩放 + 纵向偏移。 */
    struct ViewportState
    {
        TimelineViewportCamera camera{0.0, TimelineViewportCamera::kDefaultPixelsPerSecond};
        float pixelsPerSemitone = 25.0f;
        float verticalScrollOffset = 0.0f;

        bool operator==(const ViewportState& other) const noexcept
        {
            return camera == other.camera
                && pixelsPerSemitone == other.pixelsPerSemitone
                && verticalScrollOffset == other.verticalScrollOffset;
        }
        bool operator!=(const ViewportState& other) const noexcept { return !(*this == other); }
    };

    void visibilityChanged() override;

    class Listener
    {
    public:
        enum class ParameterTargetChangePolicy { Commit, Cancel };

        virtual ~Listener() = default;
        virtual bool playheadPositionChangeRequested(double timeSeconds) = 0;
        // Drag preview. The default commits immediately so standalone keeps a live seek.
        virtual bool playheadPositionPreviewRequested(double timeSeconds)
        {
            return playheadPositionChangeRequested(timeSeconds);
        }
        virtual void playPauseToggleRequested() = 0;
        virtual void stopPlaybackRequested() = 0;
        virtual void pitchCurveEdited(int startFrame, int endFrame) { (void)startFrame; (void)endFrame; }
        virtual void noteOffsetChanged(size_t noteIndex, float oldOffset, float newOffset) { (void)noteIndex; (void)oldOffset; (void)newOffset; }
        virtual void contentEdited() {}
        virtual void autoTuneRequested() {}
        virtual void escapeKeyPressed() {}
        virtual void undoRequested() {}
        virtual void redoRequested() {}
        virtual bool pointerShortcutRequested(KeyShortcutConfig::ShortcutId /*id*/) { return false; }
        virtual void currentToolChanged(ToolId tool) { (void)tool; }
        virtual void timelineDisplayModeChanged(TimelineDisplayMode mode) { (void)mode; }
        virtual void parameterTargetWillChange(ParameterTargetChangePolicy policy) { (void)policy; }
        virtual void parameterTargetDidChange() {}
    };

    enum class ScrollMode
    {
        Page,
        Continuous
    };

    using TimelineContentPlacement = OpenTune::TimelineContentPlacement;

    /** Non-owning spectrum read callback injected by the Editor. */
    using SpectrumReader = std::function<void(SpectrumArray&, SpectrumArray&)>;

    PianoRollComponent(const PlayHeadState& playHeadState,
                       UndoManager& undoManager,
                       SpectrumReader spectrumReader,
                       bool transportControlsEnabled = true);
    ~PianoRollComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void setZoomControlsArea(juce::Rectangle<int> area);
    void onHeartbeatTick();

    void setEditedContent(ContentKey contentKey,
                           std::shared_ptr<const PitchCurveSnapshot> curve,
                           std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                           double sampleRate,
                           bool activePlacementChanged = false);
    bool setEditedContentAndProjection(ContentKey contentKey,
                                       std::shared_ptr<const PitchCurveSnapshot> curve,
                                       std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                       double sampleRate,
                                       const ContentTimelineProjection& projection,
                                       bool activePlacementChanged = false);
    void requestInitialF0View(ContentKey contentKey);
    /** 是否存在由当前内容或投影变化建立、尚未消费的 F0 初始视图定位请求。 */
    bool hasPendingInitialF0View() const noexcept { return pendingInitialF0ViewContentKey_.isValid(); }

    /**
     * 双击片段进入钢琴卷帘之前调用。该片段有可定位音符时取消尚未消费的 F0
     * 初始定位（否则进入后 F0 Ready 会把镜头再移走），并返回 true。
     * 没有可定位音符时什么都不做，返回 false。
     */
    bool prepareNoteCenterOnClipEntry();

    /**
     * 把离播放头最近的音符放到视口中央。播放头在音符内时对准播放头，
     * 否则对准离播放头更近的端点。横向用传入的钢琴卷帘缩放（Kind::Cont，
     * 不另算一套缩放），纵向保持当前音高缩放只滚动。成功后置 userScrollHold_，
     * 用户之后的平移/缩放不会被这次定位覆盖。没有可定位音符时返回 false。
     */
    bool centerNoteNearestPlayhead(double pixelsPerSecond);
    void onTimeGridRevisionChanged();
    void onNotesRevisionChanged();
    void onPitchRevisionChanged();
    void setPianoKeyAudition(PianoKeyAudition* audition) { pianoKeyAudition_ = audition; }
    void setNoteAudition(NoteAudition* audition) { noteAudition_ = audition; }
    /** Access to a content's rendered (corrected) audio, for keyboard note audition. */
    using RenderCacheProvider = std::function<std::shared_ptr<const RenderCache>(ContentKey)>;
    void setRenderCacheProvider(RenderCacheProvider provider) { renderCacheProvider_ = std::move(provider); }
    /** The plugin has no use for Esc in the piano roll: let the host have it. */
    void setEscapePassesToHost(bool passes) noexcept { escapePassesToHost_ = passes; }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    /** 注入 AppPreferences 指针（两个 Editor 构造/同步时直接注入，无中转层）。
     *  用于 EQ popup 和音符整体移动吸附偏好。 */
    void setAppPreferences(AppPreferences* prefs) noexcept { appPreferences_ = prefs; }
    /** Re-evaluate the accessibility preference; stops audition when it is turned off. */
    void applyAccessibilityMode();

    using ReadContentSnapshotFn = std::function<std::shared_ptr<const EditableContentSnapshot>(ContentKey)>;
    void setReadContentSnapshot(ReadContentSnapshotFn fn) { readContentSnapshot_ = std::move(fn); }

    void setContentCommands(std::shared_ptr<ContentEditCommands> commands);
    void setProcessor(OpenTuneAudioProcessor* processor);

    ContentKey editedContentKey() const { return editedContentKey_; }
    double audioSampleRate() const noexcept { return audioBufferSampleRate_; }
    ContentTimelineProjection activeContentProjection() const noexcept;
    WaveformMipmapCache& getWaveformMipmapCache() noexcept { return waveformMipmapCache_; }

    /** [ARA 重构] 注入域内容所有者（替代 setContentProviders）。统一 ARA/Standalone/Capture 路径。 */

    void commitViewportRequest(TimelineViewportRequest req);

    /** 总览条导航入口：设置 userScrollHold_ 后走 commitViewportRequest 单一路径，
     *  阻止下一帧 heartbeat 播放跟随立即夺回 camera。 */
    void navigateFromOverview(TimelineViewportRequest req);

    int timelinePolicyViewportWidth() const noexcept { return getTimelineContentViewportWidth(); }
    TimelineViewportCamera timelineCamera() const noexcept { return camera_; }
    void activateTimelineCamera(TimelineViewportCamera camera);

    /** 读取当前完整镜头状态（横向 camera + 纵向缩放 + 纵向偏移）。 */
    ViewportState viewportState() const noexcept;

    /** 原子恢复完整镜头状态：横向 camera、纵向缩放、纵向偏移、
     *  scrollbar range、raster 与 repaint 一次性同步。 */
    void restoreViewportState(const ViewportState& state);
    void setCurrentTool(ToolId tool);
    void setExperimentalFeaturesEnabled(bool enabled);
    ToolId getCurrentTool() const { return currentTool_; }
    PianoRollToolHandler* getToolHandler() const { return toolHandler_.get(); }
    void setShowWaveform(bool shouldShow);
    void setShowPianoKeyboard(bool shouldShow);
    void setScaleAssistEnabled(bool enabled);
    void setGridStyle(PianoGridStyle gridStyle);
    void setNoteNameMode(NoteNameMode noteNameMode);
    void setShowUnvoicedFrames(bool shouldShow);
    void setBackgroundBrightness(float brightness);
    /** Non-ARA capture is writing unscanned host audio. The timeline wash is the activity cue. */
    void setTrackCaptureTint(bool active);
    void setCorrectedF0Colour(juce::Colour colour);
    void setOriginalF0Colour(juce::Colour colour);
    void setBpm(double bpm);
    void setTimeSignature(int numerator, int denominator);
    void setTimelineDisplayMode(TimelineDisplayMode mode);
    TimelineDisplayMode getTimelineDisplayMode() const { return displayMode_; }
    void refreshLocalizedText();
    void setScrollMode(ScrollMode mode) {
        if (scrollMode_ == mode) return;
        scrollMode_ = mode;
        repaint();
    }
    void setScale(int rootNote, int scaleType);
    void setAudioEditingScheme(AudioEditingScheme::Scheme scheme) { applyAudioEditingScheme(scheme); }
    bool isOpenDyne() const noexcept { return AudioEditingScheme::usesNotesPrimaryScheme(audioEditingScheme_); }

    /** 轨道主题色注入：OpenDyne waveform blob 的填充/描边色。 */
    void setTrackDisplayColour(juce::Colour colour);
    void setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& settings) { zoomSensitivity_ = settings; }
    void setShortcutSettings(const KeyShortcutConfig::KeyShortcutSettings& settings) { shortcutSettings_ = settings; }

    void resetUserZoomFlag() { userHasManuallyZoomed_ = false; }

    void setRetuneSpeed(float speed) { currentRetuneSpeed_ = speed; }
    float getCurrentRetuneSpeed() const { return currentRetuneSpeed_; }
    void setVibratoDepth(float depth) { currentVibratoDepth_ = depth; }
    float getCurrentVibratoDepth() const { return currentVibratoDepth_; }
    void setVibratoRate(float rate) { currentVibratoRate_ = rate; }
    float getCurrentVibratoRate() const { return currentVibratoRate_; }
    bool getSingleSelectedNoteParameters(float& retuneSpeedPercent, float& vibratoDepth, float& vibratoRate) const;

    AudioEditingScheme::ParameterEditResult editParameter(AudioEditingScheme::ParameterId id, float value);
    // Accessibility: where the user is in the song while REAPER is paused or stopped
    // (project seconds), and the same in the clip's source seconds. Empty while playing
    // or in other hosts. See trackReaperTransport.
    struct SongPosition {
        enum class Source { Paused, EditCursor };
        double seconds = 0.0;
        Source source = Source::EditCursor;
    };
    std::optional<SongPosition> currentSongPosition() const;
    std::optional<double> stoppedEditCursorSourceTime() const;
    // Accessibility: what a parameter knob change did, for speech ("Applied to notes: 3").
    juce::String describeParameterEdit(AudioEditingScheme::ParameterId id,
                                       const AudioEditingScheme::ParameterEditResult& result) const;
    void setCreationDefault(AudioEditingScheme::ParameterId id, float value);

    NoteGeneratorParams getCurrentAutoTuneParams() const noexcept
    {
        NoteGeneratorParams params;
        params.policy = segmentationPolicy_;
        params.retuneSpeed = currentRetuneSpeed_;
        params.vibratoDepth = currentVibratoDepth_;
        params.vibratoRate = currentVibratoRate_;
        return params;
    }
    int findLineAnchorSegmentNear(int x, int y) const;
    void selectLineAnchorSegment(int idx);
    void toggleLineAnchorSegmentSelection(int idx);
    void clearLineAnchorSegmentSelection();

    double getContentDurationSeconds() const;
    std::pair<double, double> getSelectionTimeRange() const
    {
        return { std::min(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime),
                 std::max(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime) };
    }

    /** 设置单内容投影；返回投影是否有变化。 */
    bool setContentProjection(const ContentTimelineProjection& projection);
    void setTimelineContentPlacements(std::vector<TimelineContentPlacement> placements);

    /** 设置 reference overlay 数据（ghost notes + anchors）。
     *  传入 std::nullopt 清除 overlay。 */

    void setPlayheadColour(juce::Colour colour) {
        playheadColour_ = colour;
        repaint();
    }

    void fitToScreen();
    void fitToAllNotes();
    void fitToSelectedNotes();

    enum class AutoTuneApplyStatus
    {
        Applied,
        NoChange,   // 新增：最终修正已达成（无音符 / 无音阶配置 / 全部音符已吸附且修正曲线无空洞覆盖）
        NoCurve,
        NoContentCommands,
        NoContent,
        MissingContentSnapshot,
        OriginalF0NotReady,
        MissingCurveSnapshot,
        EmptyOriginalF0,
        EmptyTimeline,
        NoTargetSelection,
        EmptyTargetRange
    };

    struct AutoTuneApplyResult
    {
        AutoTuneApplyStatus status = AutoTuneApplyStatus::NoContent;

        bool applied() const noexcept { return status == AutoTuneApplyStatus::Applied; }
        juce::String message() const;
    };

    AutoTuneApplyResult applyAutoTuneToSelection();

    // Note Copy / Paste / Duplicate
    void copySelectedNotes();
    void pasteNotes();
    void duplicateNotes();

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    /** Request a semantic content redraw. */
    void requestContentRedraw();
    void requestThemeRedraw();

    // ── EQ 工具（per-note EQ） ──────────────────────────────────────
    /** 为当前选中组打开 EQ 预览弹窗：只读选中组与主音符，主音符 eq 有值显示它，
     *  无值显示 EqSettings 默认；打开零 draft、零提交。主音符 index 由
     *  ToolHandler 唯一判定并经 toolCtx.openEqPreview 直通。 */
    void openEqPopupForSelection(int primaryIndex, bool fromKeyboard = false);

    void scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) override;
    void updateScrollBars();
    bool handleViewportShortcut(const juce::KeyPress& key);

private:
    friend struct PianoRollComponentTestProbe;

    // ── 保留表面状态快照 ──────────────────────────────────────
    juce::Image staticSurface_;
    juce::Image labelsSurface_;
    juce::Image waveformSurface_;
    juce::Image contentSurface_;
    ViewportState surfaceView_;
    bool staticDirty_ = true;
    bool contentDirty_ = true;
    // 内容编辑与相机平移待补的物理像素区域，由同一次 paint 消费。
    juce::RectangleList<int> pendingContentRaster_;
    bool cameraPanPending_ = false;
    // 有未呈现的缩放输入：paint 消费后必须按当前相机全量栅格，
    // 截止已过（心跳耗尽后才 paint）同样成立
    bool zoomCameraPending_ = false;
    // 有效倍率（peer/host/root transform 真实绘制栈）；0 = 尚未从 paint 捕获
    float renderScale_ = 0.0f;

    // ── 缩放事务 ──────────────────────────────────────────────
    int zoomDeadlineTicks_ = 0;          // 心跳计数倒计时，0 表示事务结束
    static constexpr int kZoomDeadlineTicks = 10;  // ~400ms @ 25Hz 心跳

    // ── 表面管理 ──────────────────────────────────────────────
    void invalidateTimeAxisStaticSurface();
    void ensureSurfaceSizes();
    void rasterizeStatic(std::optional<juce::Rectangle<int>> dirtyRect = std::nullopt,
                         std::optional<juce::Rectangle<int>> physicalDirtyRect = std::nullopt);
    void rasterizeLabels();
    int getTimelineContentStartX() const noexcept;

    // ── 内容集合（一次 owner render pass 一份，content/waveform/overlay 共用） ──
    // 唯一构建点在 paint：全量/条带栅格与 overlay 消费同一集合
    using Item  = PianoRollRenderer::ContentRenderItem;
    using Items = PianoRollRenderer::ContentRenderItems;   // std::map<ContentKey, Item>
    Items buildContentRenderItems();
    std::optional<Item> buildContentRenderItem(ContentKey key) const;
    void rasterizeContentSurfaces(const Items& items);
    void rasterizeContent(const Items& items,
                          std::optional<juce::Rectangle<int>> dirtyRect = std::nullopt,
                          std::optional<juce::Rectangle<int>> physicalDirtyRect = std::nullopt);
    void rasterizeWaveform(const Items& items,
                           std::optional<juce::Rectangle<int>> dirtyRect = std::nullopt,
                           std::optional<juce::Rectangle<int>> physicalDirtyRect = std::nullopt);

    // ── 按坐标域拆分的绘制函数 ──
    void drawFixedChrome(juce::Graphics& g, juce::Rectangle<int> damage);
    void drawRuler(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage);
    void drawPitchBackground(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage);
    void drawPianoKeyboard(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage);
    enum class ContentLayer { Notes, Waveform };

    void drawContent(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage,
                     ContentLayer layer, const Items& items);

    // ── 性能探针 ──────────────────────────────────────────────
    struct RasterProbe { int count = 0; double totalMs = 0.0; };
    RasterProbe staticRasterProbe_;
    RasterProbe contentRasterProbe_;
    RasterProbe overlayPresentProbe_;
    RasterProbe rootPaintProbe_;
    RasterProbe vblankToRootPaintProbe_;
    RasterProbe zoomPreviewPaintProbe_;
    RasterProbe zoomCommitRasterProbe_;
    RasterProbe snapshotBuildProbe_;
    RasterProbe moveImageSectionProbe_;
    RasterProbe waveformQueryProbe_;
    double probeReportWindowStart_ = 0.0;
    double lastVBlankMs_ = 0.0;

    enum class RenderProbePoint { StaticRaster, ContentRaster, OverlayPresent, RootPaint, VBlankToRootPaint, ZoomPreviewPaint, ZoomCommitRaster, SnapshotBuild, MoveImageSection, WaveformQuery };
    void recordRenderProbe(RenderProbePoint point, double elapsedMs, int samples = 1);

    // ── 波形 mipmap 预热（心跳驱动，无 timer）────────────────
    // 事件置「预热欠债」：进入卷帘视图/相机激活（缩放、滚动提交）/音频内容变更。
    // 欠债存在期间心跳使用更大预算，优先构建当前可见采样范围在当前显示层级
    // 覆盖的区块；可见范围建完即还清欠债、恢复常驻预算。
    void requestWaveformPreheatForVisible();

    // ── 保留式相机更新 ────────────────────────────────────────
    // 满足增量条件时提交相机位移，返回波形横移暴露的物理像素范围；
    // exactCamera=true（缩放事务进行中或存在未呈现缩放输入）时，
    // 来源状态 surfaceView_ 与当前相机不同必须推进来源并全量重绘。
    juce::Rectangle<int> commitCameraDelta(bool exactCamera);
    /** 纵向滚动事件收口：同步滚动条并请求展示（表面提交由 paint 的 commitCameraDelta 完成）。 */
    void applyVerticalPan();
    void handleHorizontalScrollPixels(double pixelDelta);
    void handleVerticalScrollPixels(double pixelDelta);

    // ── 缩放事务 ──────────────────────────────────────────────
    void beginZoomPreviewAt(int anchorViewportX, double zoomFactor);
    void extendZoomPreview();
    void handleVerticalZoomAt(float zoomFactor, float contentY);
    void zoomHorizontalIn();
    void zoomHorizontalOut();
    void zoomVerticalIn();
    void zoomVerticalOut();
    void layoutZoomControls();
    void syncZoomControls();
    void applyHorizontalZoomSlider(double pps);
    void applyVerticalZoomSlider(double pst);

    // ── EQ popup ──────────────────────────────────────────────
    void closeEqPopup();
    void invalidateEqPopup();
    /** 将选中组统一写入同一 EqSettings，作为单次消息线程事务提交。 */
    void applyEqSettingsToSelection(const EqSettings& settings);
    /** 清除选中组全部 Note.eq（nullopt），作为单次消息线程事务提交。 */
    bool removeEqFromSelection();
    void handleVolumeEnvelopeRegionEditResult(const VolumeEnvelopeRegionEditResult& result);
    /** 预览尺寸 180x80 契约，在当前组件范围内靠近选择区放置。 */
    juce::Rectangle<int> placeEqPopupBounds(const juce::Rectangle<int>& anchor) const;

    bool tryConsumeInitialF0View(ContentKey contentKey);

    enum class VibratoParam { Depth, Rate };

    juce::ScrollBar verticalScrollBar_{ true };
    SmallButton scrollModeToggleButton_;
    SmallButton timeUnitToggleButton_;

    static constexpr float kMinPixelsPerSemitone = 5.0f;
    static constexpr float kMaxPixelsPerSemitone = 60.0f;

    juce::Slider horizontalZoomSlider_;
    juce::Slider verticalZoomSlider_;
    juce::Rectangle<int> zoomControlsArea_;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify(const juce::MouseEvent& e, float scaleFactor) override;
public:
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;

    /// Re-read notes from the content store, update the cache, and return the snapshot used.
    std::shared_ptr<const EditableContentSnapshot> refreshEditedContentNotes();

    /// Timeline viewport geometry in local coordinates.
    /// Shared with the Standalone PluginEditor for the overview strip layout.
    juce::Rectangle<int> getTimelineViewportBounds() const;

private:
    bool matchesShortcut(KeyShortcutConfig::ShortcutId id, const juce::KeyPress& key) const;
    bool handlePointerShortcut(const KeyShortcutConfig::KeyBinding& binding);

    void onScrollVBlankCallback(double timestampSec);
    int getTimelineContentViewportWidth() const;
    int getTimelineContentViewportHeight() const;

    ViewMapper makeViewMapper() const noexcept;
    ViewMapper makeViewMapperForView(const ViewportState& view) const noexcept;
    juce::Rectangle<int> timeAxisRect() const;

    // ── Overlay 绘制（owner paint 收尾层，drawOverlay 按原次序调度） ──
    /** overlay 本轮内容获取：构造于 paint 局部、按引用下传，禁止存为成员。 */
    struct OverlayPassItems {
        const Items* shared = nullptr;   // 本轮已构建集合（栅格路径），可为空=合法空
        const Item* sharedItem = nullptr; // 共享集合中活动 key 的缓存命中，避免重复解析
        std::optional<Item> activeItem;  // 惰性构建缓存（无共享集合路径）
        const TimelineContentPlacement* activePlacement = nullptr;
        bool activeBuilt = false;
    };
    void drawOverlay(juce::Graphics& g, OverlayPassItems& pass);
    /** 惰性取得活动内容：优先复用本轮共享集合，缺失时构建一次（成功记 SnapshotBuild
     *  样本 1，失败缓存空不重试）；item 无关元素（播放头线、琴键、橡皮圆、tooltip）零构建。 */
    std::optional<PianoRollRenderer::PlacedContent> activeContent(OverlayPassItems& pass);
    void drawPlayheadOverlay(juce::Graphics& g);
    void drawPlayheadNoteHighlight(juce::Graphics& g, OverlayPassItems& pass);
    void drawTransientOverlay(juce::Graphics& g, OverlayPassItems& pass);
    void drawHandDrawPreview(juce::Graphics& g, const PianoRollTimeMap& map,
                             const PianoRollRenderer::ContentRenderItem& item);
    void drawEraserPreview(juce::Graphics& g);
    void drawLineAnchorPreview(juce::Graphics& g, const PianoRollTimeMap& map);
    void drawSelectionBox(juce::Graphics& g, const PianoRollTimeMap& map, ThemeId themeId);
    void drawTimeGridHandles(juce::Graphics& g, OverlayPassItems& pass);
    void drawSelectedNoteHighlights(juce::Graphics& g, OverlayPassItems& pass);
    void drawF0SelectionHighlight(juce::Graphics& g, OverlayPassItems& pass);
    void drawPianoKeysPressed(juce::Graphics& g);
    // OpenDyne overlay
    void drawScissorsPreview(juce::Graphics& g, const PianoRollTimeMap& map);
    void drawModDriftDragPreview(juce::Graphics& g);
    void drawVolumeDragPreview(juce::Graphics& g);

    bool shouldShowPianoKeys() const noexcept;
    bool isTimeView() const noexcept { return currentTool_ == ToolId::TimeTool; }

    void handleVerticalZoomWheel(const juce::MouseEvent& e, float deltaY);
    void handleHorizontalScrollWheel(float deltaX, float deltaY);
    void handleVerticalScrollWheel(float deltaY);
    void handleHorizontalZoomWheel(const juce::MouseEvent& e, float deltaY);

    // OpenDyne drag zoom/pan
    void beginOpenDyneZoomPan(const juce::MouseEvent& e);
    void updateOpenDyneZoomPan(const juce::MouseEvent& e);
    void endOpenDyneZoomPan();

    TimelineViewportRequest makeViewportRequest(
        TimelineViewportRequest::Kind kind,
        double targetTime,
        double anchorViewportX,
        double pps) const;

    void initializeUIComponents();
    void initializeRenderer();
    void applyAudioEditingScheme(AudioEditingScheme::Scheme scheme);
    void stopNotePitchAudition();
    PianoRollToolHandler::Context buildToolHandlerContext();
    void initializeToolHandler();
    void applyEditedContentCurve(std::shared_ptr<const PitchCurveSnapshot> curve);
    void applyEditedContentAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, double sampleRate);
    void applyEditedContent(ContentKey contentKey,
                            std::shared_ptr<const PitchCurveSnapshot> curve,
                            std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                            double sampleRate,
                            bool activePlacementChanged,
                            bool deferRedraw);
    const std::vector<Note>& getCommittedNotes() const;
    const std::vector<Note>& getDisplayedNotes() const;
    NoteInteractionDraft& getNoteDraft();
    const NoteInteractionDraft& getNoteDraft() const;
    void beginNoteDraft();
    bool commitNoteDraft();
    void clearNoteDraft();
    ContentCommitSnapshot commitEditedContentNotesAndSegments(const EditableContentSnapshot& snapshot,
                                             const std::vector<Note>& notes,
                                             const std::vector<PitchCorrectionSegment>& segments,
                                             F0FrameRange affectedRange);
    AutoTuneApplyResult applyAutoSnapToAllNotes(
        const std::shared_ptr<const EditableContentSnapshot>& contentSnapshot);
    bool selectNotesOverlappingFrames(int startFrame, int endFrameExclusive);
    juce::Rectangle<int> getNoteBounds(const Note& note) const;
    juce::Rectangle<int> getNotesBounds(const std::vector<Note>& notes) const;
    juce::Rectangle<int> getHandDrawPreviewBounds() const;
    juce::Rectangle<int> getLineAnchorPreviewBounds() const;
    void invalidateLiveNotes(const std::vector<Note>& beforeNotes, const std::vector<Note>& afterNotes);
    void invalidateSelectionFeedback();
    void invalidateInteractionPreview(const juce::Rectangle<int>& bounds);
    void parameterTargetWillChange(Listener::ParameterTargetChangePolicy policy);
    void parameterTargetDidChange();

    float getTotalHeight() const;

    F0Timeline currentF0Timeline() const noexcept {
        if (currentCurve_ == nullptr || currentCurve_->size() == 0) return {};
        return { currentCurve_->getHopSize(), currentCurve_->getSampleRate(),
                 static_cast<int>(currentCurve_->size()) };
    }

    const TimelineContentPlacement* findEditedPlacement() const noexcept;
    double sourceTimeToTimelineTime(double sourceSeconds) const;
    int  sourceTimeToX(double sourceSeconds) const;
    double xToSourceTime(int x) const;
    SourceEditRange sourceEditRange() const;

private:
    TimelineViewportCamera camera_{0.0, TimelineViewportCamera::kDefaultPixelsPerSecond};
    float verticalScrollOffset_ = 0.0f;
    ScrollMode scrollMode_ = ScrollMode::Page;

    const PlayHeadState& playHeadState_;
    UndoManager& undoManager_;
    SpectrumReader spectrumReader_;
    bool lastObservedPlayHeadPlaying_{false};

    bool userScrollHold_{false};

    bool userHasManuallyZoomed_ = false;
    ZoomSensitivityConfig::ZoomSensitivitySettings zoomSensitivity_ = ZoomSensitivityConfig::ZoomSensitivitySettings::getDefault();
    AudioEditingScheme::Scheme audioEditingScheme_ = AudioEditingScheme::Scheme::CorrectedF0Primary;
    KeyShortcutConfig::KeyShortcutSettings shortcutSettings_ = KeyShortcutConfig::KeyShortcutSettings::getDefault();
    const KeyShortcutConfig::KeyBinding* pointerShortcutBinding_ = nullptr;
    juce::Colour trackDisplayColour_{0xFF4A90D9};

    int scaleRootNote_ = 0;
    int scaleType_ = 1;

    static constexpr float minMidi_ = 24.0f;
    static constexpr float maxMidi_ = 108.0f;
    float pixelsPerSemitone_ = 25.0f;

    ToolId currentTool_ = ToolId::Select;

    // OpenDyne 右键工具选择弹出条
    std::unique_ptr<juce::Component> toolSelectionBar_;
    void showToolSelectionBar(juce::Point<int> screenPos);
    void showToolMenuFromKeyboard();
    a11y::ApplicationsKeyWatcher applicationsKey_;
    void dismissToolPopup();

    InteractionState interactionState_;

    // OpenDyne navigation: Command+Alt+拖拽 缩放 事务
    bool openDyneZoomPanActive_ = false;
    juce::Point<int> openDyneZoomPanStartPos_;
    double openDyneZoomPanStartPps_ = 0.0;
    float openDyneZoomPanStartPixelsPerSemitone_ = 25.0f;
    double openDyneZoomPanAnchorTime_ = 0.0;
    float openDyneZoomPanAnchorMidi_ = 60.0f;
    AxisLockState openDyneZoomAxisLock_;

    // OpenDyne: Cmd+Alt+double-click → zoom to note / restore zoom
    std::optional<ViewportState> savedOpenDyneZoomState_;
    void saveOpenDyneZoomState();
    void restoreOpenDyneZoomState();
    void fitToNote(const Note& note);

    float dragStartVerticalScrollOffset_ = 0.0f;
    double dragStartVisibleStartSeconds_ = 0.0;

    // 手指手势（仅 TouchInput 判定为手指时启用；鼠标/触控板/Apple Pencil 走原有路径）：
    // 空白处拖动平移视图，双击空白处播放/暂停，双指捏合按手指连线方向分别缩放两轴。
    struct FingerGesture
    {
        enum class Mode { Idle, Forwarded, EmptyTap, Panning, Pinch, Ignore };

        Mode mode = Mode::Idle;
        std::map<int, juce::Point<float>> fingers;
        int primaryIndex = -1;
        std::optional<juce::MouseEvent> lastPrimaryEvent;

        juce::Point<float> panStartPos;
        double panStartVisibleSeconds = 0.0;
        float panStartScrollY = 0.0f;

        int pinchA = -1;
        int pinchB = -1;
        juce::Point<float> pinchStartSpan;
        bool pinchZoomsTime = false;
        bool pinchZoomsPitch = false;
        double pinchStartPps = 0.0;
        float pinchStartPixelsPerSemitone = 25.0f;
        double pinchAnchorTime = 0.0;
        float pinchAnchorMidi = 60.0f;

        bool suppressDoubleClick = false;
    };
    FingerGesture fingerGesture_;

    bool beginFingerPress(const juce::MouseEvent& e);
    bool continueFingerDrag(const juce::MouseEvent& e);
    bool endFingerPress(const juce::MouseEvent& e);
    void beginFingerPinch();
    void updateFingerPinch();
    void updateFingerPan(const juce::MouseEvent& e);
    void applyViewportFromGesture(double visibleStartSeconds, double pps, float scrollY);
    bool fingerDragOnEmptySpacePans() const noexcept;
    void dispatchMouseDown(const juce::MouseEvent& e);
    void dispatchMouseDrag(const juce::MouseEvent& e);
    void dispatchMouseUp(const juce::MouseEvent& e);

private:

    bool showWaveform_ = true;
    bool showPianoKeyboard_ = true;
    bool scaleAssistEnabled_ = false;
    PianoGridStyle gridStyle_ = PianoGridStyle::PianoLanes;
    NoteNameMode noteNameMode_ = NoteNameMode::COnly;
    bool showUnvoicedFrames_ = false;
    float backgroundBrightness_ = 1.0f;
    juce::Colour correctedF0Colour_{0xFF1D6FC0};
    juce::Colour originalF0Colour_{0xFFC91B1E};
    bool showOriginalF0_ = true;
    bool showCorrectedF0_ = true;
    float currentRetuneSpeed_ = PitchControlConfig::kDefaultRetuneSpeedNormalized;
    float currentPitchDriftScale_ = 1.0f;
    float currentVibratoDepth_ = PitchControlConfig::kDefaultVibratoDepth;
    float currentVibratoRate_ = PitchControlConfig::kDefaultVibratoRateHz;

    NoteSegmentationPolicy segmentationPolicy_;
    

    double bpm_ = 120.0;
    int timeSigNum_ = 4;
    int timeSigDenom_ = 4;
    TimelineDisplayMode displayMode_ = TimelineDisplayMode::Time;

    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer_;
    double audioBufferSampleRate_ = 0.0;

    std::vector<TimelineContentPlacement> timelineContentPlacements_;
    ContentTimelineProjection pendingSingleContentProjection_;
    bool explicitTimelineContentPlacements_ = false;

    AppPreferences* appPreferences_ = nullptr;

    ReadContentSnapshotFn readContentSnapshot_;
    std::shared_ptr<ContentEditCommands> contentCommands_;
    OpenTuneAudioProcessor* processor_ = nullptr;
    // Accessibility: REAPER transport as last seen (trackReaperTransport) and the last halt.
    void trackReaperTransport();
    int lastReaperPlayState_ = 0;
    std::optional<double> haltEditCursor_;   // edit cursor when playback paused or stopped

    std::shared_ptr<const EditableContentSnapshot> readSnapshotFor(ContentKey key) const {
        return readContentSnapshot_ ? readContentSnapshot_(key) : nullptr;
    }
    std::shared_ptr<const EditableContentSnapshot> readEditedSnapshot() const {
        return readSnapshotFor(editedContentKey_);
    }

    ContentKey editedContentKey_;
    ContentKey pendingInitialF0ViewContentKey_;
    bool experimentalFeaturesEnabled_ = false;
    std::vector<Note> cachedNotes_;
    std::vector<Note> notesClipboard_;   // Note Copy/Paste 剪贴板


    // Undo support
    juce::String pendingUndoDescription_;

    std::vector<PitchCorrectionSegment> getCurrentSegments() const;
    

    bool applyTimelineContentPlacements(std::vector<TimelineContentPlacement> placements,
                                                bool explicitContract);
    void deriveSingleTimelineContentPlacement();

    std::vector<Note> getEditedContentNotesCopy() const;

    std::shared_ptr<const PitchCurveSnapshot> currentCurve_;
    
    std::unique_ptr<PianoRollRenderer> renderer_;
    std::unique_ptr<PianoRollToolHandler> toolHandler_;
    mutable WaveformMipmapCache waveformMipmapCache_;
    // F0 投影点列设备无关缓存（快照/投影/scheme 键控，绘制 pass 末尾裁剪）
    F0VisualCacheStore f0VisualCache_;

    uint64_t lastKnownNotesRevision_ = 0;
    uint64_t lastKnownPitchRevision_ = 0;
    uint64_t lastKnownTimeGridRevision_ = 0;

    static constexpr int pianoKeyWidth_ = 60;
    static constexpr int rulerHeight_ = 30;
    static constexpr int timelineExtendedHitArea_ = 20;
    static constexpr int dragThreshold_ = 5;
    
    PianoKeyAudition* pianoKeyAudition_ = nullptr;
    NoteAudition* noteAudition_ = nullptr;

    // Accessibility: per-note UIA children and speech for the keyboard-selected note.
    class NoteAccessibilityItem;
    std::vector<std::unique_ptr<NoteAccessibilityItem>> noteAccessibilityItems_;
    void syncNoteAccessibilityItems();
    void clearNoteAccessibilityItems();
    void handleKeyboardNote(PianoRollToolHandler::KeyboardNoteEvent event, const std::vector<Note>& notes, int index);
    enum class NoteDetail { Navigation, Target, Full };
    juce::String describeKeyboardNote(const std::vector<Note>& notes, int index, NoteDetail detail) const;
    /** @param renderCacheToOverlay settled canonical render to overlay; when null the
     *  best available cache is used for navigation audition. */
    void auditionNote(const Note& note, const RenderCache* renderCacheToOverlay = nullptr);
    juce::String keyboardToolName(ToolId tool) const;
    juce::String modulationPercentText(const Note& note) const;
    juce::String driftPercentText(const Note& note) const;
    juce::String volumeText(const Note& note) const;
    juce::String describeEditedSelection(const std::vector<Note>& notes, int index,
                                         const std::function<juce::String(int)>& valueAt) const;
    void scheduleAuditionAfterRender(int noteIndex);
    void playPendingAudition(bool renderFailed);
    void cancelPendingAudition();
    void handleRenderTerminal(ContentKey key, bool completed);
    void announceForAccessibility(const juce::String& text,
                                  a11y::SpeechQueue queue = a11y::SpeechQueue::replacePrevious);
    bool accessibilityModeEnabled() const noexcept;
    bool escapePassesToHost_ = false;
    RenderCacheProvider renderCacheProvider_;
    ContentKey pendingAuditionContentKey_;
    int pendingAuditionNoteIndex_ = -1;
    bool accessibilityModeApplied_ = false;
    int pressedPianoKey_ = -1;

    std::unique_ptr<juce::VBlankAttachment> scrollVBlankAttachment_;

    // OpenDyne：双击滚动条 → 缩放到全部音符
    std::unique_ptr<juce::MouseListener> fitToAllNotesOnDoubleClick_;

    // per-note EQ 预览弹窗：PianoRollComponent 唯一持有，不经过任何中间转发层
    std::unique_ptr<EqPopupComponent> eqPopup_;
    std::vector<int> eqEditTargetIndices_; // EQ 弹窗绑定的目标音符索引快照
    uint64_t eqEditRevision_ = 0;

    // EQ 工具 cursor：构造函数初始化列表中从 ToolbarIcons::createEqIconImage 构造一次并 resolveCursor。
    juce::MouseCursor eqCursor_;

    double playheadTimeForPaint_ = 0.0;
    double pendingSeekTime_ = -1.0;  // -1.0 sentinel: no pending seek
    double pendingSeekIssuedAtSeconds_ = 0.0;
    double pendingSeekHostTimeAtGesture_ = 0.0;
    uint64_t pendingSeekEpoch_ = 0;
    bool pendingSeekHostBound_ = false;
    bool pendingSeekHostWasPlaying_ = false;
    bool transportControlsEnabled_ = true;
    bool trackCaptureTint_ = false;

    juce::ListenerList<Listener> listeners_;
    
    juce::Colour playheadColour_{0xFFE74C3C};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};

} // namespace OpenTune
