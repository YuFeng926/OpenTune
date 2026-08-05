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
 * 渲染架构：两张保留 Image + 一层透明 Overlay
 * - staticSurface_  ：主题背景、标尺、lane、网格、琴键（由 drawFixedChrome/drawRuler/drawPitchBackground/drawPianoKeyboard 共享）
 * - contentSurface_ ：波形、无声帧、notes、F0、TimeGrid 锚点、ghost（由 drawContent 共享）
 * - overlay_        ：播放头、选中高亮、框选、绘制预览、TimeGrid 把手
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
#include "Utils/ZoomSensitivityConfig.h"
#include <cmath>
#include <memory>
#include <algorithm>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <optional>
#include <utility>
#include <atomic>
#include <array>
#include "SmallButton.h"
#include "PianoRoll/PianoRollRenderer.h"

#include "PianoRoll/PianoRollToolHandler.h"
#include "PianoRoll/InteractionState.h"
#include "TimelineViewportCamera.h"
#include "TimelineViewportPolicy.h"
#include "WaveformMipmap.h"
#include "../../Utils/UndoManager.h"
#include "../../Content/ContentEditCommands.h"
#include "../../Utils/TimelineDisplayMode.h"
namespace OpenTune {

class OpenTuneAudioProcessor;
class PianoKeyAudition;
struct PlayHeadState;

struct PianoRollComponentTestProbe;

// ============================================================================
// PianoRollOverlayComponent — 透明覆盖层，绘制所有交互/动态元素
// ============================================================================

class PianoRollComponent; // fwd

class PianoRollOverlayComponent : public juce::Component {
public:
    explicit PianoRollOverlayComponent(PianoRollComponent& owner);
    void paint(juce::Graphics& g) override;
private:
    PianoRollComponent& owner_;
};

// ============================================================================
// PianoRollComponent — piano roll editor with retained-image rendering.
// ============================================================================

class PianoRollComponent : public juce::Component,
                           public juce::ScrollBar::Listener {
public:
    void visibilityChanged() override;
    static constexpr int kAudioSampleRate = 44100;

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual bool playheadPositionChangeRequested(double timeSeconds) = 0;
        virtual void pitchCurveEdited(int startFrame, int endFrame) { (void)startFrame; (void)endFrame; }
        virtual void noteOffsetChanged(size_t noteIndex, float oldOffset, float newOffset) { (void)noteIndex; (void)oldOffset; (void)newOffset; }
        virtual void contentEdited() {}
        virtual void autoTuneRequested() {}
        virtual void escapeKeyPressed() {}
        virtual void undoRequested() {}
        virtual void redoRequested() {}
        virtual void currentToolChanged(ToolId tool) { (void)tool; }
        virtual void timelineDisplayModeChanged(TimelineDisplayMode mode) { (void)mode; }
    };

    enum class ScrollMode
    {
        Page,
        Continuous
    };

    using TimelineContentPlacement = OpenTune::TimelineContentPlacement;

    PianoRollComponent(const PlayHeadState& playHeadState);
    ~PianoRollComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void onHeartbeatTick();

    void setEditedContent(ContentKey contentKey,
                           std::shared_ptr<PitchCurve> curve,
                           std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                           int sampleRate);
    void requestInitialF0View(ContentKey contentKey);
    void onTimeGridRevisionChanged();
    void onNotesRevisionChanged();
    void onPitchRevisionChanged();
    void setPianoKeyAudition(PianoKeyAudition* audition) { pianoKeyAudition_ = audition; }

    void setProcessor(OpenTuneAudioProcessor* processor);

    using ReadContentSnapshotFn = std::function<std::shared_ptr<const EditableContentSnapshot>(ContentKey)>;
    void setReadContentSnapshot(ReadContentSnapshotFn fn) { readContentSnapshot_ = std::move(fn); }

    void setContentCommands(std::shared_ptr<ContentEditCommands> commands);

