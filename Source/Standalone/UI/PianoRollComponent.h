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
#include "PlayheadOverlayComponent.h"
#include "Utils/PitchCurve.h"
#include "Utils/Note.h"
#include "Utils/NoteGenerator.h"
#include "Utils/PitchControlConfig.h"
#include "Utils/UndoAction.h"
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
#include "PianoRoll/PianoRollUndoSupport.h"
#include "PianoRoll/PianoRollCoordinateSystem.h"
#include "PianoRoll/PianoRollRenderer.h"
#include "PianoRoll/PianoRollToolHandler.h"
#include "PianoRoll/PianoRollCorrectionWorker.h"

namespace OpenTune {

class PianoRollComponent : public juce::Component,
                           public juce::ScrollBar::Listener,
                           private juce::Timer {
public:
    static constexpr int kContextMenuCommandSelect = 3001;
    static constexpr int kContextMenuCommandDrawNote = 3002;
    static constexpr int kContextMenuCommandHandDraw = 3003;
    static constexpr int kAudioSampleRate = 44100;

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void playheadPositionChangeRequested(double timeSeconds) = 0;
        virtual void playPauseToggleRequested() = 0;
        virtual void stopPlaybackRequested() = 0;
        virtual void pitchCurveEdited(int startFrame, int endFrame) { (void)startFrame; (void)endFrame; }
        virtual void noteOffsetChanged(Note* note, float oldOffset, float newOffset) { (void)note; (void)oldOffset; (void)newOffset; }
        virtual void autoTuneRequested() {}
        virtual void trackTimeOffsetChanged(int trackId, double newOffset) { (void)trackId; (void)newOffset; }
        virtual void escapeKeyPressed() {}
        virtual void notesChanged(const std::vector<Note>& notes) { (void)notes; }
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

    void setPitchCurve(std::shared_ptr<PitchCurve> curve);
    void setAudioBuffer(const juce::AudioBuffer<float>* buffer, int sampleRate);
    void setGlobalUndoManager(UndoManager* um) { globalUndoManager_ = um; }
    void setIsPlaying(bool playing) {
        bool stateChanged = (isPlaying_.load(std::memory_order_relaxed) != playing);
        isPlaying_.store(playing, std::memory_order_relaxed);
        playheadOverlay_.setPlaying(playing);
        if (stateChanged) {
            snapNextScroll_ = true;
        }
    }
    void setZoomLevel(double zoom);
    void setCurrentTool(ToolId tool);
    ToolId getCurrentTool() const { return currentTool_; }
    bool selectToolByContextMenuCommand(int commandId);
    void setShowWaveform(bool shouldShow);
    void setShowLanes(bool shouldShow);
    void setInferenceActive(bool active);
    void setBpm(double bpm);
    void setTimeSignature(int numerator, int denominator);
    void setTimeUnit(TimeUnit unit);
    TimeUnit getTimeUnit() const { return timeUnit_; }
    void setScrollMode(ScrollMode mode) { scrollMode_ = mode; }
    ScrollMode getScrollMode() const { return scrollMode_; }
    void setScrollOffset(int offset);
    int getScrollOffset() const { return scrollOffset_; }
    void setHopSize(int hopSize) { hopSize_ = hopSize; }
    void setF0SampleRate(double rate) { f0SampleRate_ = rate; }
    void setHasUserAudio(bool hasAudio);
    void setScale(int rootNote, int scaleType);

    void resetUserZoomFlag() { userHasManuallyZoomed_ = false; }
    bool hasUserManuallyZoomed() const { return userHasManuallyZoomed_; }

    void setShowOriginalF0(bool show) { showOriginalF0_ = show; repaint(); }
    void setShowCorrectedF0(bool show) { showCorrectedF0_ = show; repaint(); }
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
    
    void setRenderingProgress(float progress, int pendingTasks);
    
    bool isAutoTuneProcessing() const { return isAutoTuneProcessing_.load(); }
    bool hasSelectionRange() const { return hasSelectionArea_ && selectionStartTime_ != selectionEndTime_; }
    std::pair<double, double> getSelectionTimeRange() const
    {
        return { std::min(selectionStartTime_, selectionEndTime_),
                 std::max(selectionStartTime_, selectionEndTime_) };
    }

    void setActiveTrackId(int trackId) { activeTrackId_ = trackId; }
    void setTrackTimeOffset(double offsetSeconds) {
        trackOffsetSeconds_ = juce::jmax(0.0, offsetSeconds);
        playheadOverlay_.setTrackOffsetSeconds(trackOffsetSeconds_);
        repaint();
    }
    double getTrackTimeOffset() const { return trackOffsetSeconds_; }
    
    void setAlignmentOffset(double offsetSeconds) { 
        alignmentOffsetSeconds_ = offsetSeconds; 
        playheadOverlay_.setAlignmentOffsetSeconds(offsetSeconds);
        snapNextScroll_ = true; 
        repaint(); 
    }
    double getAlignmentOffset() const { return alignmentOffsetSeconds_; }

    void setPlayheadColour(juce::Colour colour) {
        playheadOverlay_.setPlayheadColour(colour);
    }

    void setPlayheadPositionSource(std::weak_ptr<std::atomic<double>> source) {
        positionSource_ = source;
    }

    void fitToScreen();

    void undo();
    void redo();
    void refreshAfterUndoRedo();
    bool canUndo() const;
    bool canRedo() const;

    void setNotes(const std::vector<Note>& notes);
    const std::vector<Note>& getNotes() const { return noteSequence_.getNotes(); }
    
    std::shared_ptr<PitchCurve> getPitchCurve() const { return currentCurve_; }
    
    bool applyAutoTuneToSelection();

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    void scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) override;
    void updateScrollBars();

