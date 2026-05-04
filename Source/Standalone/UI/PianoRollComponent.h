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
#include "TimeConverter.h"
#include "ToolIds.h"
#include "UIColors.h"
#include "Utils/F0Timeline.h"
#include "Utils/AudioEditingScheme.h"
#include "Utils/MaterializationTimelineProjection.h"
#include "Utils/PianoRollVisualPreferences.h"
#include "Utils/PitchCurve.h"
#include "Utils/Note.h"
#include "Utils/NoteGenerator.h"
#include "Utils/PitchControlConfig.h"
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
#include "PianoRoll/PianoRollToolHandler.h"
#include "PianoRoll/PianoRollVisualInvalidation.h"
#include "PianoRoll/PianoRollCorrectionWorker.h"
#include "PianoRoll/InteractionState.h"
#include "WaveformMipmap.h"
#include "../../Utils/UndoManager.h"

namespace OpenTune {

class OpenTuneAudioProcessor;
class PianoKeyAudition;

struct PianoRollComponentTestProbe;

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

    PianoRollComponent();
    ~PianoRollComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void onHeartbeatTick();

    void setEditedMaterialization(uint64_t materializationId,
                           std::shared_ptr<PitchCurve> curve,
                           std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                           int sampleRate);
    void setPianoKeyAudition(PianoKeyAudition* audition) { pianoKeyAudition_ = audition; }
    int getPressedPianoKey() const { return pressedPianoKey_; }

    void setProcessor(OpenTuneAudioProcessor* processor);
    void setIsPlaying(bool playing) {
        bool stateChanged = (isPlaying_.load(std::memory_order_relaxed) != playing);
        isPlaying_.store(playing, std::memory_order_relaxed);
        playheadOverlay_.setPlaying(playing);
        if (stateChanged)
            userScrollHold_ = false;
    }
    void setZoomLevel(double zoom);
    void setCurrentTool(ToolId tool);
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
        invalidateVisual(static_cast<uint32_t>(PianoRollVisualInvalidationReason::Viewport),
                         PianoRollVisualInvalidationPriority::Interactive);
    }
    ScrollMode getScrollMode() const { return scrollMode_; }
    void setScrollOffset(int offset);
    int getScrollOffset() const { return scrollOffset_; }
    void setScale(int rootNote, int scaleType);
    void setAudioEditingScheme(AudioEditingScheme::Scheme scheme) { audioEditingScheme_ = scheme; }
    void setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings& settings) { zoomSensitivity_ = settings; }
    void setShortcutSettings(const KeyShortcutConfig::KeyShortcutSettings& settings) { shortcutSettings_ = settings; }

    void resetUserZoomFlag() { userHasManuallyZoomed_ = false; }
    bool hasUserManuallyZoomed() const { return userHasManuallyZoomed_; }

    void setShowOriginalF0(bool show) {
        showOriginalF0_ = show;
        invalidateVisual(static_cast<uint32_t>(PianoRollVisualInvalidationReason::Content));
    }
    void setShowCorrectedF0(bool show) {
        showCorrectedF0_ = show;
        invalidateVisual(static_cast<uint32_t>(PianoRollVisualInvalidationReason::Content));
    }
    bool isShowingOriginalF0() const { return showOriginalF0_; }

    void setRetuneSpeed(float speed) { currentRetuneSpeed_ = speed; }
    float getCurrentRetuneSpeed() const { return currentRetuneSpeed_; }
    bool applyRetuneSpeedToSelection(float speed);
    bool applyRetuneSpeedToSelectedLineAnchorSegments(float speed);
    void setVibratoDepth(float depth) { currentVibratoDepth_ = depth; }
    float getCurrentVibratoDepth() const { return currentVibratoDepth_; }
    bool applyVibratoDepthToSelection(float depth);
    void setVibratoRate(float rate) { currentVibratoRate_ = rate; }
    float getCurrentVibratoRate() const { return currentVibratoRate_; }
    bool applyVibratoRateToSelection(float rate);
    bool getSingleSelectedNoteParameters(float& retuneSpeedPercent, float& vibratoDepth, float& vibratoRate) const;
    bool getSelectedSegmentRetuneSpeed(float& retuneSpeedPercent) const;
    int findLineAnchorSegmentNear(int x, int y) const;
    void selectLineAnchorSegment(int idx);
    void toggleLineAnchorSegmentSelection(int idx);
    void clearLineAnchorSegmentSelection();
    bool applyCorrectionAsyncForEntireClip(float retuneSpeed, float vibratoDepth, float vibratoRate);
    void setNoteSplit(float value);
    
    bool isAutoTuneProcessing() const;
    double getMaterializationDurationSeconds() const { return materializationProjection_.materializationDurationSeconds; }
    bool hasSelectionRange() const { return interactionState_.selection.hasSelectionArea && interactionState_.selection.selectionStartTime != interactionState_.selection.selectionEndTime; }
    std::pair<double, double> getSelectionTimeRange() const
    {
        return { std::min(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime),
                 std::max(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime) };
    }

    void setMaterializationProjection(const MaterializationTimelineProjection& projection);
    
    void setPlayheadColour(juce::Colour colour) {
        playheadOverlay_.setPlayheadColour(colour);
    }

    void setPlayheadPositionSource(std::weak_ptr<std::atomic<double>> source) {
        positionSource_ = source;
    }

    void fitToScreen();

    bool applyAutoTuneToSelection();

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    void invalidateVisual(const PianoRollVisualInvalidationRequest& request);
    void invalidateVisual(uint32_t reasonsMask,
                          PianoRollVisualInvalidationPriority priority = PianoRollVisualInvalidationPriority::Normal);
    void invalidateVisual(uint32_t reasonsMask,
                          const juce::Rectangle<int>& dirtyArea,
                          PianoRollVisualInvalidationPriority priority = PianoRollVisualInvalidationPriority::Interactive);
    void flushPendingVisualInvalidation();

    void scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) override;
    void updateScrollBars();