    ContentKey editedContentKey() const { return editedContentKey_; }
    ContentTimelineProjection activeContentProjection() const noexcept;
    WaveformMipmapCache& getWaveformMipmapCache() noexcept { return waveformMipmapCache_; }

    /** [ARA 重构] 注入域内容所有者（替代 setContentProviders）。统一 ARA/Standalone/Capture 路径。 */

    void commitViewportRequest(TimelineViewportRequest req);
    int timelinePolicyViewportWidth() const noexcept { return getTimelineContentViewportWidth(); }
    TimelineViewportCamera timelineCamera() const noexcept { return camera_; }
    void activateTimelineCamera(TimelineViewportCamera camera);
    void setCurrentTool(ToolId tool);
    void setExperimentalFeaturesEnabled(bool enabled);
    ToolId getCurrentTool() const { return currentTool_; }
    PianoRollToolHandler* getToolHandler() const { return toolHandler_.get(); }
    void setShowWaveform(bool shouldShow);
    void setShowLanes(bool shouldShow);
    void setNoteNameMode(NoteNameMode noteNameMode);
    void setShowUnvoicedFrames(bool shouldShow);
    void setInferenceActive(bool active);
    void setBpm(double bpm);
    void setTimeSignature(int numerator, int denominator);
    void setTimelineDisplayMode(TimelineDisplayMode mode);
    TimelineDisplayMode getTimelineDisplayMode() const { return displayMode_; }
    void setScrollMode(ScrollMode mode) {
        if (scrollMode_ == mode) return;
        scrollMode_ = mode;
        overlay_->repaint();
    }
    ScrollMode getScrollMode() const { return scrollMode_; }
    void setScale(int rootNote, int scaleType);
    void setAudioEditingScheme(AudioEditingScheme::Scheme scheme) { applyAudioEditingScheme(scheme); }
    bool isOpenDyne() const noexcept { return AudioEditingScheme::usesNotesPrimaryScheme(audioEditingScheme_); }

    /** 轨道主题色注入：OpenDyne waveform blob 的填充/描边色。 */
    void setTrackDisplayColour(juce::Colour colour);
    void setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& settings) { zoomSensitivity_ = settings; }
    void setShortcutSettings(const KeyShortcutConfig::KeyShortcutSettings& settings) { shortcutSettings_ = settings; }

    void resetUserZoomFlag() { userHasManuallyZoomed_ = false; }
    bool hasUserManuallyZoomed() const { return userHasManuallyZoomed_; }

    void setShowOriginalF0(bool show);
    void setShowCorrectedF0(bool show);
    bool isShowingOriginalF0() const { return showOriginalF0_; }

    void setRetuneSpeed(float speed) { currentRetuneSpeed_ = speed; }
    float getCurrentRetuneSpeed() const { return currentRetuneSpeed_; }
    bool applyRetuneSpeedToSelection(float speed);
    void setVibratoDepth(float depth) { currentVibratoDepth_ = depth; }
    float getCurrentVibratoDepth() const { return currentVibratoDepth_; }
    bool applyVibratoDepthToSelection(float depth);
    void setVibratoRate(float rate) { currentVibratoRate_ = rate; }
    float getCurrentVibratoRate() const { return currentVibratoRate_; }
    bool applyVibratoRateToSelection(float rate);
    bool getSingleSelectedNoteParameters(float& retuneSpeedPercent, float& vibratoDepth, float& vibratoRate) const;

    // 手动 AUTO / OpenDyne 导入 AUTO 共用的 canonical 参数源：
    // segmentationPolicy_ + currentRetuneSpeed_/currentVibratoDepth_/currentVibratoRate_。
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
    void setNoteSplit(float value);

    bool applyCorrectionToEntireClip(float retuneSpeed, float vibratoDepth, float vibratoRate);
    double getContentDurationSeconds() const;
    std::pair<double, double> getSelectionTimeRange() const
    {
        return { std::min(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime),
                 std::max(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime) };
    }

    void setContentProjection(const ContentTimelineProjection& projection);
    void setTimelineContentPlacements(std::vector<TimelineContentPlacement> placements);

    /** 设置 reference overlay 数据（ghost notes + anchors）。
     *  传入 std::nullopt 清除 overlay。 */
    void setReferenceOverlay(std::optional<PianoRollRenderer::ReferenceOverlay> overlay);

    void setPlayheadColour(juce::Colour colour) {
        playheadColour_ = colour;
        overlay_->repaint();
    }

    void fitToScreen();
    void fitToAllNotes();
    void fitToSelectedNotes();

    enum class AutoTuneApplyStatus
    {
        Applied,
        NoCurve,
        NoProcessor,
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

    void scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) override;
    void updateScrollBars();