    std::function<void(int root, int scaleType)> onKeyDetected;
    std::function<void()> onRenderComplete_;
    void setRenderCompleteCallback(std::function<void()> cb) { onRenderComplete_ = std::move(cb); }

private:
    void enqueueManualCorrectionPatchAsync(const std::vector<PianoRollCorrectionWorker::AsyncCorrectionRequest::ManualOp>& ops,
                                           int dirtyStartFrame,
                                           int dirtyEndFrame,
                                           bool triggerRenderEvent);

    bool hasHandDrawCorrectionInRange(int startFrame, int endFrame) const;
    bool applyRetuneSpeedToSelectedNotes(float speed);
    bool applyRetuneSpeedToSelectionArea(float speed, int startFrame, int endFrame);

    bool userIsInteracting_ = false;

    juce::ScrollBar horizontalScrollBar_{ false };
    juce::ScrollBar verticalScrollBar_{ true };
    SmallButton scrollModeToggleButton_;
    SmallButton timeUnitToggleButton_;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
public:
    bool keyPressed(const juce::KeyPress& key) override;

private:

    void timerCallback() override;
    void requestInteractiveRepaint();
    void requestInteractiveRepaint(const juce::Rectangle<int>& dirtyArea);
    void updateAutoScroll();
    void onScrollVBlankCallback(double timestampSec);
    double readPlayheadTime() const;

    void drawSelectedOriginalF0Curve(juce::Graphics& g, const std::vector<float>& originalF0, double offsetSeconds);
    void drawHandDrawPreview(juce::Graphics& g, double offsetSeconds);
    void drawLineAnchorPreview(juce::Graphics& g, double offsetSeconds);
    void drawSelectionBox(juce::Graphics& g, double offsetSeconds, ThemeId themeId);
    void drawRenderingProgress(juce::Graphics& g);

    void handleVerticalZoomWheel(const juce::MouseEvent& e, float deltaY);
    void handleHorizontalScrollWheel(float deltaX, float deltaY);
    void handleVerticalScrollWheel(float deltaY);
    void handleHorizontalZoomWheel(const juce::MouseEvent& e, float deltaY);

    void initializeUIComponents();
    void initializeUndoSupport();
    void initializeCoordinateSystem();
    void initializeRenderer();
    void initializeCorrectionWorker();
    PianoRollToolHandler::Context buildToolHandlerContext();
    void initializeToolHandler();

    float midiToY(float midiNote) const;
    float yToMidi(float y) const;
    float freqToMidi(float frequency) const;
    float midiToFreq(float midiNote) const;

    float getTotalHeight() const;
    
    int clipSecondsToFrameIndex(double clipSeconds, size_t totalFrames = 0) const {
        if (hopSize_ <= 0 || f0SampleRate_ <= 0.0) return -1;
        const double frameDuration = static_cast<double>(hopSize_) / f0SampleRate_;
        const int frame = static_cast<int>(std::floor(clipSeconds / frameDuration));
        if (totalFrames > 0 && frame >= static_cast<int>(totalFrames)) {
            return static_cast<int>(totalFrames) - 1;
        }
        return frame;
    }

