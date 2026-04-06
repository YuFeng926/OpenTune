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
#include "PianoRoll/PianoRollRenderer.h"
#include "PianoRoll/PianoRollToolHandler.h"
#include "PianoRoll/PianoRollCorrectionWorker.h"
#include "PianoRoll/InteractionState.h"
#include "WaveformMipmap.h"

namespace OpenTune {

class OpenTuneAudioProcessor;

class PianoRollComponent : public juce::Component,
                           public juce::ScrollBar::Listener,
                           private juce::Timer {
public:
    void visibilityChanged() override;
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
        virtual void noteOffsetChanged(size_t noteIndex, float oldOffset, float newOffset) { (void)noteIndex; (void)oldOffset; (void)newOffset; }
        virtual void autoTuneRequested() {}
        virtual void trackTimeOffsetChanged(int trackId, double newOffset) { (void)trackId; (void)newOffset; }
        virtual void escapeKeyPressed() {}
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
    void setAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, int sampleRate);
    void setGlobalUndoManager(UndoManager* um) { globalUndoManager_ = um; }
    void setProcessor(OpenTuneAudioProcessor* processor) { processor_ = processor; }
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
    void setVibratoDepth(float depth) { currentVibratoDepth_ = depth; }
    float getCurrentVibratoDepth() const { return currentVibratoDepth_; }
    bool applyVibratoDepthToSelection(float depth);
    void setVibratoRate(float rate) { currentVibratoRate_ = rate; }
    float getCurrentVibratoRate() const { return currentVibratoRate_; }
    bool applyVibratoRateToSelection(float rate);
    bool getSingleSelectedNoteParameters(float& retuneSpeedPercent, float& vibratoDepth, float& vibratoRate) const;
    bool applyCorrectionAsyncForEntireClip(float retuneSpeed, float vibratoDepth, float vibratoRate);
    void setNoteSplit(float value);
    
    void setRenderingProgress(float progress, int pendingTasks);
    
    bool isAutoTuneProcessing() const;
    bool hasSelectionRange() const;
    std::pair<double, double> getSelectionTimeRange() const;

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

    void refreshAfterUndoRedo();
    void refreshAfterUndoRedoWithRange(int startFrame, int endFrame);

    void setNotes(const std::vector<Note>& notes);
    std::vector<Note> getNotes() const { return getCurrentClipNotesCopy(); }
    
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
    void enqueueManualCorrectionPatchAsync(const std::vector<PianoRollToolHandler::ManualCorrectionOp>& ops,
                                           int dirtyStartFrame,
                                           int dirtyEndFrame,
                                           bool triggerRenderEvent);
    void enqueueNoteBasedCorrectionAsync(int startFrame,
                                         int endFrameExclusive,
                                         float retuneSpeed,
                                         float vibratoDepth,
                                         float vibratoRate);

    bool applyRetuneSpeedToSelectedNotes(float speed);

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
    void initializeRenderer();
    void initializeCorrectionWorker();
    void consumeCompletedCorrectionResults();
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

    InteractionState interactionState_;

    int dragStartScrollOffset_ = 0;
    float dragStartVerticalScrollOffset_ = 0.0f;

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
    
    std::atomic<bool> correctionInFlight_{false};
    std::atomic<uint64_t> clipContextGeneration_{0};

    juce::Colour currentTrackColor_ = juce::Colours::white;
    juce::String currentTrackName_;

    double bpm_ = 120.0;
    int timeSigNum_ = 4;
    int timeSigDenom_ = 4;
    TimeUnit timeUnit_ = TimeUnit::Seconds;

    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer_;
    int hopSize_ = 512;
    double f0SampleRate_ = 16000.0;
    double trackOffsetSeconds_ = 0.0;
    double alignmentOffsetSeconds_ = 0.0;
    double lastPaintedPlayheadTime_ = -1.0;
    double lastInteractiveRepaintMs_ = 0.0;
    bool pendingInteractiveRepaint_ = false;
    juce::Rectangle<int> pendingInteractiveDirtyArea_;
    bool hasPendingInteractiveDirtyArea_ = false;
    bool inferenceActive_ = false;
    int waveformBuildTickCounter_ = 0;

    OpenTuneAudioProcessor* processor_ = nullptr;
    UndoManager* globalUndoManager_ = nullptr;
    int currentTrackId_ = -1;
    uint64_t currentClipId_ = 0;
    
    enum class VibratoParam { Depth, Rate };
    bool applyVibratoParameterToSelection(VibratoParam param, float value);
    
    std::vector<Note>& getCurrentClipNotes();
    std::vector<Note> getCurrentClipNotesCopy() const;
    
    std::unique_ptr<PianoRollUndoSupport> undoSupport_;
    
    std::unique_ptr<PianoRollRenderer> renderer_;
    std::unique_ptr<PianoRollToolHandler> toolHandler_;
    std::unique_ptr<PianoRollCorrectionWorker> correctionWorker_;
    WaveformMipmap waveformMipmap_;

    static constexpr int pianoKeyWidth_ = 60;
    static constexpr int rulerHeight_ = 30;
    static constexpr int timelineExtendedHitArea_ = 20;
    static constexpr int dragThreshold_ = 5;
    
    bool hasUserAudio_ = false;

    PlayheadOverlayComponent playheadOverlay_;

    std::unique_ptr<juce::VBlankAttachment> scrollVBlankAttachment_;

    std::weak_ptr<std::atomic<double>> positionSource_;

    juce::ListenerList<Listener> listeners_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};

} // namespace OpenTune