private:
    friend struct PianoRollComponentTestProbe;
    friend class PianoRollOverlayComponent;

    // ── 保留表面状态快照 ──────────────────────────────────────
    struct ViewState {
        TimelineViewportCamera camera{0.0, TimelineViewportCamera::kDefaultPixelsPerSecond};
        float pixelsPerSemitone = 25.0f;
        float verticalScrollOffset = 0.0f;
    };
    juce::Image staticSurface_;
    juce::Image contentSurface_;
    ViewState surfaceView_;
    bool staticDirty_ = true;
    bool contentDirty_ = true;

    // ── 缩放事务 ──────────────────────────────────────────────
    bool zoomPreviewActive_ = false;
    double zoomAnchorTime_ = -1.0;   // -1.0 sentinel: invalid until beginZoomPreview sets it
    int zoomAnchorViewportX_ = 0;
    int zoomDeadlineTicks_ = 0;          // 心跳计数倒计时，0 表示事务结束
    static constexpr int kZoomDeadlineTicks = 10;  // ~400ms @ 25Hz 心跳

    // ── 表面管理 ──────────────────────────────────────────────
    void invalidateTimeAxisStaticSurface();
    void rasterizeDirtySurfaces();
    void rasterizeStatic(std::optional<juce::Rectangle<int>> dirtyRect = std::nullopt);
    void rasterizeContent(std::optional<juce::Rectangle<int>> dirtyRect = std::nullopt);

    // ── 按坐标域拆分的唯一绘制函数（raster target 与 preview target 共用） ──
    void drawFixedChrome(juce::Graphics& g, juce::Rectangle<int> damage);
    void drawRuler(juce::Graphics& g, const ViewState& view, juce::Rectangle<int> damage);
    void drawPitchBackground(juce::Graphics& g, const ViewState& view, juce::Rectangle<int> damage);
    void drawPianoKeyboard(juce::Graphics& g, const ViewState& view, juce::Rectangle<int> damage);
    void drawContent(juce::Graphics& g, const ViewState& view, juce::Rectangle<int> damage);

    // ── 性能探针 ──────────────────────────────────────────────
    struct RasterProbe { int count = 0; double totalMs = 0.0; };
    RasterProbe staticRasterProbe_;
    RasterProbe contentRasterProbe_;
    RasterProbe overlayPresentProbe_;
    RasterProbe rootPaintProbe_;
    RasterProbe vblankToRootPaintProbe_;
    RasterProbe zoomPreviewPaintProbe_;
    RasterProbe zoomCommitRasterProbe_;
    double probeReportWindowStart_ = 0.0;
    double lastVBlankMs_ = 0.0;

    enum class RenderProbePoint { StaticRaster, ContentRaster, OverlayPresent, RootPaint, VBlankToRootPaint, ZoomPreviewPaint, ZoomCommitRaster };
    void recordRenderProbe(RenderProbePoint point, double elapsedMs);

    // ── 保留式相机更新 ────────────────────────────────────────
    void applyRasterCamera(const TimelineViewportCamera& newCamera);

    // ── 缩放事务 ──────────────────────────────────────────────
    void beginZoomPreview(const juce::MouseEvent& e, float deltaY);
    void updateZoomPreview(float deltaY);
    void endZoomPreview();

    // ── 内容构建（供 rasterize + overlay 共用） ────────────────
    std::vector<PianoRollRenderer::ContentRenderItem> buildContentRenderItems() const;

    bool tryConsumeInitialF0View(ContentKey contentKey);

    bool applyManualCorrectionPatch(const std::vector<PianoRollToolHandler::ManualCorrectionOp>& ops,
                                    int dirtyStartFrame,
                                    int dirtyEndFrame,
                                    bool triggerRenderEvent);

    enum class VibratoParam { Depth, Rate };

    bool applyNoteParameterToSelectedNotes(float retuneSpeed, float vibratoDepth, float vibratoRate);
    bool applyParameterToFrameRange(float retuneSpeed, float vibratoDepth, float vibratoRate, int startFrame, int endFrameExclusive);
    bool getFrameRangeForTimeSpan(double startTime, double endTime, int& startFrame, int& endFrameExclusive) const;
    bool getSelectedNotesFrameRange(int& startFrame, int& endFrameExclusive) const;
    void syncF0SelectionToSelectedNotes();
    bool getSelectionAreaFrameRange(int& startFrame, int& endFrameExclusive) const;
    bool getF0SelectionFrameRange(int& startFrame, int& endFrameExclusive) const;

    juce::ScrollBar horizontalScrollBar_{ false };
    juce::ScrollBar verticalScrollBar_{ true };
    SmallButton scrollModeToggleButton_;
    SmallButton timeUnitToggleButton_;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
