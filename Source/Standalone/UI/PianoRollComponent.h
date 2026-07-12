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
#include <map>
#include <unordered_map>
#include <optional>
#include <utility>
#include <atomic>
#include <array>
    #include "SmallButton.h"
    #include "PianoRoll/PianoRollRenderer.h"

#include "PianoRoll/PianoRollToolHandler.h"
#include "PianoRoll/PianoRollCorrectionWorker.h"
#include "PianoRoll/InteractionState.h"
#include "TimelineViewportCamera.h"
#include "TimelineViewportPolicy.h"
#include "TimelineCompositeCache.h"
#include "WaveformMipmap.h"
#include "../../Utils/UndoManager.h"
#include "../../Content/ContentEditCommands.h"

namespace OpenTune {

class OpenTuneAudioProcessor;
class PianoKeyAudition;

struct PianoRollComponentTestProbe;

// ============================================================================
// PianoRollComponent — piano roll editor with tile-cached rendering.
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
        if (stateChanged) {
            userScrollHold_ = false;
            repaint();
        }
    }
    void commitViewportRequest(TimelineViewportRequest req);
    int timelinePolicyViewportWidth() const noexcept { return getTimelineContentViewportWidth(); }
    void focusActiveContentForRegionSwitch(const std::vector<SilentGap>& silentGaps);
    TimelineViewportCamera timelineCamera() const noexcept { return camera_; }
    void activateTimelineCamera(TimelineViewportCamera camera);
    void setCurrentTool(ToolId tool);
    void setExperimentalFeaturesEnabled(bool enabled);
    ToolId getCurrentTool() const { return currentTool_; }
    void setShowWaveform(bool shouldShow);
    void setShowLanes(bool shouldShow);
    void setNoteNameMode(NoteNameMode noteNameMode);
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
        repaint();
    }
    ScrollMode getScrollMode() const { return scrollMode_; }
    void setScale(int rootNote, int scaleType);
    void setAudioEditingScheme(AudioEditingScheme::Scheme scheme) { audioEditingScheme_ = scheme; }
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

    /** 设置 reference overlay 数据（ghost notes + anchors）。
     *  传入 std::nullopt 清除 overlay。 */
    void setReferenceOverlay(std::optional<PianoRollRenderer::ReferenceOverlay> overlay);

    void setPlayheadColour(juce::Colour colour) {
        playheadColour_ = colour;
        repaint();
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

    /** Request a semantic content redraw. */
    void requestContentRedraw();

    void scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) override;
    void updateScrollBars();

private:
    friend struct PianoRollComponentTestProbe;

    void rebuildTimelineCoverage();
    void invalidateStableScene();

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

private:
    void onScrollVBlankCallback(double timestampSec);
    double readPlayheadTime() const;
    juce::Rectangle<int> getTimelineViewportBounds() const;
    int getTimelineContentViewportWidth() const;
    int getTimelineContentViewportHeight() const;
    int getMaxHorizontalScroll() const;
    PianoRollRenderer::RenderContext makePresentationRenderContext() const;

    // v12 New: camera-based viewport
    ViewMapper makeViewMapper() const noexcept;
    double computeContentTimelineEndSeconds() const noexcept;
    void prepareCoverageCompositeTilesNew();

    // Composite cache helpers (Phase 2)
    GenerationSignature makeGenerationSignature() const;
    void buildCompositeTile(juce::Graphics& g, juce::Rectangle<int> tileBounds,
                           int64_t absoluteTile, double ppsCanonical, double tileDuration);

    void drawNoteDragCurvePreview(juce::Graphics& g);
    void drawHandDrawPreview(juce::Graphics& g);
    void drawLineAnchorPreview(juce::Graphics& g);
    void drawSelectionBox(juce::Graphics& g, ThemeId themeId);
    bool shouldShowPianoKeys() const noexcept;
    bool isTimeView() const noexcept { return currentTool_ == ToolId::TimeTool; }


    void handleVerticalZoomWheel(const juce::MouseEvent& e, float deltaY);
    void handleHorizontalScrollWheel(float deltaX, float deltaY);
    void handleVerticalScrollWheel(float deltaY);
    void handleHorizontalZoomWheel(const juce::MouseEvent& e, float deltaY);

    void drawTransientOverlay(juce::Graphics& g);
    void drawPlayhead(juce::Graphics& g);

    TimelineViewportRequest makeViewportRequest(
        TimelineViewportRequest::Kind kind,
        double targetTime,
        double anchorViewportX,
        double pps) const;

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

    const TimelineContentPlacement* findEditedPlacement() const noexcept;
    bool hasTimelineContentPlacement() const noexcept;
    ContentTimelineProjection activeContentProjection() const noexcept;
    double sourceTimeToTimelineTime(double sourceSeconds) const;
    int  sourceTimeToX(double sourceSeconds) const;
    double xToSourceTime(int x) const;
    SourceEditRange sourceEditRange() const;

    PianoRollRenderer::RenderContext buildRenderContext() const
    {
        const auto viewport = getTimelineViewportBounds();
        return buildRenderContext(viewport.getWidth(), pianoKeyWidth_);
    }

    PianoRollRenderer::RenderContext buildRenderContext(int renderWidthPx, int renderPianoKeyWidth) const;

    void refreshVerticalViewportGeometry();

private:
    TimelineViewportCamera camera_{0.0, TimelineViewportCamera::kDefaultPixelsPerSecond};
    double tileCoverageStartSeconds_ = 0.0;
    double tileCoverageEndSeconds_ = 0.0;
    float verticalScrollOffset_ = 0.0f;
    ScrollMode scrollMode_ = ScrollMode::Continuous;
    std::atomic<bool> isPlaying_{false};

    // Cont-mode scroll state
    bool userScrollHold_{false};         // user manually scrolled → pause auto-follow

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
    std::unique_ptr<PianoRollCorrectionWorker> correctionWorker_;
    mutable WaveformMipmapCache waveformMipmapCache_;

    // 新增 composite cache (Phase 2 集成)
    mutable TimelineCompositeCache compositeCache_;
    uint64_t stableVisualSceneEpoch_ = 0;

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
