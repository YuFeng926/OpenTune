#pragma once

/**
 * 钢琴卷帘组件
 * 
 * 显示和编辑音高曲线、音符序列的组件，支持：
 * - F0 曲线显示（原始音高和校正后音高）
 * - 音符绘制和编辑
 * - 多种工具（选择、绘制、音高线锚点等）
 * - 缩放和滚动
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
#include "Utils/LegacyNoteGenerator.h"
#include "Utils/PitchControlConfig.h"
#include "Utils/SilentGapDetector.h"
#include "Utils/KeyShortcutConfig.h"
#include "Utils/ZoomSensitivityConfig.h"
#include <cmath>
#include <memory>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <optional>
#include <utility>
#include <atomic>
#include <thread>
#include "SmallButton.h"
#include "PlayheadOverlayComponent.h"
#include "PianoRoll/PianoRollRenderer.h"
#include "PianoRoll/PianoRollSurfaceCache.h"

#include "PianoRoll/PianoRollToolHandler.h"
#include "PianoRoll/PianoRollVisualInvalidation.h"
#include "PianoRoll/PianoRollCorrectionWorker.h"
#include "PianoRoll/InteractionState.h"
#include "TimelineViewportCamera.h"
#include "WaveformMipmap.h"
#include "../../Utils/UndoManager.h"
#include "../../Content/ContentEditCommands.h"

namespace OpenTune {

class OpenTuneAudioProcessor;
class PianoKeyAudition;

struct PianoRollComponentTestProbe;

// ============================================================================
// Preview Overlay — lightweight child that draws transient interaction previews
// (note-draw rectangle, hand-draw F0, line-anchor, selection box) without
// triggering the expensive main render-model rebuild.
// ============================================================================
class PianoRollPreviewOverlay : public juce::Component
{
public:
    explicit PianoRollPreviewOverlay(class PianoRollComponent& owner)
        : owner_(owner)
    {
        setOpaque(false);
        setInterceptsMouseClicks(false, false);
    }

    void paint(juce::Graphics& g) override;

private:
    PianoRollComponent& owner_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollPreviewOverlay)
};

class PianoRollComponent : public juce::Component,
                           public juce::ScrollBar::Listener {
public:
    void visibilityChanged() override;
    static constexpr int kAudioSampleRate = 44100;

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void playheadPositionChangeRequested(double timeSeconds) = 0;
        virtual void playPauseToggleRequested() = 0;
        virtual void stopPlaybackRequested() = 0;
        virtual void pitchCurveEdited(int startFrame, int endFrame) { (void)startFrame; (void)endFrame; }
        virtual void noteOffsetChanged(size_t noteIndex, float oldOffset, float newOffset) { (void)noteIndex; (void)oldOffset; (void)newOffset; }
        virtual void autoTuneRequested() {}
        virtual void escapeKeyPressed() {}
        virtual void undoRequested() {}
        virtual void redoRequested() {}
        virtual void currentToolChanged(ToolId tool) { (void)tool; }
        // Timeline viewport camera上报 - 两个视图共享同一时间窗口
        virtual void timelineViewportChanged(TimelineViewportCamera camera) { (void)camera; }
    };

    enum class TimeUnit
    {
        Seconds,
        Bars
    };

    enum class ScrollMode
    {
        Page,
        Continuous
    };

    using TimelineContentPlacement = OpenTune::TimelineContentPlacement;

    PianoRollComponent();
    ~PianoRollComponent() override;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    void onHeartbeatTick();

    void setEditedContent(ContentKey contentKey,
                           std::shared_ptr<PitchCurve> curve,
                           std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                           int sampleRate);
    void onTimeGridRevisionChanged();
    void onNotesRevisionChanged();
    void onPitchRevisionChanged();
    void setPianoKeyAudition(PianoKeyAudition* audition) { pianoKeyAudition_ = audition; }
    int getPressedPianoKey() const { return pressedPianoKey_; }

    void setProcessor(OpenTuneAudioProcessor* processor);

    /** Phase 4: Inject read callback that takes ContentKey → EditableContentSnapshot. */
    using ReadContentSnapshotFn = std::function<std::shared_ptr<const EditableContentSnapshot>(ContentKey)>;
    void setReadContentSnapshot(ReadContentSnapshotFn fn) { readContentSnapshot_ = std::move(fn); }

    /** Inject content commands (Phase 4: ContentEditCommands, write path with ContentKey). */
    void setContentCommands(std::shared_ptr<ContentEditCommands> commands);

    void setEditedContentKey(ContentKey key) { editedContentKey_ = key; }
    ContentKey editedContentKey() const { return editedContentKey_; }

    /** [ARA 重构] 注入域内容所有者（替代 setContentProviders）。统一 ARA/Standalone/Capture 路径。 */

    void setIsPlaying(bool playing) {
        bool stateChanged = (isPlaying_.load(std::memory_order_relaxed) != playing);
        isPlaying_.store(playing, std::memory_order_relaxed);
        if (stateChanged && playing)
            pendingSeekTime_ = -1.0;
        if (stateChanged) {
            lastObservedRawPlayheadTime_ = readPlayheadTime();
            resetPresentationClock(readPlayheadTime());
            userScrollHold_ = false;
            updatePlayheadPresentationPolicy();
        }
    }
    void setTimelineViewport(TimelineViewportCamera camera, juce::NotificationType notify);
    void focusActiveContentForRegionSwitch(const std::vector<SilentGap>& silentGaps,
                                           juce::NotificationType notify);
    void setCurrentTool(ToolId tool);
    void setExperimentalFeaturesEnabled(bool enabled);
    ToolId getCurrentTool() const { return currentTool_; }
    void setShowWaveform(bool shouldShow);
    void setShowLanes(bool shouldShow);
    void setNoteNameMode(NoteNameMode noteNameMode);
    void setShowChunkBoundaries(bool shouldShow);
    void setShowUnvoicedFrames(bool shouldShow);
    void setInferenceActive(bool active);
    void setBpm(double bpm);
    void setTimeSignature(int numerator, int denominator);
    void setTimeUnit(TimeUnit unit);
    TimeUnit getTimeUnit() const { return timeUnit_; }
    void setScrollMode(ScrollMode mode) {
        if (scrollMode_ == mode) {
            return;
        }

        scrollMode_ = mode;
        updatePlayheadPresentationPolicy();
        invalidateVisual(static_cast<uint32_t>(PianoRollVisualInvalidationReason::Viewport),
                         PianoRollVisualInvalidationPriority::Interactive);
    }
    ScrollMode getScrollMode() const { return scrollMode_; }
    void setScale(int rootNote, int scaleType);
    void setAudioEditingScheme(AudioEditingScheme::Scheme scheme) { audioEditingScheme_ = scheme; }
    void setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& settings) { zoomSensitivity_ = settings; }
    void setShortcutSettings(const KeyShortcutConfig::KeyShortcutSettings& settings) { shortcutSettings_ = settings; }

    void resetUserZoomFlag() { userHasManuallyZoomed_ = false; }
    bool hasUserManuallyZoomed() const { return userHasManuallyZoomed_; }

    void setShowOriginalF0(bool show) {
        if (showOriginalF0_ == show) return;
        showOriginalF0_ = show;
        invalidateVisual(static_cast<uint32_t>(PianoRollVisualInvalidationReason::Content));
    }
    void setShowCorrectedF0(bool show) {
        if (showCorrectedF0_ == show) return;
        showCorrectedF0_ = show;
        invalidateVisual(static_cast<uint32_t>(PianoRollVisualInvalidationReason::Content));
    }
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
    int findLineAnchorSegmentNear(int x, int y) const;
    void selectLineAnchorSegment(int idx);
    void toggleLineAnchorSegmentSelection(int idx);
    void clearLineAnchorSegmentSelection();
    bool applyCorrectionAsyncForEntireClip(float retuneSpeed, float vibratoDepth, float vibratoRate);
    void setNoteSplit(float value);
    
    bool isAutoTuneProcessing() const;
    double getContentDurationSeconds() const;
    bool hasSelectionRange() const { return interactionState_.selection.hasSelectionArea && interactionState_.selection.selectionStartTime != interactionState_.selection.selectionEndTime; }
    std::pair<double, double> getSelectionTimeRange() const
    {
        return { std::min(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime),
                 std::max(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime) };
    }

    void setContentProjection(const ContentTimelineProjection& projection);
    void setTimelineContentPlacements(std::vector<TimelineContentPlacement> placements);
    void setTimelineViewDomain(double viewStartSeconds, double viewEndSeconds);
    void clearTimelineViewDomain();
    
    /** 设置 reference overlay 数据（ghost notes + anchors）。
     *  传入 std::nullopt 清除 overlay。 */
    void setReferenceOverlay(std::optional<PianoRollRenderer::ReferenceOverlay> overlay);

    void setPlayheadColour(juce::Colour colour) {
        playheadColour_ = colour;
        playheadOverlay_.setPlayheadColour(colour);
    }

    void setPlayheadPositionSource(std::weak_ptr<std::atomic<double>> source) {
        positionSource_ = source;
    }

    void fitToScreen();

    enum class AutoTuneApplyStatus
    {
        Applied,
        NoCurve,
        NoProcessor,
        NoContent,
        MissingContentSnapshot,
        OriginalF0NotReady,
        AlreadyInFlight,
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

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    void invalidateVisual(const PianoRollVisualInvalidationRequest& request);
    void invalidateVisual(uint32_t reasonsMask,
                          PianoRollVisualInvalidationPriority priority = PianoRollVisualInvalidationPriority::Normal);
    void invalidateVisual(uint32_t reasonsMask,
                          const juce::Rectangle<int>& dirtyArea,
                          PianoRollVisualInvalidationPriority priority = PianoRollVisualInvalidationPriority::Interactive);
    void flushPendingVisualInvalidation();

    /** Request a semantic content redraw via FrameScheduler. */
    void requestContentRedraw();

    void scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) override;
    void updateScrollBars();