public:
    bool keyPressed(const juce::KeyPress& key) override;

    /// Re-read notes from the content store and update the cache.
    void refreshEditedContentNotes();

private:
    void onScrollVBlankCallback(double timestampSec);
    juce::Rectangle<int> getTimelineViewportBounds() const;
    int getTimelineContentViewportWidth() const;
    int getTimelineContentViewportHeight() const;

    ViewMapper makeViewMapper() const noexcept;
    ViewMapper makeViewMapperForView(const ViewState& view) const noexcept;
    double computeContentTimelineEndSeconds() const noexcept;
    juce::Rectangle<int> timeAxisRect() const;

    // ── Overlay 绘制委托（由 PianoRollOverlayComponent 调用） ──
    void drawPlayheadOverlay(juce::Graphics& g);
    void drawTransientOverlay(juce::Graphics& g);
    void drawNoteDragCurvePreview(juce::Graphics& g);
    void drawHandDrawPreview(juce::Graphics& g);
    void drawLineAnchorPreview(juce::Graphics& g);
    void drawSelectionBox(juce::Graphics& g, ThemeId themeId);
    void drawTimeGridHandles(juce::Graphics& g);
    void drawSelectedNoteHighlights(juce::Graphics& g);
    void drawF0SelectionHighlight(juce::Graphics& g);
    void drawPianoKeysPressed(juce::Graphics& g);
    // OpenDyne overlay
    void drawScissorsPreview(juce::Graphics& g);
    void drawModDriftDragPreview(juce::Graphics& g);

    bool shouldShowPianoKeys() const noexcept;
    bool isTimeView() const noexcept { return currentTool_ == ToolId::TimeTool; }

    void handleVerticalZoomWheel(const juce::MouseEvent& e, float deltaY);
    void handleHorizontalScrollWheel(float deltaX, float deltaY);
    void handleVerticalScrollWheel(float deltaY);
    void handleHorizontalZoomWheel(const juce::MouseEvent& e, float deltaY);

    // OpenDyne Melodyne-style navigation
    void handleOpenDyneHorizontalScrollWheel(float deltaX, float deltaY);
    void handleOpenDyneZoomAtMouse(const juce::MouseEvent& e, float deltaY);
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
    PianoRollToolHandler::Context buildToolHandlerContext();
    void initializeToolHandler();
    void applyEditedContentCurve(std::shared_ptr<PitchCurve> curve);
    void applyEditedContentAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, int sampleRate);
    std::optional<PianoRollRenderer::ContentRenderItem> buildContentRenderItem(
        const TimelineContentPlacement& placement) const;
    const std::vector<Note>& getCommittedNotes() const;
    const std::vector<Note>& getDisplayedNotes() const;
    NoteInteractionDraft& getNoteDraft();
    const NoteInteractionDraft& getNoteDraft() const;
    void beginNoteDraft();
    bool commitNoteDraft();
    void clearNoteDraft();
    ContentCommitSnapshot commitEditedContentNotesAndSegments(const std::vector<Note>& notes,
                                             const std::vector<PitchCorrectionSegment>& segments,
                                             F0FrameRange affectedRange);
    ContentCommitSnapshot commitEditedContentPitchCorrectionSegments(const std::vector<PitchCorrectionSegment>& segments,
                                                       F0FrameRange affectedRange);
    bool selectNotesOverlappingFrames(int startFrame, int endFrameExclusive);
    juce::Rectangle<int> getNoteBounds(const Note& note) const;
    juce::Rectangle<int> getNotesBounds(const std::vector<Note>& notes) const;
    juce::Rectangle<int> getSelectionBounds() const;
    juce::Rectangle<int> getHandDrawPreviewBounds() const;
    juce::Rectangle<int> getLineAnchorPreviewBounds() const;
    juce::Rectangle<int> getNoteDragCurvePreviewBounds() const;
    void invalidateLiveNotes(const std::vector<Note>& beforeNotes, const std::vector<Note>& afterNotes);
    void invalidateSelectionFeedback();
    void invalidateInteractionPreview(const juce::Rectangle<int>& bounds);

    float getTotalHeight() const;

    F0Timeline currentF0Timeline() const noexcept {
        if (currentCurve_ == nullptr) return {};
        auto snap = currentCurve_->getSnapshot();
        if (snap == nullptr || snap->size() == 0) return {};
        return { snap->getHopSize(), snap->getSampleRate(), static_cast<int>(snap->size()) };
    }

    const TimelineContentPlacement* findEditedPlacement() const noexcept;
    double sourceTimeToTimelineTime(double sourceSeconds) const;
    int  sourceTimeToX(double sourceSeconds) const;
    double xToSourceTime(int x) const;
    SourceEditRange sourceEditRange() const;