private:
    friend struct PianoRollComponentTestProbe;

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

    bool hasHandDrawCorrectionInRange(int startFrame, int endFrame) const;
    bool applyNoteParameterToSelectedNotes(float retuneSpeed, float vibratoDepth, float vibratoRate);
    bool applyParameterToFrameRange(float retuneSpeed, float vibratoDepth, float vibratoRate, int startFrame, int endFrameExclusive);
    bool getFrameRangeForTimeSpan(double startTime, double endTime, int& startFrame, int& endFrameExclusive) const;
    bool getSelectedNotesFrameRange(int& startFrame, int& endFrameExclusive) const;
    bool getSelectionAreaFrameRange(int& startFrame, int& endFrameExclusive) const;
    bool getF0SelectionFrameRange(int& startFrame, int& endFrameExclusive) const;

    juce::ScrollBar horizontalScrollBar_{ false };
    juce::ScrollBar verticalScrollBar_{ true };
    SmallButton scrollModeToggleButton_;
    SmallButton timeUnitToggleButton_;
    PlayheadOverlayComponent playheadOverlay_;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
public:
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void onScrollVBlankCallback(double timestampSec);
    double readPlayheadTime() const;
    double readProjectedPlayheadTime() const;
    juce::Rectangle<int> getTimelineViewportBounds() const;

    void drawSelectedOriginalF0Curve(juce::Graphics& g, const std::vector<float>& originalF0);
    void drawNoteDragCurvePreview(juce::Graphics& g);
    void drawHandDrawPreview(juce::Graphics& g);
    void drawLineAnchorPreview(juce::Graphics& g);
    void drawSelectionBox(juce::Graphics& g, ThemeId themeId);


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
    void applyEditedMaterializationCurve(std::shared_ptr<PitchCurve> curve);
    void applyEditedMaterializationAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, int sampleRate);
    void refreshEditedMaterializationNotes();
    const std::vector<Note>& getCommittedNotes() const;
    const std::vector<Note>& getDisplayedNotes() const;
    NoteInteractionDraft& getNoteDraft();
    const NoteInteractionDraft& getNoteDraft() const;
    void beginNoteDraft();
    bool commitNoteDraft();
    void clearNoteDraft();
    bool commitEditedMaterializationNotes(const std::vector<Note>& notes);
    bool commitEditedMaterializationNotesAndSegments(const std::vector<Note>& notes,
                                             const std::vector<CorrectedSegment>& segments);
    bool commitEditedMaterializationCorrectedSegments(const std::vector<CorrectedSegment>& segments);
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

    double toVisibleTimelineSeconds(double absoluteSeconds) const;
    double toAbsoluteTimelineSeconds(double visibleSeconds) const;
    double projectTimelineTimeToMaterialization(double timelineSeconds) const;
    double projectMaterializationTimeToTimeline(double materializationSeconds) const;
    double getTimelinePixelsPerSecond() const;
    double getPlayheadAbsolutePixelX(double playheadTimeSeconds) const;

    int timeToX(double seconds) const;
    double xToTime(int x) const;

    PianoRollRenderer::RenderContext buildRenderContext() const;

    TimeConverter timeConverter_;

    std::shared_ptr<PitchCurve> currentCurve_;
    double zoomLevel_ = 1.0;
    int scrollOffset_ = 0;
    float verticalScrollOffset_ = 0.0f;
    ScrollMode scrollMode_ = ScrollMode::Continuous;
    std::atomic<bool> isPlaying_{false};

    // Cont-mode scroll state
    bool userScrollHold_{false};         // user manually scrolled → pause auto-follow
    float scrollSeekOffset_{0.0f};       // smooth seek: decays to 0 each frame
    double pendingSeekTime_{-1.0};       // ARA seek pending host confirmation; -1 = none

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

    int dragStartScrollOffset_ = 0;
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
    std::atomic<uint64_t> editedMaterializationEpoch_{0};

    double bpm_ = 120.0;
    int timeSigNum_ = 4;
    int timeSigDenom_ = 4;
    TimeUnit timeUnit_ = TimeUnit::Seconds;

    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer_;
    double audioBufferSampleRate_ = static_cast<double>(kAudioSampleRate);

    MaterializationTimelineProjection materializationProjection_;
    PianoRollVisualInvalidationState pendingVisualInvalidation_;
    double lastVisualFlushMs_ = 0.0;
    bool inferenceActive_ = false;
    int waveformBuildTickCounter_ = 0;

    OpenTuneAudioProcessor* processor_ = nullptr;

    uint64_t editedMaterializationId_ = 0;
    std::vector<Note> cachedNotes_;

    // Undo support
    juce::String pendingUndoDescription_;
    std::vector<Note> beforeUndoNotes_;
    std::vector<CorrectedSegment> beforeUndoSegments_;
    bool undoSnapshotCaptured_{false};
    void captureBeforeUndoSnapshot();
    void recordUndoAction(const juce::String& description);
    std::vector<CorrectedSegment> getCurrentSegments() const;
    
    bool applyVibratoParameterToSelection(VibratoParam param, float value);

    std::vector<Note> getEditedMaterializationNotesCopy() const;

    std::unique_ptr<PianoRollRenderer> renderer_;
    std::unique_ptr<PianoRollToolHandler> toolHandler_;
    std::unique_ptr<PianoRollCorrectionWorker> correctionWorker_;
    WaveformMipmap waveformMipmap_;

    static constexpr int pianoKeyWidth_ = 60;
    static constexpr int rulerHeight_ = 30;
    static constexpr int timelineExtendedHitArea_ = 20;
    static constexpr int dragThreshold_ = 5;
    
    PianoKeyAudition* pianoKeyAudition_ = nullptr;
    int pressedPianoKey_ = -1;
    
    std::unique_ptr<juce::VBlankAttachment> scrollVBlankAttachment_;

    std::weak_ptr<std::atomic<double>> positionSource_;

    juce::ListenerList<Listener> listeners_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};

} // namespace OpenTune