    int clipSecondsToFrameIndexCeil(double clipSeconds, size_t totalFrames = 0) const {
        if (hopSize_ <= 0 || f0SampleRate_ <= 0.0) return -1;
        const double frameDuration = static_cast<double>(hopSize_) / f0SampleRate_;
        int frame = static_cast<int>(std::ceil(clipSeconds / frameDuration));
        if (totalFrames > 0 && frame >= static_cast<int>(totalFrames)) {
            return static_cast<int>(totalFrames) - 1;
        }
        return frame;
    }

    std::pair<int, int> clipTimeRangeToFrameRangeHalfOpen(double startTime, double endTime, int maxFrameExclusive) const {
        const int startFrame = clipSecondsToFrameIndex(startTime, maxFrameExclusive);
        const int endFrame = clipSecondsToFrameIndexCeil(endTime, maxFrameExclusive);
        return { std::max(0, startFrame), std::min(maxFrameExclusive, endFrame) };
    }

    double frameIndexToClipSeconds(int frameIndex) const {
        if (hopSize_ <= 0 || f0SampleRate_ <= 0.0) return 0.0;
        return static_cast<double>(frameIndex) * static_cast<double>(hopSize_) / f0SampleRate_;
    }

    float yToFreq(float y) const;
    float freqToY(float freq) const;

    int timeToX(double seconds) const;
    double xToTime(int x) const;

    PianoRollRenderer::RenderContext buildRenderContext() const;
    
    Note* findNoteAtPixel(int pixelX, int pixelY);

    TimeConverter timeConverter_;

    std::shared_ptr<PitchCurve> currentCurve_;
    double zoomLevel_ = 1.0;
    int scrollOffset_ = 0;
    float verticalScrollOffset_ = 0.0f;
    ScrollMode scrollMode_ = ScrollMode::Continuous;
    std::atomic<bool> isPlaying_{false};

    float smoothScrollCurrent_{0.0f};
    bool snapNextScroll_{false};
    bool isSmoothScrolling_{false};

    bool userHasManuallyZoomed_ = false;

    int scaleRootNote_ = 0;
    int scaleType_ = 1;

    static constexpr float minMidi_ = 24.0f;
    static constexpr float maxMidi_ = 108.0f;
    float pixelsPerSemitone_ = 25.0f;

    ToolId currentTool_ = ToolId::Select;

    bool isDragging_ = false;
    bool isPanning_ = false;
    juce::Point<int> dragStartPos_;
    double dragStartTime_ = 0.0;
    float dragStartF0_ = 0.0f;
    int dragStartScrollOffset_ = 0;
    float dragStartVerticalScrollOffset_ = 0.0f;

    enum class NoteResizeEdge {
        None,
        Left,
        Right
    };

    Note* draggedNote_ = nullptr;
    float originalPitchOffset_ = 0.0f;
    std::vector<std::pair<Note*, float>> initialNoteOffsets_;
    bool isDraggingNotes_ = false;
    bool isResizingNote_ = false;
    bool isResizingNoteDirty_ = false;
    Note* resizingNote_ = nullptr;
    NoteResizeEdge resizingEdge_ = NoteResizeEdge::None;
    double resizeStartTime_ = 0.0;
    double resizeEndTime_ = 0.0;
    int noteDragManualStartFrame_ = -1;
    int noteDragManualEndFrame_ = -1;
    std::vector<std::pair<int, float>> noteDragInitialManualTargets_;

    bool isSelectingArea_ = false;
    bool hasSelectionArea_ = false;
    double selectionStartTime_ = 0.0;
    double selectionEndTime_ = 0.0;
    float selectionStartMidi_ = 0.0f;
    float selectionEndMidi_ = 0.0f;

    struct F0Edit {
        int frameIndex;
        float oldF0;
        float newF0;
    };
    bool isDrawingF0_ = false;
    std::vector<F0Edit> currentDrawEdits_;
    juce::Point<float> lastDrawPoint_;
    int dirtyRangeStart_ = -1;
    int dirtyRangeEnd_ = -1;
    std::vector<std::pair<double, float>> handDrawPoints_;
    std::vector<float> handDrawBuffer_;

    bool isPlacingAnchors_ = false;
    std::vector<LineAnchor> pendingAnchors_;
    juce::Point<float> currentMousePos_;
    std::vector<int> selectedLineAnchorSegmentIds_;

    struct F0EditRange {
        int startFrame = -1;
        int endFrame = -1;
        bool isValid() const noexcept { return startFrame >= 0 && endFrame >= startFrame; }
    };
    F0EditRange pendingF0EditRange_;