private:
    TimelineViewportCamera camera_{0.0, TimelineViewportCamera::kDefaultPixelsPerSecond};
    float verticalScrollOffset_ = 0.0f;
    ScrollMode scrollMode_ = ScrollMode::Continuous;

    const PlayHeadState& playHeadState_;
    bool lastObservedPlayHeadPlaying_{false};

    bool userScrollHold_{false};

    bool userHasManuallyZoomed_ = false;
    ZoomSensitivityConfig::ZoomSensitivitySettings zoomSensitivity_ = ZoomSensitivityConfig::ZoomSensitivitySettings::getDefault();
    AudioEditingScheme::Scheme audioEditingScheme_ = AudioEditingScheme::Scheme::CorrectedF0Primary;
    KeyShortcutConfig::KeyShortcutSettings shortcutSettings_ = KeyShortcutConfig::KeyShortcutSettings::getDefault();
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
    void dismissToolPopup();

    InteractionState interactionState_;

    // OpenDyne navigation: Command+Alt+拖拽 缩放 事务
    bool openDyneZoomPanActive_ = false;
    juce::Point<int> openDyneZoomPanStartPos_;
    double openDyneZoomPanStartPps_ = 0.0;
    float openDyneZoomPanStartPixelsPerSemitone_ = 25.0f;
    double openDyneZoomPanAnchorTime_ = 0.0;
    float openDyneZoomPanAnchorMidi_ = 60.0f;

    // OpenDyne: Cmd+Alt+double-click → zoom to note / restore zoom
    std::optional<ViewState> savedOpenDyneZoomState_;
    void saveOpenDyneZoomState();
    void restoreOpenDyneZoomState();
    void fitToNote(const Note& note);

    float dragStartVerticalScrollOffset_ = 0.0f;

    float recalculatePIP(Note& note);