private:
    friend struct PianoRollComponentTestProbe;
    friend class PianoRollPreviewOverlay;

    bool enqueueManualCorrectionPatchAsync(const std::vector<PianoRollToolHandler::ManualCorrectionOp>& ops,
                                           int dirtyStartFrame,
                                           int dirtyEndFrame,
                                           bool triggerRenderEvent);
    void enqueueNoteBasedCorrectionAsync(const std::vector<Note>& notes,
                                         int startFrame,
                                         int endFrameExclusive,
                                         float retuneSpeed,
                                         float vibratoDepth,
                                         float vibratoRate);

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
    PlayheadOverlayComponent playheadOverlay_;
    PianoRollPreviewOverlay previewOverlay_{*this};

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;   // ⚡️ §8.4 Time tool
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
public:
    bool keyPressed(const juce::KeyPress& key) override;

    /// Re-read notes from the content store and update the cache.
    /// Public so editors can drive a refresh after an async note generator
    /// (e.g. GAME) commits without changing the active ContentKey.
    void refreshEditedContentNotes();
    void rebuildSurfaceCache();

private:
    void onScrollVBlankCallback(double timestampSec);
    double readPlayheadTime() const;
    juce::Rectangle<int> getTimelineViewportBounds() const;
    int getTimelineContentViewportWidth() const;
    int getTimelineContentViewportHeight() const;
    int getContinuousPinnedPlayheadViewportX() const;
    void updatePlayheadPresentationPolicy();
    int getMaxHorizontalScroll() const;
    double getDisplayPlayheadTime(double timestampSec) const;
    void updatePresentationClock(double authoritativeTime, double timestampSec);
    void resetPresentationClock(double authoritativeTime);
    PianoRollRenderer::RenderContext makePresentationRenderContext() const;

    // v12 New: camera-based viewport
    ViewMapper makeViewMapper() const noexcept;
    int computeScrollOffsetPx() const noexcept;
    double computeContentTimelineEndSeconds() const noexcept;
    double computeSurfaceStartTimelineSeconds() const noexcept;
    double computeSurfaceEndTimelineSeconds() const noexcept;
    double computeMaxTimelineEndSeconds() const noexcept;
    double computeMaxVisibleStartSeconds(double pps) const noexcept;
    void publishPlayheadPresentation(double displayPlayheadTime);
    void followAndPublishPlayhead(double timelinePlayheadTime);

    void drawNoteDragCurvePreview(juce::Graphics& g);
    void drawHandDrawPreview(juce::Graphics& g);
    void drawLineAnchorPreview(juce::Graphics& g);
    void drawSelectionBox(juce::Graphics& g, ThemeId themeId);
    bool shouldShowPianoKeys() const noexcept;


    void handleVerticalZoomWheel(const juce::MouseEvent& e, float deltaY);
    void handleHorizontalScrollWheel(float deltaX, float deltaY);
    void handleVerticalScrollWheel(float deltaY);
    void handleHorizontalZoomWheel(const juce::MouseEvent& e, float deltaY);

    void initializeUIComponents();
    void initializeRenderer();
    void initializeCorrectionWorker();
    void consumeCompletedCorrectionResults();
    bool commitCompletedAutoTuneResult(const PianoRollCorrectionWorker::AsyncCorrectionRequest& completed);
    bool commitCompletedNoteCorrectionResult(const PianoRollCorrectionWorker::AsyncCorrectionRequest& completed);
    PianoRollToolHandler::Context buildToolHandlerContext();
    void initializeToolHandler();
    void applyEditedContentCurve(std::shared_ptr<PitchCurve> curve);
    void applyEditedContentAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, int sampleRate);
    PianoRollRenderer::ContentRenderItem buildContentRenderItem(
        const TimelineContentPlacement& placement) const;
    const std::vector<Note>& getCommittedNotes() const;
    const std::vector<Note>& getDisplayedNotes() const;
    NoteInteractionDraft& getNoteDraft();
    const NoteInteractionDraft& getNoteDraft() const;
    void beginNoteDraft();
    bool commitNoteDraft();
    void clearNoteDraft();
    bool commitEditedContentNotesAndSegments(const std::vector<Note>& notes,
                                             const std::vector<PitchCorrectionSegment>& segments,
                                             F0FrameRange affectedRange);
    bool commitEditedContentPitchCorrectionSegments(const std::vector<PitchCorrectionSegment>& segments,
                                                       F0FrameRange affectedRange);
    bool selectNotesOverlappingFrames(int startFrame, int endFrameExclusive);
    juce::Rectangle<int> getNoteBounds(const Note& note) const;
    juce::Rectangle<int> getNotesBounds(const std::vector<Note>& notes) const;
    juce::Rectangle<int> getSelectionBounds() const;
    juce::Rectangle<int> getHandDrawPreviewBounds() const;
    juce::Rectangle<int> getLineAnchorPreviewBounds() const;
    juce::Rectangle<int> getNoteDragCurvePreviewBounds() const;
    void invalidateInteractionArea(const juce::Rectangle<int>& dirtyArea);

    float midiToY(float midiNote) const;
    float yToMidi(float y) const;
    float freqToMidi(float frequency) const;
    float midiToFreq(float midiNote) const;

    float getTotalHeight() const;
    
    F0Timeline currentF0Timeline() const noexcept {
        if (currentCurve_ == nullptr) return {};
        auto snap = currentCurve_->getSnapshot();
        if (snap == nullptr || snap->size() == 0) return {};
        return { snap->getHopSize(), snap->getSampleRate(), static_cast<int>(snap->size()) };
    }

    float yToFreq(float y) const;
    float freqToY(float freq) const;

    double timelineViewOriginSeconds() const noexcept;
    double timelineViewEndSeconds() const noexcept;
    bool hasExplicitTimelineViewDomain() const noexcept;
    const TimelineContentPlacement* findActiveTimelineContentPlacement() const noexcept;
    ContentTimelineProjection activeContentProjection() const noexcept;
    double projectTimelineTimeToContent(double timelineSeconds) const;
    double projectContentTimeToTimeline(double contentSeconds) const;

    int timeToX(double seconds) const;
    double xToTime(int x) const;

    PianoRollRenderer::RenderContext buildRenderContext() const
    {
        const auto viewport = getTimelineViewportBounds();
        return buildRenderContext(viewport.getWidth(), pianoKeyWidth_);
    }

    PianoRollRenderer::RenderContext buildRenderContext(int renderWidthPx, int renderPianoKeyWidth) const;

    void refreshVerticalViewportGeometry(PianoRollVisualInvalidationPriority priority =
                                             PianoRollVisualInvalidationPriority::Interactive);

    std::shared_ptr<PitchCurve> currentCurve_;
    TimelineViewportCamera camera_{0.0, TimelineViewportCamera::kDefaultPixelsPerSecond};
    float verticalScrollOffset_ = 0.0f;
    ScrollMode scrollMode_ = ScrollMode::Continuous;
    std::atomic<bool> isPlaying_{false};

    // Cont-mode scroll state
    bool userScrollHold_{false};         // user manually scrolled → pause auto-follow
    double pendingSeekTime_{-1.0};       // pending playhead presentation intent; -1 = none
    double lastObservedRawPlayheadTime_{0.0}; // host raw playhead last seen by stopped-state presentation

    bool userHasManuallyZoomed_ = false;
    ZoomSensitivityConfig::ZoomSensitivitySettings zoomSensitivity_ = ZoomSensitivityConfig::ZoomSensitivitySettings::getDefault();
    AudioEditingScheme::Scheme audioEditingScheme_ = AudioEditingScheme::Scheme::CorrectedF0Primary;
    KeyShortcutConfig::KeyShortcutSettings shortcutSettings_ = KeyShortcutConfig::KeyShortcutSettings::getDefault();

    int scaleRootNote_ = 0;
    int scaleType_ = 1;

    static constexpr float minMidi_ = 24.0f;
    static constexpr float maxMidi_ = 108.0f;
    float pixelsPerSemitone_ = 25.0f;

    ToolId currentTool_ = ToolId::Select;

    InteractionState interactionState_;

    float dragStartVerticalScrollOffset_ = 0.0f;

    float recalculatePIP(Note& note);