    bool isDrawingNote_ = false;
    double drawingNoteStartTime_ = 0.0;
    double drawingNoteEndTime_ = 0.0;
    float drawingNotePitch_ = 0.0f;
    int drawingNoteIndex_ = -1;

    float recalculatePIP(Note& note);

public:
    void setCurrentClipContext(int trackId, uint64_t clipId);
    void clearClipContext();
    bool hasActiveClipContext() const { return currentClipId_ != 0; }
    int getCurrentTrackId() const { return currentTrackId_; }
    uint64_t getCurrentClipId() const { return currentClipId_; }
    bool isCurrentClipOriginalF0Visible() const
    {
        if (!showOriginalF0_ || currentCurve_ == nullptr)
            return false;

        const auto snapshot = currentCurve_->getSnapshot();
        return snapshot != nullptr && !snapshot->getOriginalF0().empty();
    }

private:

    bool isDraggingTrack_ = false;
    double dragStartOffset_ = 0.0;
    int dragStartX_ = 0;

    bool showWaveform_ = true;
    bool showLanes_ = true;
    bool showOriginalF0_ = true;
    bool showCorrectedF0_ = true;
    float currentRetuneSpeed_ = PitchControlConfig::kDefaultRetuneSpeedNormalized;
    float currentVibratoDepth_ = PitchControlConfig::kDefaultVibratoDepth;
    float currentVibratoRate_ = PitchControlConfig::kDefaultVibratoRateHz;

    NoteSegmentationPolicy segmentationPolicy_;
    
    float renderingProgress_ = 0.0f;
    int pendingRenderTasks_ = 0;
    bool isRendering_ = false;
    
    std::atomic<bool> isAutoTuneProcessing_{false};
    std::atomic<uint64_t> clipContextGeneration_{0};

    juce::Colour currentTrackColor_ = juce::Colours::white;
    juce::String currentTrackName_;

    double bpm_ = 120.0;
    int timeSigNum_ = 4;
    int timeSigDenom_ = 4;
    TimeUnit timeUnit_ = TimeUnit::Seconds;

    const juce::AudioBuffer<float>* audioBuffer_ = nullptr;
    int hopSize_ = 512;
    double f0SampleRate_ = 16000.0;
    int activeTrackId_ = 0;
    double trackOffsetSeconds_ = 0.0;
    double alignmentOffsetSeconds_ = 0.0;
    double lastPaintedPlayheadTime_ = -1.0;
    double lastInteractiveRepaintMs_ = 0.0;
    bool pendingInteractiveRepaint_ = false;
    juce::Rectangle<int> pendingInteractiveDirtyArea_;
    bool hasPendingInteractiveDirtyArea_ = false;
    bool inferenceActive_ = false;
    int waveformBuildTickCounter_ = 0;

    NoteSequence noteSequence_;
    
    UndoManager* globalUndoManager_ = nullptr;
    int currentTrackId_ = -1;
    uint64_t currentClipId_ = 0;
    
    UndoManager* getCurrentUndoManager() noexcept;
    const UndoManager* getCurrentUndoManager() const noexcept;
    bool canUndoInternal() const noexcept;
    bool canRedoInternal() const noexcept;
    
    enum class VibratoParam { Depth, Rate };
    bool applyVibratoParameterToSelection(VibratoParam param, float value);
    
    std::unique_ptr<PianoRollUndoSupport> undoSupport_;
    
    PianoRollCoordinateSystem coordSystem_;
    std::unique_ptr<PianoRollRenderer> renderer_;
    std::unique_ptr<PianoRollToolHandler> toolHandler_;
    std::unique_ptr<PianoRollCorrectionWorker> correctionWorker_;

    static constexpr int pianoKeyWidth_ = 60;
    static constexpr int rulerHeight_ = 30;
    static constexpr int timelineExtendedHitArea_ = 20;
    static constexpr int dragThreshold_ = 5;
    
    bool drawNoteToolPendingDrag_ = false;
    juce::Point<int> drawNoteToolMouseDownPos_;

    bool handDrawPendingDrag_ = false;

    bool hasUserAudio_ = false;

    PlayheadOverlayComponent playheadOverlay_;

    std::unique_ptr<juce::VBlankAttachment> scrollVBlankAttachment_;

    std::weak_ptr<std::atomic<double>> positionSource_;

    juce::ListenerList<Listener> listeners_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};

} // namespace OpenTune