private:

    bool showWaveform_ = true;
    bool showLanes_ = true;
    NoteNameMode noteNameMode_ = NoteNameMode::COnly;
    bool showUnvoicedFrames_ = false;
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
    double audioBufferSampleRate_ = static_cast<double>(kAudioSampleRate);

    std::vector<TimelineContentPlacement> timelineContentPlacements_;
    ContentTimelineProjection pendingSingleContentProjection_;
    bool explicitTimelineContentPlacements_ = false;
    bool inferenceActive_ = false;
    int waveformBuildTickCounter_ = 0;

    OpenTuneAudioProcessor* processor_ = nullptr;

    ReadContentSnapshotFn readContentSnapshot_;
    std::shared_ptr<ContentEditCommands> contentCommands_;

    std::shared_ptr<const EditableContentSnapshot> readSnapshotFor(ContentKey key) const {
        return readContentSnapshot_ ? readContentSnapshot_(key) : nullptr;
    }
    std::shared_ptr<const EditableContentSnapshot> readEditedSnapshot() const {
        return readSnapshotFor(editedContentKey_);
    }

    ContentKey editedContentKey_;
    std::set<ContentKey> pendingInitialF0ViewRequests_;
    bool experimentalFeaturesEnabled_ = false;
    std::vector<Note> cachedNotes_;
    std::vector<Note> notesClipboard_;   // Note Copy/Paste 剪贴板

    std::optional<PianoRollRenderer::ReferenceOverlay> referenceOverlay_;

    // Undo support
    juce::String pendingUndoDescription_;
    std::vector<Note> beforeUndoNotes_;
    std::vector<PitchCorrectionSegment> beforeUndoSegments_;
    bool undoSnapshotCaptured_{false};
    void captureBeforeUndoSnapshot();
    void recordUndoAction(const juce::String& description, F0FrameRange affectedRange);

    std::vector<PitchCorrectionSegment> getCurrentSegments() const;
    
    bool applyVibratoParameterToSelection(VibratoParam param, float value);
    

    bool applyTimelineContentPlacements(std::vector<TimelineContentPlacement> placements,
                                                bool explicitContract);
    void deriveSingleTimelineContentPlacement();

    std::vector<Note> getEditedContentNotesCopy() const;

    std::shared_ptr<PitchCurve> currentCurve_;
    
    std::unique_ptr<PianoRollRenderer> renderer_;
    std::unique_ptr<PianoRollToolHandler> toolHandler_;
    mutable WaveformMipmapCache waveformMipmapCache_;

    uint64_t lastKnownNotesRevision_ = 0;
    uint64_t lastKnownPitchRevision_ = 0;
    uint64_t lastKnownTimeGridRevision_ = 0;

    static constexpr int pianoKeyWidth_ = 60;
    static constexpr int rulerHeight_ = 30;
    static constexpr int timelineExtendedHitArea_ = 20;
    static constexpr int dragThreshold_ = 5;
    
    PianoKeyAudition* pianoKeyAudition_ = nullptr;
    int pressedPianoKey_ = -1;

    int64_t lastDpiMilli_ = 1000;

    std::unique_ptr<juce::VBlankAttachment> scrollVBlankAttachment_;
    std::unique_ptr<PianoRollOverlayComponent> overlay_;

    // OpenDyne：双击滚动条 → 缩放到全部音符
    std::unique_ptr<juce::MouseListener> fitToAllNotesOnDoubleClick_;

    double playheadTimeForPaint_ = 0.0;
    double pendingSeekTime_{-1.0};
    uint64_t seekSentRevision_{0};

    juce::ListenerList<Listener> listeners_;
    
    juce::Colour playheadColour_{0xFFE74C3C};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};

} // namespace OpenTune