private:

    bool showWaveform_ = true;
    bool showLanes_ = true;
    NoteNameMode noteNameMode_ = NoteNameMode::COnly;
    bool showChunkBoundaries_ = false;
    bool showUnvoicedFrames_ = false;
    bool showOriginalF0_ = true;
    bool showCorrectedF0_ = true;
    float currentRetuneSpeed_ = PitchControlConfig::kDefaultRetuneSpeedNormalized;
    float currentVibratoDepth_ = PitchControlConfig::kDefaultVibratoDepth;
    float currentVibratoRate_ = PitchControlConfig::kDefaultVibratoRateHz;

    NoteSegmentationPolicy segmentationPolicy_;
    
    std::atomic<bool> autoTuneInFlight_{false};
    std::atomic<uint64_t> editedContentEpoch_{0};

    double bpm_ = 120.0;
    int timeSigNum_ = 4;
    int timeSigDenom_ = 4;
    TimeUnit timeUnit_ = TimeUnit::Seconds;

    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer_;
    double audioBufferSampleRate_ = static_cast<double>(kAudioSampleRate);

    std::vector<TimelineContentPlacement> timelineContentPlacements_;
    ContentTimelineProjection pendingSingleContentProjection_;
    bool explicitTimelineContentPlacements_ = false;
    struct TimelineViewDomain {
        double startSeconds{0.0};
        double endSeconds{0.0};

        bool isValid() const noexcept { return endSeconds > startSeconds; }
    };
    TimelineViewDomain timelineViewDomain_;
    PianoRollVisualInvalidationState pendingVisualInvalidation_;
    double lastVisualFlushMs_ = 0.0;
    bool inferenceActive_ = false;
    int waveformBuildTickCounter_ = 0;
    bool waveformVisualRefreshPending_ = false;

    OpenTuneAudioProcessor* processor_ = nullptr;

    // Phase 4: Read snapshot via callback, write via ContentEditCommands
    ReadContentSnapshotFn readContentSnapshot_;
    std::shared_ptr<ContentEditCommands> contentCommands_;

    std::shared_ptr<const EditableContentSnapshot> readSnapshotFor(ContentKey key) const {
        return readContentSnapshot_ ? readContentSnapshot_(key) : nullptr;
    }
    std::shared_ptr<const EditableContentSnapshot> readEditedSnapshot() const {
        return readSnapshotFor(editedContentKey_);
    }

    // [ARA 重构] 域内容所有者（替代 contentAccess_/contentCommands_ 的旧路由）

    ContentKey editedContentKey_;
    bool experimentalFeaturesEnabled_ = false;
    std::vector<Note> cachedNotes_;

    std::optional<PianoRollRenderer::ReferenceOverlay> referenceOverlay_;

    mutable uint64_t interactionRevision_ = 0;
    double lastAuthoritativePlayheadTime_ = 0.0;
    double presentationClockAnchorTime_ = 0.0;
    double presentationClockAnchorTimestampSec_ = 0.0;
    double presentationClockLastObservationTimestampSec_ = 0.0;
    bool presentationClockPrimed_ = false;

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

    std::unique_ptr<PianoRollRenderer> renderer_;
    std::unique_ptr<PianoRollToolHandler> toolHandler_;
    std::unique_ptr<PianoRollCorrectionWorker> correctionWorker_;
    mutable WaveformMipmapCache waveformMipmapCache_;
    PianoRollSurfaceCache surfaceCache_;

    static constexpr int pianoKeyWidth_ = 60;
    static constexpr int rulerHeight_ = 30;
    static constexpr int timelineExtendedHitArea_ = 20;
    static constexpr int dragThreshold_ = 5;
    
    PianoKeyAudition* pianoKeyAudition_ = nullptr;
    int pressedPianoKey_ = -1;
    
    std::unique_ptr<juce::VBlankAttachment> scrollVBlankAttachment_;
    std::weak_ptr<std::atomic<double>> positionSource_;

    juce::ListenerList<Listener> listeners_;
    
    juce::Colour playheadColour_{0xFFE74C3C};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};

} // namespace OpenTune
