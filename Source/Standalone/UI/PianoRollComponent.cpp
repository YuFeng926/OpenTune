#include "PianoRollComponent.h"
#include "PianoRoll/PianoRollUndoSupport.h"
#include "../Utils/AppLogger.h"
#include <algorithm>
#include <cmath>
#include "../DSP/ScaleInference.h"
#include "../Utils/NoteGenerator.h"
#include "../Utils/SimdPerceptualPitchEstimator.h"
#include "../Utils/ZoomSensitivityConfig.h"
#include "../../PluginProcessor.h"
#include "FrameScheduler.h"
#include "UiText.h"
#include "ToolbarIcons.h"

namespace OpenTune {

void PianoRollComponent::initializeUIComponents() {
    setWantsKeyboardFocus(true);
    addAndMakeVisible(horizontalScrollBar_);
    addAndMakeVisible(verticalScrollBar_);
    horizontalScrollBar_.addListener(this);
    verticalScrollBar_.addListener(this);
    horizontalScrollBar_.setAutoHide(false);
    verticalScrollBar_.setAutoHide(false);

    scrollModeToggleButton_.setButtonText(scrollMode_ == ScrollMode::Continuous ? "Cont" : "Page");
    scrollModeToggleButton_.setFontHeight(11.0f);
    scrollModeToggleButton_.onClick = [this] {
        if (scrollMode_ == ScrollMode::Page) {
            scrollMode_ = ScrollMode::Continuous;
            scrollModeToggleButton_.setButtonText("Cont");
        } else {
            scrollMode_ = ScrollMode::Page;
            scrollModeToggleButton_.setButtonText("Page");
        }
        updateAutoScroll();
    };
    addAndMakeVisible(scrollModeToggleButton_);

    timeUnitToggleButton_.setButtonText("Time");
    timeUnitToggleButton_.setFontHeight(11.0f);
    timeUnitToggleButton_.onClick = [this] {
        if (timeUnit_ == TimeUnit::Seconds) {
            timeUnit_ = TimeUnit::Bars;
            timeUnitToggleButton_.setButtonText("BPM");
        } else {
            timeUnit_ = TimeUnit::Seconds;
            timeUnitToggleButton_.setButtonText("Time");
        }
        repaint();
    };
    addAndMakeVisible(timeUnitToggleButton_);

    addAndMakeVisible(playheadOverlay_);
    playheadOverlay_.setPianoKeyWidth(pianoKeyWidth_);

    scrollVBlankAttachment_ = std::make_unique<juce::VBlankAttachment>(
        this, [this](double timestampSec) { onScrollVBlankCallback(timestampSec); });
}

void PianoRollComponent::initializeUndoSupport() {
    PianoRollUndoSupport::Context undoCtx;
    undoCtx.getNotes = [this]() -> std::vector<Note>& { return noteSequence_.getNotes(); };
    undoCtx.getNotesCopy = [this]() { return noteSequence_.getNotes(); };
    undoCtx.getPitchCurve = [this]() { return currentCurve_; };
    undoCtx.getUndoManager = [this]() { return globalUndoManager_; };
    undoCtx.notifyPitchCurveEdited = [this](int startFrame, int endFrame) {
        listeners_.call([startFrame, endFrame](Listener& l) { l.pitchCurveEdited(startFrame, endFrame); });
    };
    undoCtx.notifyNotesChanged = [this](const std::vector<Note>& notes) {
        listeners_.call([&notes](Listener& l) { l.notesChanged(notes); });
    };
    undoCtx.requestRepaint = [this]() { requestInteractiveRepaint(); };
    undoCtx.updateScrollBars = [this]() { updateScrollBars(); };
    undoSupport_ = std::make_unique<PianoRollUndoSupport>(std::move(undoCtx));
}

void PianoRollComponent::initializeCoordinateSystem() {
    coordSystem_.setBpm(bpm_);
    coordSystem_.setTimeSignature(timeSigNum_, timeSigDenom_);
    coordSystem_.setPianoKeyWidth(pianoKeyWidth_);
    coordSystem_.setHopSize(hopSize_);
    coordSystem_.setF0SampleRate(f0SampleRate_);
}

void PianoRollComponent::initializeRenderer() {
    renderer_ = std::make_unique<PianoRollRenderer>();
}

void PianoRollComponent::initializeCorrectionWorker() {
    correctionWorker_ = std::make_unique<PianoRollCorrectionWorker>();
    correctionWorker_->setApplyCallback([this](PianoRollCorrectionWorker::RequestPtr req) {
        DBG("PianoRollComponent::applyCallback - called");
        if (!req) {
            DBG("PianoRollComponent::applyCallback - req is null");
            requestInteractiveRepaint();
            return;
        }

        auto& request = *req;

        DBG("PianoRollComponent::applyCallback - kind=" + juce::String(static_cast<int>(request.kind))
            + " notes.size=" + juce::String(static_cast<int>(request.notes.size())));

        if (request.kind == PianoRollCorrectionWorker::AsyncCorrectionRequest::Kind::AutoTuneGenerate) {
            DBG("PianoRollComponent::applyCallback - AutoTuneGenerate processing, notes count=" + juce::String(static_cast<int>(request.notes.size())));
            undoSupport_->setNotesEditUndoState(
                std::move(request.autoNotesBefore),
                std::move(request.autoF0Before));

            noteSequence_.replaceRangeWithNotes(
                request.autoStartTime, request.autoEndTime, request.notes);

            undoSupport_->commitNotesEditUndo("Auto Tune");

            const int startFrame = request.autoStartFrame;
            const int endFrame = request.autoEndFrame;
            listeners_.call([startFrame, endFrame](Listener& l) {
                l.pitchCurveEdited(startFrame, endFrame);
            });
            grabKeyboardFocus();
            isAutoTuneProcessing_.store(false, std::memory_order_release);
            requestInteractiveRepaint();
            return;
        }

        if (request.onApplied) {
            request.onApplied();
        }
        requestInteractiveRepaint();
    });
}

PianoRollToolHandler::Context PianoRollComponent::buildToolHandlerContext() {
    PianoRollToolHandler::Context toolCtx;
    toolCtx.xToTime = [this](int x) { return xToTime(x); };
    toolCtx.timeToX = [this](double seconds) { return timeToX(seconds); };
    toolCtx.yToFreq = [this](float y) { return yToFreq(y); };
    toolCtx.freqToY = [this](float f) { return freqToY(f); };

    const double frameDuration = static_cast<double>(hopSize_) / f0SampleRate_;
    toolCtx.clipSecondsToFrameIndex = [this, frameDuration](double seconds) -> double {
        return seconds / frameDuration;
    };
    toolCtx.clipTimeRangeToFrameRangeHalfOpen = [this, frameDuration](double startTime, double endTime, int maxFrameExclusive) -> std::pair<int, int> {
        int startFrame = static_cast<int>(startTime / frameDuration);
        int endFrame = static_cast<int>(endTime / frameDuration);
        if (startFrame < 0) startFrame = 0;
        if (endFrame > maxFrameExclusive) endFrame = maxFrameExclusive;
        if (startFrame > endFrame) startFrame = endFrame;
        return { startFrame, endFrame };
    };

    toolCtx.getNotes = [this]() -> std::vector<Note>& { return noteSequence_.getNotes(); };
    toolCtx.getSelectedNotes = [this]() -> std::vector<Note*> {
        std::vector<Note*> selected;
        for (auto& n : noteSequence_.getNotes()) {
            if (n.selected) selected.push_back(&n);
        }
        return selected;
    };
    toolCtx.findNoteAt = [this](double time, float targetPitchHz, float pitchToleranceHz) -> Note* {
        return noteSequence_.findNoteAt(time, targetPitchHz, pitchToleranceHz);
    };
    toolCtx.deselectAllNotes = [this]() {
        for (auto& n : noteSequence_.getNotes()) n.selected = false;
    };
    toolCtx.selectAllNotes = [this]() {
        for (auto& n : noteSequence_.getNotes()) n.selected = true;
    };
    toolCtx.insertNoteSorted = [this](const Note& note) {
        noteSequence_.insertNoteSorted(note);
    };
    toolCtx.getPitchCurve = [this]() { return currentCurve_; };
    toolCtx.getCurveSize = [this]() -> int {
        if (!currentCurve_) return 0;
        auto snap = currentCurve_->getSnapshot();
        return snap ? static_cast<int>(snap->size()) : 0;
    };
    toolCtx.getCurveHopSize = [this]() { return hopSize_; };
    toolCtx.getCurveSampleRate = [this]() { return f0SampleRate_; };
    toolCtx.clearCorrectionRange = [this](int s, int e) {
        if (!currentCurve_) return;
        currentCurve_->clearCorrectionRange(s, e);
    };
    toolCtx.clearAllCorrections = [this]() {
        if (!currentCurve_) return;
        currentCurve_->clearAllCorrections();
    };
    toolCtx.restoreCorrectedSegment = [this](const CorrectedSegment& seg) {
        if (!currentCurve_) return;
        currentCurve_->restoreCorrectedSegment(seg);
    };
    toolCtx.getOriginalF0 = [this]() -> std::vector<float> {
        if (!currentCurve_) return {};
        auto snap = currentCurve_->getSnapshot();
        if (!snap) return {};
        return snap->getOriginalF0();
    };
    toolCtx.getMinMidi = [this]() { return minMidi_; };
    toolCtx.getMaxMidi = [this]() { return maxMidi_; };
    toolCtx.getRetuneSpeed = [this]() { return currentRetuneSpeed_; };
    toolCtx.getVibratoDepth = [this]() { return currentVibratoDepth_; };
    toolCtx.getVibratoRate = [this]() { return currentVibratoRate_; };
    toolCtx.recalculatePIP = [this](Note& note) -> float { return recalculatePIP(note); };
    toolCtx.setCurrentTool = [this](ToolId tool) { setCurrentTool(tool); };
    toolCtx.showToolSelectionMenu = [this]() {
        juce::PopupMenu menu;
        menu.addItem("Select (3)", [this]() { setCurrentTool(ToolId::Select); });
        menu.addItem("Draw Note (2)", [this]() { setCurrentTool(ToolId::DrawNote); });
        menu.addItem("Line Anchor (4)", [this]() { setCurrentTool(ToolId::LineAnchor); });
        menu.addItem("Hand Draw (5)", [this]() { setCurrentTool(ToolId::HandDraw); });
        menu.showMenuAsync(juce::PopupMenu::Options());
    };
    toolCtx.notifyAutoTuneRequested = [this]() { listeners_.call([](Listener& l) { l.autoTuneRequested(); }); };
    toolCtx.notifyPlayPauseToggle = [this]() { listeners_.call([](Listener& l) { l.playPauseToggleRequested(); }); };
    toolCtx.notifyStopPlayback = [this]() { listeners_.call([](Listener& l) { l.stopPlaybackRequested(); }); };
    toolCtx.notifyEscapeKey = [this]() { listeners_.call([](Listener& l) { l.escapeKeyPressed(); }); };
    toolCtx.notifyNoteOffsetChanged = [this](Note* note, float oldOffset, float newOffset) {
        listeners_.call([note, oldOffset, newOffset](Listener& l) { l.noteOffsetChanged(note, oldOffset, newOffset); });
    };
    toolCtx.undo = [this]() { if (auto* um = getCurrentUndoManager()) um->undo(); };
    toolCtx.redo = [this]() { if (auto* um = getCurrentUndoManager()) um->redo(); };
    toolCtx.addActionToUndoManager = [this](std::unique_ptr<UndoAction> action) {
        if (auto* um = getCurrentUndoManager()) um->addAction(std::move(action));
    };
    toolCtx.isNotesEditUndoActive = [this]() { return undoSupport_ && undoSupport_->isNotesEditUndoActive(); };
    toolCtx.enqueueNoteBasedCorrection = [this](int startFrame, int endFrameExclusive, int, float retuneSpeed, float vibratoDepth, float vibratoRate) {
        if (!currentCurve_) return;
        auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
        request->curve = currentCurve_;
        request->notes = noteSequence_.getNotes();
        request->startFrame = startFrame;
        request->endFrameExclusive = endFrameExclusive;
        request->retuneSpeed = retuneSpeed;
        request->vibratoDepth = vibratoDepth;
        request->vibratoRate = vibratoRate;
        request->audioSampleRate = 44100.0;
        request->onApplied = [this, startFrame, endFrame = endFrameExclusive - 1]() {
            listeners_.call([startFrame, endFrame](Listener& l) { l.pitchCurveEdited(startFrame, endFrame); });
        };
        correctionWorker_->enqueue(request);
    };
    toolCtx.getPianoKeyWidth = [this]() { return pianoKeyWidth_; };
    toolCtx.getTrackOffsetSeconds = [this]() { return static_cast<float>(trackOffsetSeconds_); };
    toolCtx.getAudioSampleRate = [this]() { return PianoRollComponent::kAudioSampleRate; };
    toolCtx.getAudioBuffer = [this]() -> juce::AudioBuffer<float>* { return const_cast<juce::AudioBuffer<float>*>(audioBuffer_); };
    toolCtx.hasSelectionArea = [this]() { return hasSelectionArea_; };
    toolCtx.getSelectionStartTime = [this]() { return selectionStartTime_; };
    toolCtx.getSelectionEndTime = [this]() { return selectionEndTime_; };
    toolCtx.getSelectionStartMidi = [this]() { return selectionStartMidi_; };
    toolCtx.getSelectionEndMidi = [this]() { return selectionEndMidi_; };
    toolCtx.setHasSelectionArea = [this](bool v) { hasSelectionArea_ = v; };
    toolCtx.setSelectionStartTime = [this](double v) { selectionStartTime_ = v; };
    toolCtx.setSelectionEndTime = [this](double v) { selectionEndTime_ = v; };
    toolCtx.setSelectionStartMidi = [this](float v) { selectionStartMidi_ = v; };
    toolCtx.setSelectionEndMidi = [this](float v) { selectionEndMidi_ = v; };

    toolCtx.getIsDrawingF0 = [this]() { return isDrawingF0_; };
    toolCtx.setIsDrawingF0 = [this](bool v) { isDrawingF0_ = v; };
    toolCtx.getHandDrawBuffer = [this]() -> std::vector<float>& { return handDrawBuffer_; };
    toolCtx.getDirtyRangeStart = [this]() { return dirtyRangeStart_; };
    toolCtx.setDirtyRangeStart = [this](int v) { dirtyRangeStart_ = v; };
    toolCtx.getDirtyRangeEnd = [this]() { return dirtyRangeEnd_; };
    toolCtx.setDirtyRangeEnd = [this](int v) { dirtyRangeEnd_ = v; };

    toolCtx.getIsDrawingNote = [this]() { return isDrawingNote_; };
    toolCtx.setIsDrawingNote = [this](bool v) { isDrawingNote_ = v; };
    toolCtx.getDrawingNoteStartTime = [this]() { return drawingNoteStartTime_; };
    toolCtx.setDrawingNoteStartTime = [this](double v) { drawingNoteStartTime_ = v; };
    toolCtx.getDrawingNoteEndTime = [this]() { return drawingNoteEndTime_; };
    toolCtx.setDrawingNoteEndTime = [this](double v) { drawingNoteEndTime_ = v; };
    toolCtx.getDrawingNotePitch = [this]() { return drawingNotePitch_; };
    toolCtx.setDrawingNotePitch = [this](float v) { drawingNotePitch_ = v; };
    toolCtx.getDrawingNoteIndex = [this]() { return drawingNoteIndex_; };
    toolCtx.setDrawingNoteIndex = [this](int v) { drawingNoteIndex_ = v; };

    toolCtx.getIsDraggingNotes = [this]() { return isDraggingNotes_; };
    toolCtx.setIsDraggingNotes = [this](bool v) { isDraggingNotes_ = v; };
    toolCtx.getDrawNoteToolPendingDrag = [this]() { return drawNoteToolPendingDrag_; };
    toolCtx.setDrawNoteToolPendingDrag = [this](bool v) { drawNoteToolPendingDrag_ = v; };
    toolCtx.getDrawNoteToolMouseDownPos = [this]() { return drawNoteToolMouseDownPos_; };
    toolCtx.setDrawNoteToolMouseDownPos = [this](juce::Point<int> v) { drawNoteToolMouseDownPos_ = v; };
    toolCtx.getDragThreshold = [this]() { return dragThreshold_; };

    toolCtx.getHandDrawPendingDrag = [this]() { return handDrawPendingDrag_; };
    toolCtx.setHandDrawPendingDrag = [this](bool v) { handDrawPendingDrag_ = v; };

    toolCtx.getNoteDragManualStartFrame = [this]() { return noteDragManualStartFrame_; };
    toolCtx.setNoteDragManualStartFrame = [this](int v) { noteDragManualStartFrame_ = v; };
    toolCtx.getNoteDragManualEndFrame = [this]() { return noteDragManualEndFrame_; };
    toolCtx.setNoteDragManualEndFrame = [this](int v) { noteDragManualEndFrame_ = v; };
    toolCtx.getNoteDragInitialManualTargets = [this]() -> std::vector<std::pair<int, float>>& { return noteDragInitialManualTargets_; };

    toolCtx.requestRepaint = [this]() { requestInteractiveRepaint(); };
    toolCtx.setMouseCursor = [this](const juce::MouseCursor& c) { setMouseCursor(c); };
    toolCtx.grabKeyboardFocus = [this]() { grabKeyboardFocus(); };
    toolCtx.notifyPlayheadChange = [this](double time) {
        listeners_.call([time](Listener& l) { l.playheadPositionChangeRequested(time); });
        if (!isPlaying_.load(std::memory_order_relaxed)) {
            playheadOverlay_.setPlayheadSeconds(time);
            playheadOverlay_.repaint();
        }
    };
    toolCtx.notifyPitchCurveEdited = [this](int s, int e) {
        listeners_.call([s, e](Listener& l) { l.pitchCurveEdited(s, e); });
    };
    toolCtx.beginNotesEditUndo = [this](const juce::String&) {
        if (undoSupport_) undoSupport_->beginNotesEditUndo();
    };
    toolCtx.commitNotesEditUndo = [this](const juce::String& name) {
        if (undoSupport_) undoSupport_->commitNotesEditUndo(name);
    };
    toolCtx.beginF0EditUndo = [this](const juce::String& name) {
        if (undoSupport_) undoSupport_->beginF0EditUndo(name);
    };
    toolCtx.commitF0EditUndo = [this](const juce::String&) {
        if (undoSupport_) undoSupport_->commitF0EditUndo();
    };
    toolCtx.enqueueManualCorrection = [this](std::vector<PianoRollCorrectionWorker::AsyncCorrectionRequest::ManualOp> ops, int s, int e, bool render) {
        enqueueManualCorrectionPatchAsync(ops, s, e, render);
    };
    toolCtx.isAutoTuneProcessing = [this]() { return isAutoTuneProcessing_.load(); };
    toolCtx.setAutoTuneProcessing = [this](bool v) { isAutoTuneProcessing_.store(v); };
    toolCtx.getIsPlacingAnchors = [this]() { return isPlacingAnchors_; };
    toolCtx.setIsPlacingAnchors = [this](bool v) { isPlacingAnchors_ = v; };
    toolCtx.getPendingAnchors = [this]() -> std::vector<LineAnchor>& { return pendingAnchors_; };
    toolCtx.setCurrentMousePos = [this](const juce::Point<float>& p) { currentMousePos_ = p; };
    toolCtx.clearPendingAnchors = [this]() { pendingAnchors_.clear(); };
    toolCtx.getIsLineAnchorToolActive = [this]() { return currentTool_ == ToolId::LineAnchor; };
    toolCtx.findLineAnchorSegmentNear = [this](int x, int y) { return findLineAnchorSegmentNear(x, y); };
    toolCtx.selectLineAnchorSegment = [this](int idx) { selectLineAnchorSegment(idx); };
    toolCtx.toggleLineAnchorSegmentSelection = [this](int idx) { toggleLineAnchorSegmentSelection(idx); };
    toolCtx.clearLineAnchorSegmentSelection = [this]() { clearLineAnchorSegmentSelection(); };
    return toolCtx;
}

void PianoRollComponent::initializeToolHandler() {
    toolHandler_ = std::make_unique<PianoRollToolHandler>(buildToolHandlerContext());
}

PianoRollComponent::PianoRollComponent() {
    initializeUIComponents();
    initializeUndoSupport();
    initializeCoordinateSystem();
    initializeRenderer();
    initializeCorrectionWorker();
    initializeToolHandler();
}

PianoRollComponent::~PianoRollComponent() {
    if (correctionWorker_) {
        correctionWorker_->stop();
    }
    scrollVBlankAttachment_.reset();
    stopTimer();
    horizontalScrollBar_.removeListener(this);
    verticalScrollBar_.removeListener(this);
}

bool PianoRollComponent::applyCorrectionAsyncForEntireClip(float retuneSpeed, float vibratoDepth, float vibratoRate)
{
    if (!currentCurve_) {
        return false;
    }

    auto snapshot = currentCurve_->getSnapshot();
    const int maxFrame = static_cast<int>(snapshot->size());
    if (maxFrame <= 0) {
        return false;
    }

    auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
    request->curve = currentCurve_;
    request->notes = noteSequence_.getNotes();
    request->startFrame = 0;
    request->endFrameExclusive = maxFrame;
    request->retuneSpeed = retuneSpeed;
    request->vibratoDepth = vibratoDepth;
    request->vibratoRate = vibratoRate;
    request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);
    request->onApplied = [this, startFrame = 0, endFrame = maxFrame - 1]() {
        listeners_.call([startFrame, endFrame](Listener& l) {
            l.pitchCurveEdited(startFrame, endFrame);
        });
        requestInteractiveRepaint();
    };

    correctionWorker_->enqueue(request);
    return true;
}

void PianoRollComponent::enqueueManualCorrectionPatchAsync(const std::vector<PianoRollCorrectionWorker::AsyncCorrectionRequest::ManualOp>& ops,
                                                           int dirtyStartFrame,
                                                           int dirtyEndFrame,
                                                           bool triggerRenderEvent)
{
    if (!currentCurve_ || ops.empty()) {
        return;
    }

    auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
    request->kind = PianoRollCorrectionWorker::AsyncCorrectionRequest::Kind::ManualPatch;
    request->curve = currentCurve_;
    request->manualOps = ops;
    request->startFrame = dirtyStartFrame;
    request->endFrameExclusive = dirtyEndFrame + 1;
    request->onApplied = [this, dirtyStartFrame, dirtyEndFrame, triggerRenderEvent]() {
        if (triggerRenderEvent && dirtyEndFrame >= dirtyStartFrame) {
            listeners_.call([dirtyStartFrame, dirtyEndFrame](Listener& l) {
                l.pitchCurveEdited(dirtyStartFrame, dirtyEndFrame);
            });
        }
        requestInteractiveRepaint();
    };

    correctionWorker_->enqueue(request);
}

void PianoRollComponent::drawSelectedOriginalF0Curve(juce::Graphics& g, const std::vector<float>& originalF0, double offsetSeconds) {
    const auto& notes = noteSequence_.getNotes();
    bool hasNoteSelection = false;
    for (const auto& note : notes) {
        if (note.selected) { hasNoteSelection = true; break; }
    }

    if (!hasNoteSelection && !hasSelectionArea_) return;

    const double frameDuration = hopSize_ / f0SampleRate_;
    juce::Path selectedPath;
    bool pathStarted = false;

    for (const auto& note : notes) {
        if (!note.selected) continue;

        int startFrame = static_cast<int>(note.startTime / frameDuration);
        int endFrame = static_cast<int>(note.endTime / frameDuration);
        startFrame = std::max(0, startFrame);
        endFrame = std::min(static_cast<int>(originalF0.size()) - 1, endFrame);

        if (endFrame <= startFrame) continue;

        bool segmentStarted = false;
        for (int i = startFrame; i <= endFrame; ++i) {
            float f0 = originalF0[i];
            if (f0 > 0.0f) {
                float y = freqToY(f0);
                double timePos = i * frameDuration;
                float x = static_cast<float>(timeToX(timePos + offsetSeconds));

                if (!pathStarted) {
                    selectedPath.startNewSubPath(x, y);
                    pathStarted = true;
                    segmentStarted = true;
                } else if (!segmentStarted) {
                    selectedPath.startNewSubPath(x, y);
                    segmentStarted = true;
                } else {
                    juce::Point<float> last = selectedPath.getCurrentPosition();
                    if (std::abs(x - last.x) > 50.0f) {
                        selectedPath.startNewSubPath(x, y);
                    } else {
                        selectedPath.lineTo(x, y);
                    }
                }
            } else {
                segmentStarted = false;
            }
        }
    }

    if (hasSelectionArea_) {
        double startTime = std::min(selectionStartTime_, selectionEndTime_);
        double endTime = std::max(selectionStartTime_, selectionEndTime_);

        int startFrame = static_cast<int>(startTime / frameDuration);
        int endFrame = static_cast<int>(endTime / frameDuration);
        startFrame = std::max(0, startFrame);
        endFrame = std::min(static_cast<int>(originalF0.size()) - 1, endFrame);

        float minMidi = std::min(selectionStartMidi_, selectionEndMidi_);
        float maxMidi = std::max(selectionStartMidi_, selectionEndMidi_);

        if (endFrame > startFrame) {
            bool segmentStarted = false;
            for (int i = startFrame; i <= endFrame; ++i) {
                float f0 = originalF0[i];
                if (f0 > 0.0f) {
                    float f0Midi = freqToMidi(f0);
                    if (f0Midi >= minMidi && f0Midi <= maxMidi) {
                        float y = freqToY(f0);
                        double timePos = i * frameDuration;
                        float x = static_cast<float>(timeToX(timePos + offsetSeconds));

                        if (!pathStarted) {
                            selectedPath.startNewSubPath(x, y);
                            pathStarted = true;
                            segmentStarted = true;
                        } else if (!segmentStarted) {
                            selectedPath.startNewSubPath(x, y);
                            segmentStarted = true;
                        } else {
                            juce::Point<float> last = selectedPath.getCurrentPosition();
                            if (std::abs(x - last.x) > 50.0f) {
                                selectedPath.startNewSubPath(x, y);
                            } else {
                                selectedPath.lineTo(x, y);
                            }
                        }
                    } else {
                        segmentStarted = false;
                    }
                } else {
                    segmentStarted = false;
                }
            }
        }
    }

    if (!selectedPath.isEmpty()) {
        g.setColour(UIColors::originalF0.withAlpha(0.85f));
        juce::PathStrokeType strokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.strokePath(selectedPath, strokeType);
    }
}

void PianoRollComponent::drawHandDrawPreview(juce::Graphics& g, double offsetSeconds) {
    if (!isDrawingF0_ || currentTool_ != ToolId::HandDraw || handDrawBuffer_.empty() || !currentCurve_) return;

    auto snapshot = currentCurve_->getSnapshot();
    const auto& originalF0 = snapshot->getOriginalF0();
    if (originalF0.empty() || handDrawBuffer_.size() != originalF0.size()) return;

    const double frameDuration = hopSize_ / f0SampleRate_;
    juce::Colour previewColour = juce::Colour(0xFF00DDDD);
    juce::Path previewPath;
    bool pathStarted = false;

    for (size_t i = 0; i < handDrawBuffer_.size(); ++i) {
        float f0 = handDrawBuffer_[i];
        if (f0 > 0.0f) {
            float y = freqToY(f0);
            double timePos = i * frameDuration;
            float x = static_cast<float>(timeToX(timePos + offsetSeconds));

            if (!pathStarted) {
                previewPath.startNewSubPath(x, y);
                pathStarted = true;
            } else {
                juce::Point<float> last = previewPath.getCurrentPosition();
                if (std::abs(x - last.x) > 30.0f) {
                    previewPath.startNewSubPath(x, y);
                } else {
                    previewPath.lineTo(x, y);
                }
            }
        } else if (pathStarted && f0 < -0.5f) {
            pathStarted = false;
        }
    }

    if (!previewPath.isEmpty()) {
        g.setColour(previewColour.withAlpha(0.85f));
        juce::PathStrokeType strokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.strokePath(previewPath, strokeType);
    }
}

void PianoRollComponent::drawLineAnchorPreview(juce::Graphics& g, double offsetSeconds) {
    if (!isPlacingAnchors_ || currentTool_ != ToolId::LineAnchor || pendingAnchors_.empty()) return;

    juce::Colour anchorColour = UIColors::correctedF0;

    for (size_t i = 0; i < pendingAnchors_.size(); ++i) {
        const auto& anchor = pendingAnchors_[i];
        float x = static_cast<float>(timeToX(anchor.time + offsetSeconds));
        float y = freqToY(anchor.freq);

        g.setColour(anchorColour);
        g.fillEllipse(x - 2.0f, y - 2.0f, 4.0f, 4.0f);

        if (i > 0) {
            const auto& prev = pendingAnchors_[i - 1];
            float prevX = static_cast<float>(timeToX(prev.time + offsetSeconds));
            float prevY = freqToY(prev.freq);
            g.setColour(anchorColour.withAlpha(0.7f));
            g.drawLine(prevX, prevY, x, y, 2.0f);
        }
    }

    if (!pendingAnchors_.empty()) {
        const auto& last = pendingAnchors_.back();
        float lastX = static_cast<float>(timeToX(last.time + offsetSeconds));
        float lastY = freqToY(last.freq);
        g.setColour(anchorColour.withAlpha(0.4f));
        g.drawLine(lastX, lastY, currentMousePos_.x, currentMousePos_.y, 1.5f);
    }
}

void PianoRollComponent::drawSelectionBox(juce::Graphics& g, double offsetSeconds, ThemeId themeId) {
    if (!hasSelectionArea_) return;
    if (!toolHandler_ || !toolHandler_->isSelectingArea()) return;

    double startTime = std::min(selectionStartTime_, selectionEndTime_);
    double endTime = std::max(selectionStartTime_, selectionEndTime_);
    float minMidi = std::min(selectionStartMidi_, selectionEndMidi_);
    float maxMidi = std::max(selectionStartMidi_, selectionEndMidi_);

    int x1 = timeToX(startTime + offsetSeconds);
    int x2 = timeToX(endTime + offsetSeconds);
    float y1 = midiToY(maxMidi);
    float y2 = midiToY(minMidi);

    float left = static_cast<float>(std::min(x1, x2));
    float top = std::min(y1, y2);
    float width = static_cast<float>(std::abs(x2 - x1));
    float height = std::abs(y2 - y1);

    juce::Rectangle<float> rect(left, top, width, height);
    juce::Colour fill = UIColors::lightPurple;
    juce::Colour stroke = UIColors::lightPurple;
    float fillAlpha = 0.12f;
    float strokeAlpha = 0.5f;
    float strokeThickness = 1.0f;

    if (themeId == ThemeId::DarkBlueGrey) {
        fill = juce::Colours::white;
        stroke = juce::Colours::white;
        fillAlpha = 0.20f;
        strokeAlpha = 0.90f;
        strokeThickness = 2.0f;
    }

    g.setColour(fill.withAlpha(fillAlpha));
    g.fillRoundedRectangle(rect, 3.0f);
    g.setColour(stroke.withAlpha(strokeAlpha));
    g.drawRoundedRectangle(rect, 3.0f, strokeThickness);
}

void PianoRollComponent::drawRenderingProgress(juce::Graphics& g) {
    if (!isRendering_) return;

    int margin = 10;
    int topY = 30;
    int rightX = getWidth() - margin - 30;

    const float spinnerRadius = 6.0f;
    const float spinnerX = static_cast<float>(rightX - 120);
    const float spinnerY = static_cast<float>(topY + 10);
    const float spinnerThickness = 1.5f;

    const double currentTime = juce::Time::getMillisecondCounterHiRes();
    const float rotationPhase = static_cast<float>(std::fmod(currentTime * 0.003, juce::MathConstants<double>::twoPi));

    g.setColour(UIColors::textPrimary.withAlpha(0.2f));
    g.drawEllipse(spinnerX - spinnerRadius, spinnerY - spinnerRadius,
                  spinnerRadius * 2.0f, spinnerRadius * 2.0f, spinnerThickness);

    juce::Path arcPath;
    const float arcLength = juce::MathConstants<float>::pi * 1.5f;
    arcPath.addCentredArc(spinnerX, spinnerY, spinnerRadius, spinnerRadius,
                          rotationPhase, 0.0f, arcLength, true);
    g.setColour(UIColors::textPrimary.withAlpha(0.85f));
    g.strokePath(arcPath, juce::PathStrokeType(spinnerThickness,
                 juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour(UIColors::textPrimary);
    g.setFont(UIColors::getUIFont(12.0f));
    g.drawText(juce::String::fromUTF8("音符渲染中"), static_cast<int>(spinnerX + spinnerRadius + 8), topY + 5, 100, 15,
               juce::Justification::topLeft, false);
}

void PianoRollComponent::paint(juce::Graphics& g) {
    AppLogger::debug("[PianoRollComponent] paint: starting");
    auto ctx = buildRenderContext();
    auto bounds = getLocalBounds().toFloat().reduced(12.0f);
    const auto themeId = Theme::getActiveTheme();

    UIColors::drawShadow(g, bounds);

    juce::Path backgroundPath;
    backgroundPath.addRoundedRectangle(bounds, UIColors::cornerRadius);
    g.reduceClipRegion(backgroundPath);

    if (themeId == ThemeId::DarkBlueGrey)
        UIColors::fillSoothe2SpectrumBackground(g, bounds, UIColors::cornerRadius);
    else
        g.setColour(UIColors::rollBackground);

    if (themeId != ThemeId::DarkBlueGrey)
        g.fillPath(backgroundPath);

    renderer_->drawTimeRuler(g, ctx);

    {
        const juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(juce::Rectangle<int>(pianoKeyWidth_, 0, getWidth() - pianoKeyWidth_, getHeight()));

        renderer_->drawGridLines(g, ctx);

        if (showWaveform_ && hasUserAudio_) {
            renderer_->drawWaveform(g, ctx);
        }

        renderer_->drawLanes(g, ctx);

        renderer_->drawNotes(g, ctx, noteSequence_.getNotes(), trackOffsetSeconds_);

        if (currentCurve_ != nullptr) {
            if (showOriginalF0_) {
                auto snapshot = currentCurve_->getSnapshot();
                const auto& originalF0 = snapshot->getOriginalF0();
                if (!originalF0.empty()) {
                    renderer_->drawF0Curve(g, originalF0, UIColors::originalF0, 0.55f, true, ctx, currentCurve_);
                    drawSelectedOriginalF0Curve(g, originalF0, trackOffsetSeconds_);
                }
            }

            if (showCorrectedF0_) {
                auto currentSnapshot = currentCurve_->getSnapshot();
                const int totalFrames = static_cast<int>(currentSnapshot->size());
                if (totalFrames > 0 && currentSnapshot->hasAnyCorrection()) {
                    renderer_->updateCorrectedF0Cache(currentSnapshot);
                    renderer_->drawF0Curve(g, renderer_->getCorrectedF0Cache(), UIColors::correctedF0, 1.0f, false, ctx, currentCurve_, nullptr);
                }
            }

            drawHandDrawPreview(g, trackOffsetSeconds_);
            drawLineAnchorPreview(g, trackOffsetSeconds_);
        }
    }

    renderer_->drawPianoKeys(g, ctx);

    drawSelectionBox(g, trackOffsetSeconds_, themeId);
    drawRenderingProgress(g);

    AppLogger::debug("[PianoRollComponent] paint: completed");
}

void PianoRollComponent::setRenderingProgress(float progress, int pendingTasks) {
    bool wasRendering = isRendering_;
    renderingProgress_ = progress;
    pendingRenderTasks_ = pendingTasks;
    isRendering_ = (pendingTasks > 0) || (progress > 0.0f);

    if (isRendering_)
        startTimerHz(30);
    else if (!inferenceActive_)
        stopTimer();
    
    if (wasRendering != isRendering_ || isRendering_) {
        repaint();
    }
}

void PianoRollComponent::setInferenceActive(bool active)
{
    inferenceActive_ = active;
    waveformBuildTickCounter_ = 0;

    if (isRendering_)
        startTimerHz(30);
    else
        stopTimer();
}

bool PianoRollComponent::applyRetuneSpeedToSelectedNotes(float speed) {
    bool hasSelectedNotes = false;
    for (const auto& n : noteSequence_.getNotes()) {
        if (n.selected) { hasSelectedNotes = true; break; }
    }
    if (!hasSelectedNotes) return false;

    undoSupport_->beginNotesEditUndo();
    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;
    const double frameDuration = hopSize_ / f0SampleRate_;
    for (auto& n : noteSequence_.getNotes()) {
        if (!n.selected) continue;
        n.retuneSpeed = speed;
        n.dirty = true;
        dirtyStartTime = std::min(dirtyStartTime, n.startTime);
        dirtyEndTime = std::max(dirtyEndTime, n.endTime);
    }

    if (dirtyEndTime > dirtyStartTime) {
        int startFrame = static_cast<int>(dirtyStartTime / frameDuration);
        int endFrame = static_cast<int>(dirtyEndTime / frameDuration);
        if (startFrame < 0) startFrame = 0;
        if (endFrame < startFrame) endFrame = startFrame;

        auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
        request->curve = currentCurve_;
        request->notes = noteSequence_.getNotes();
        request->startFrame = startFrame;
        request->endFrameExclusive = endFrame + 1;
        request->retuneSpeed = speed;
        request->vibratoDepth = currentVibratoDepth_;
        request->vibratoRate = currentVibratoRate_;
        request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);
        request->onApplied = [this, startFrame, endFrame]() {
            listeners_.call([startFrame, endFrame](Listener& l) { l.pitchCurveEdited(startFrame, endFrame); });
        };
        correctionWorker_->enqueue(request);
    }
    undoSupport_->commitNotesEditUndo("Retune Speed");
    repaint();
    return true;
}

bool PianoRollComponent::applyRetuneSpeedToSelectionArea(float speed, int startFrame, int endFrame) {
    bool hasVisibleCorrectionInRange = currentCurve_->hasCorrectionInRange(startFrame, endFrame);
    if (!hasVisibleCorrectionInRange) return false;

    auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
    request->curve = currentCurve_;
    request->notes = noteSequence_.getNotes();
    request->startFrame = startFrame;
    request->endFrameExclusive = endFrame + 1;
    request->retuneSpeed = speed;
    request->vibratoDepth = currentVibratoDepth_;
    request->vibratoRate = currentVibratoRate_;
    request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);
    request->onApplied = [this, startFrame, endFrame]() {
        listeners_.call([startFrame, endFrame](Listener& l) { l.pitchCurveEdited(startFrame, endFrame); });
        requestInteractiveRepaint();
    };
    correctionWorker_->enqueue(request);
    repaint();
    return true;
}

bool PianoRollComponent::applyRetuneSpeedToSelection(float speed) {
    speed = juce::jlimit(0.0f, 1.0f, speed);
    if (!currentCurve_) return false;

    int selectionStartFrame = -1;
    int selectionEndFrame = -1;
    const double frameDuration = hopSize_ / f0SampleRate_;
    if (hasSelectionArea_) {
        const double selStart = std::min(selectionStartTime_, selectionEndTime_);
        const double selEnd = std::max(selectionStartTime_, selectionEndTime_);
        selectionStartFrame = static_cast<int>(selStart / frameDuration);
        selectionEndFrame = static_cast<int>(selEnd / frameDuration);
        if (selectionStartFrame < 0) selectionStartFrame = 0;
        if (selectionEndFrame < selectionStartFrame) selectionEndFrame = selectionStartFrame;

        if (hasHandDrawCorrectionInRange(selectionStartFrame, selectionEndFrame + 1)) {
            return true;
        }
    }

    if (applyRetuneSpeedToSelectedNotes(speed)) return true;

    if (hasSelectionArea_) {
        return applyRetuneSpeedToSelectionArea(speed, selectionStartFrame, selectionEndFrame);
    }

    return false;
}

bool PianoRollComponent::applyRetuneSpeedToSelectedLineAnchorSegments(float speed) {
    if (currentCurve_ == nullptr || selectedLineAnchorSegmentIds_.empty()) {
        return false;
    }

    auto snapshot = currentCurve_->getSnapshot();
    const auto& allSegments = snapshot->getCorrectedSegments();

    int modifiedCount = 0;
    int affectedStartFrame = INT_MAX;
    int affectedEndFrame = -1;

    for (int idx : selectedLineAnchorSegmentIds_) {
        if (idx < 0 || idx >= static_cast<int>(allSegments.size())) continue;
        const auto& seg = allSegments[idx];
        if (seg.source != CorrectedSegment::Source::LineAnchor) continue;

        modifiedCount++;
        affectedStartFrame = std::min(affectedStartFrame, seg.startFrame);
        affectedEndFrame = std::max(affectedEndFrame, seg.endFrame);
    }

    if (modifiedCount == 0) {
        return false;
    }

    undoSupport_->beginF0EditUndo("Line Anchor Retune Speed");

    for (int idx : selectedLineAnchorSegmentIds_) {
        if (idx < 0 || idx >= static_cast<int>(allSegments.size())) continue;
        const auto& seg = allSegments[idx];
        if (seg.source != CorrectedSegment::Source::LineAnchor) continue;

        CorrectedSegment updatedSeg = seg;
        updatedSeg.retuneSpeed = juce::jlimit(0.0f, 1.0f, speed);
        currentCurve_->restoreCorrectedSegment(updatedSeg);
    }

    if (affectedStartFrame <= affectedEndFrame) {
        listeners_.call([affectedStartFrame, affectedEndFrame](Listener& l) {
            l.pitchCurveEdited(affectedStartFrame, affectedEndFrame);
        });
        requestInteractiveRepaint();
    }

    undoSupport_->commitF0EditUndo();
    return true;
}

bool PianoRollComponent::applyVibratoDepthToSelection(float depth) {
    return applyVibratoParameterToSelection(VibratoParam::Depth, depth);
}

bool PianoRollComponent::applyVibratoRateToSelection(float rate) {
    return applyVibratoParameterToSelection(VibratoParam::Rate, rate);
}

bool PianoRollComponent::applyVibratoParameterToSelection(VibratoParam param, float value) {
    auto clampValue = [&]() -> float {
        return (param == VibratoParam::Depth) ? juce::jlimit(0.0f, 100.0f, value)
                                              : juce::jlimit(0.1f, 30.0f, value);
    };
    
    value = clampValue();
    if (!currentCurve_) return false;
    
    bool hasSelectedNotes = false;
    for (const auto& n : noteSequence_.getNotes()) {
        if (n.selected) { hasSelectedNotes = true; break; }
    }
    if (!hasSelectedNotes) return false;
    
    undoSupport_->beginNotesEditUndo();
    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;
    const double frameDuration = hopSize_ / f0SampleRate_;
    
    for (auto& n : noteSequence_.getNotes()) {
        if (!n.selected) continue;
        if (param == VibratoParam::Depth) {
            n.vibratoDepth = value;
        } else {
            n.vibratoRate = value;
        }
        n.dirty = true;
        dirtyStartTime = std::min(dirtyStartTime, n.startTime);
        dirtyEndTime = std::max(dirtyEndTime, n.endTime);
    }

    if (dirtyEndTime > dirtyStartTime) {
        int startFrame = static_cast<int>(dirtyStartTime / frameDuration);
        int endFrame = static_cast<int>(dirtyEndTime / frameDuration);
        if (startFrame < 0) startFrame = 0;
        if (endFrame < startFrame) endFrame = startFrame;

        auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
        request->curve = currentCurve_;
        request->notes = noteSequence_.getNotes();
        request->startFrame = startFrame;
        request->endFrameExclusive = endFrame + 1;
        request->retuneSpeed = currentRetuneSpeed_;
        request->vibratoDepth = (param == VibratoParam::Depth) ? value : currentVibratoDepth_;
        request->vibratoRate = (param == VibratoParam::Rate) ? value : currentVibratoRate_;
        request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);
        request->onApplied = [this, startFrame, endFrame]() {
            listeners_.call([startFrame, endFrame](Listener& l) { l.pitchCurveEdited(startFrame, endFrame); });
        };
        correctionWorker_->enqueue(request);
    }
    
    const char* undoLabel = (param == VibratoParam::Depth) ? "Vibrato Depth" : "Vibrato Rate";
    undoSupport_->commitNotesEditUndo(undoLabel);
    
    if (param == VibratoParam::Depth) {
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Normal);
    } else {
        repaint();
    }
    return true;
}

bool PianoRollComponent::getSingleSelectedNoteParameters(float& retuneSpeedPercent, float& vibratoDepth, float& vibratoRate) const
{
    const auto& notes = noteSequence_.getNotes();
    const Note* selectedNote = nullptr;

    for (const auto& note : notes) {
        if (!note.selected) {
            continue;
        }

        if (selectedNote != nullptr) {
            return false;
        }

        selectedNote = &note;
    }

    if (selectedNote == nullptr) {
        return false;
    }

    const float resolvedRetuneSpeed = selectedNote->retuneSpeed >= 0.0f ? selectedNote->retuneSpeed : currentRetuneSpeed_;
    const float resolvedVibratoDepth = selectedNote->vibratoDepth >= 0.0f ? selectedNote->vibratoDepth : currentVibratoDepth_;
    const float resolvedVibratoRate = selectedNote->vibratoRate >= 0.0f ? selectedNote->vibratoRate : currentVibratoRate_;

    retuneSpeedPercent = juce::jlimit(0.0f, 100.0f, resolvedRetuneSpeed * 100.0f);
    vibratoDepth = juce::jlimit(0.0f, 100.0f, resolvedVibratoDepth);
    vibratoRate = juce::jlimit(3.0f, 12.0f, resolvedVibratoRate);
    return true;
}

bool PianoRollComponent::getSelectedSegmentRetuneSpeed(float& retuneSpeedPercent) const
{
    if (currentCurve_ == nullptr || selectedLineAnchorSegmentIds_.empty()) {
        return false;
    }

    auto snapshot = currentCurve_->getSnapshot();
    const auto& allSegments = snapshot->getCorrectedSegments();

    if (selectedLineAnchorSegmentIds_.size() != 1) {
        return false;
    }

    int idx = selectedLineAnchorSegmentIds_[0];
    if (idx < 0 || idx >= static_cast<int>(allSegments.size())) {
        return false;
    }

    const auto& seg = allSegments[idx];
    if (seg.source != CorrectedSegment::Source::LineAnchor) {
        return false;
    }

    if (seg.retuneSpeed < 0.0f) {
        retuneSpeedPercent = currentRetuneSpeed_ * 100.0f;
    } else {
        retuneSpeedPercent = seg.retuneSpeed * 100.0f;
    }

    retuneSpeedPercent = juce::jlimit(0.0f, 100.0f, retuneSpeedPercent);
    return true;
}

int PianoRollComponent::findLineAnchorSegmentNear(int x, int y) const
{
    if (currentCurve_ == nullptr) return -1;

    auto snapshot = currentCurve_->getSnapshot();
    const auto& allSegments = snapshot->getCorrectedSegments();

    const double frameDuration = static_cast<double>(hopSize_) / f0SampleRate_;
    const float tolerancePixels = 15.0f;
    const double clickTime = xToTime(x);

    int bestIdx = -1;
    float bestDist = tolerancePixels;

    for (int i = 0; i < static_cast<int>(allSegments.size()); ++i) {
        const auto& seg = allSegments[i];
        if (seg.source != CorrectedSegment::Source::LineAnchor) continue;
        if (seg.f0Data.empty()) continue;

        const double startTime = static_cast<double>(seg.startFrame) * frameDuration;
        const double endTime = static_cast<double>(seg.endFrame) * frameDuration;

        const int startX = timeToX(startTime + trackOffsetSeconds_);
        const int endX = timeToX(endTime + trackOffsetSeconds_);

        if (x < startX - tolerancePixels || x > endX + tolerancePixels) continue;

        const double relT = juce::jlimit(0.0, 1.0, (clickTime - startTime) / (endTime - startTime));
        const int f0Idx = juce::jlimit(0, static_cast<int>(seg.f0Data.size()) - 1,
                                       static_cast<int>(relT * (seg.f0Data.size() - 1)));

        const float segFreq = seg.f0Data[f0Idx];
        if (segFreq <= 0.0f) continue;

        const float segY = freqToY(segFreq);
        const float dist = std::abs(segY - static_cast<float>(y));

        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = i;
        }
    }

    return bestIdx;
}

void PianoRollComponent::selectLineAnchorSegment(int idx)
{
    selectedLineAnchorSegmentIds_.clear();
    if (idx >= 0) {
        selectedLineAnchorSegmentIds_.push_back(idx);
    }
}

void PianoRollComponent::toggleLineAnchorSegmentSelection(int idx)
{
    auto it = std::find(selectedLineAnchorSegmentIds_.begin(), selectedLineAnchorSegmentIds_.end(), idx);
    if (it != selectedLineAnchorSegmentIds_.end()) {
        selectedLineAnchorSegmentIds_.erase(it);
    } else {
        selectedLineAnchorSegmentIds_.push_back(idx);
    }
}

void PianoRollComponent::clearLineAnchorSegmentSelection()
{
    selectedLineAnchorSegmentIds_.clear();
}

void PianoRollComponent::setNoteSplit(float value) {
    // Note Split 控制音高分段阈值（cents）
    segmentationPolicy_.transitionThresholdCents = juce::jlimit(
        OpenTune::PitchControlConfig::kMinNoteSplitCents,
        OpenTune::PitchControlConfig::kMaxNoteSplitCents,
        value);

    // Note Split 仅更新分段策略参数，不触发 AUTO 重新生成。
    // AUTO 操作由用户主动触发，使用当前策略执行分段。
    requestInteractiveRepaint();
}

bool PianoRollComponent::hasHandDrawCorrectionInRange(int startFrame, int endFrame) const {
    if (currentCurve_ == nullptr || startFrame >= endFrame) {
        return false;
    }

    auto snapshot = currentCurve_->getSnapshot();
    const auto& segments = snapshot->getCorrectedSegments();
    for (const auto& seg : segments) {
        if (seg.source != CorrectedSegment::Source::HandDraw) {
            continue;
        }
        if (seg.endFrame <= startFrame || seg.startFrame >= endFrame) {
            continue;
        }
        return true;
    }
    return false;
}

void PianoRollComponent::resized() {
    auto bounds = getLocalBounds().reduced(12);

    // Reserve space for scrollbars
    horizontalScrollBar_.setBounds(bounds.removeFromBottom(15));
    verticalScrollBar_.setBounds(bounds.removeFromRight(15));

    updateScrollBars();

    // Position toggle buttons in top right of ruler
    int btnW = 50;
    int btnH = 20;
    int spacing = 5;
    int currentX = getWidth() - spacing - btnW;
    
    scrollModeToggleButton_.setBounds(currentX, 5, btnW, btnH);
    currentX -= (btnW + spacing);
    timeUnitToggleButton_.setBounds(currentX, 5, btnW, btnH);

    // 播放头覆盖层覆盖整个组件区域
    playheadOverlay_.setBounds(getLocalBounds());
}

void PianoRollComponent::setPitchCurve(std::shared_ptr<PitchCurve> curve) {
    currentCurve_ = curve;
    if (renderer_) {
        renderer_->clearCorrectedF0Cache();
    }

    hasSelectionArea_ = false;
    selectionStartTime_ = 0.0;
    selectionEndTime_ = 0.0;
    selectionStartMidi_ = 0.0f;
    selectionEndMidi_ = 0.0f;

    if (curve) {
        int curveHopSize = curve->getHopSize();
        if (curveHopSize > 0) {
            setHopSize(curveHopSize);
        }

        double curveSampleRate = curve->getSampleRate();
        if (curveSampleRate > 0.0) {
            setF0SampleRate(curveSampleRate);
        }
    } else {
        noteSequence_.clear();
    }
    
    snapNextScroll_ = true;
    repaint();
}

void PianoRollComponent::setAudioBuffer(const juce::AudioBuffer<float>* buffer, int sampleRate) {
    audioBuffer_ = buffer;
    
    timeConverter_.setContext(bpm_, timeSigNum_, timeSigDenom_);
    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);
    
    playheadOverlay_.setZoomLevel(zoomLevel_);
    playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));

    renderer_->prepareWaveformThumbCache(buffer, kAudioSampleRate);

    snapNextScroll_ = true;
    updateScrollBars();
    repaint();
}

void PianoRollComponent::requestInteractiveRepaint() {
    requestInteractiveRepaint(getLocalBounds());
}

void PianoRollComponent::requestInteractiveRepaint(const juce::Rectangle<int>& dirtyArea) {
    auto clippedDirtyArea = dirtyArea;
    if (clippedDirtyArea.isEmpty()) {
        clippedDirtyArea = getLocalBounds();
    } else {
        clippedDirtyArea = clippedDirtyArea.getIntersection(getLocalBounds());
    }

    if (clippedDirtyArea.isEmpty()) {
        return;
    }

    // 交互重绘节流：限制到约 60fps，避免鼠标拖拽时重复 repaint 导致主线程抖动。
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    constexpr double minIntervalMs = 1000.0 / 60.0;
    if ((nowMs - lastInteractiveRepaintMs_) >= minIntervalMs) {
        lastInteractiveRepaintMs_ = nowMs;
        pendingInteractiveRepaint_ = false;
        hasPendingInteractiveDirtyArea_ = false;
        pendingInteractiveDirtyArea_ = {};
        FrameScheduler::instance().requestInvalidate(*this, clippedDirtyArea, FrameScheduler::Priority::Interactive);
        return;
    }

    if (hasPendingInteractiveDirtyArea_) {
        pendingInteractiveDirtyArea_ = pendingInteractiveDirtyArea_.getUnion(clippedDirtyArea);
    } else {
        pendingInteractiveDirtyArea_ = clippedDirtyArea;
        hasPendingInteractiveDirtyArea_ = true;
    }

    pendingInteractiveRepaint_ = true;
}

void PianoRollComponent::setScrollOffset(int offset) {
    const int newOffset = juce::jmax(0, offset);
    if (newOffset == scrollOffset_) return;
    
    const int oldOffset = scrollOffset_;
    scrollOffset_ = newOffset;
    coordSystem_.setScrollOffset(scrollOffset_);
    timeConverter_.setScrollOffset(scrollOffset_);
    horizontalScrollBar_.setCurrentRangeStart(scrollOffset_);
    playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));
    
    const int scrollDelta = scrollOffset_ - oldOffset;
    const int contentWidth = getWidth() - pianoKeyWidth_;
    if (contentWidth > 0 && std::abs(scrollDelta) < contentWidth) {
        juce::Rectangle<int> dirtyArea(pianoKeyWidth_, 0, contentWidth, getHeight());
        FrameScheduler::instance().requestInvalidate(*this, dirtyArea, FrameScheduler::Priority::Interactive);
    } else {
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
    }
}

double PianoRollComponent::readPlayheadTime() const
{
    if (auto source = positionSource_.lock()) {
        return source->load(std::memory_order_relaxed);
    }
    return 0.0;
}

void PianoRollComponent::updateAutoScroll() {
    if (!isPlaying_.load(std::memory_order_relaxed)) {
        return;
    }

    const double playheadTime = readPlayheadTime();

    if (scrollMode_ == ScrollMode::Page) {
        double pixelsPerSecond = 100.0 * zoomLevel_;
        int playheadVisualX = static_cast<int>(playheadTime * pixelsPerSecond) - scrollOffset_ + pianoKeyWidth_;

        if (playheadVisualX >= getWidth()) {
            int visibleW = getWidth() - pianoKeyWidth_;
            setScrollOffset(scrollOffset_ + visibleW);
            smoothScrollCurrent_ = static_cast<float>(scrollOffset_);
        } else if (playheadVisualX < pianoKeyWidth_) {
            int absX = static_cast<int>(playheadTime * pixelsPerSecond);
            
            int visibleW = getWidth() - pianoKeyWidth_;
            int pageIndex = absX / visibleW;
            int newScroll = pageIndex * visibleW;
            
            setScrollOffset(newScroll);
            smoothScrollCurrent_ = static_cast<float>(newScroll);
        }
    }
}

void PianoRollComponent::timerCallback() {
    if (isShowing() && isRendering_)
        repaint();
}

void PianoRollComponent::onHeartbeatTick() {
    if (!isShowing()) {
        return;
    }

    timeConverter_.setContext(bpm_, timeSigNum_, timeSigDenom_);
    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);

    timeConverter_.setContext(bpm_, timeSigNum_, timeSigDenom_);
    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);

    if (pendingInteractiveRepaint_) {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        constexpr double minIntervalMs = 1000.0 / 60.0;
        if (!isPlaying_.load(std::memory_order_relaxed) || (nowMs - lastInteractiveRepaintMs_) >= minIntervalMs) {
            lastInteractiveRepaintMs_ = nowMs;
            pendingInteractiveRepaint_ = false;
            if (hasPendingInteractiveDirtyArea_) {
                auto dirtyArea = pendingInteractiveDirtyArea_.getIntersection(getLocalBounds());
                hasPendingInteractiveDirtyArea_ = false;
                pendingInteractiveDirtyArea_ = {};
                if (!dirtyArea.isEmpty()) {
                    FrameScheduler::instance().requestInvalidate(*this, dirtyArea, FrameScheduler::Priority::Interactive);
                } else {
                    FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
                }
            } else {
                FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
            }
        }
    }

    // Repaint for spinner animation when rendering
    if (isRendering_) {
        repaint();
    }

    // 非关键路径：推理活跃时显著降频并缩小时间片。
    if (!renderer_->getWaveformThumbCache().thumbComplete && showWaveform_) {
        if (inferenceActive_) {
            waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 8;
            if (waveformBuildTickCounter_ == 0 && renderer_->buildWaveformThumbSlice(1.0)) {
                repaint();
            }
        } else {
            waveformBuildTickCounter_ = 0;
            if (renderer_->buildWaveformThumbSlice(5.0)) {
                repaint();
            }
        }
    }

}

void PianoRollComponent::onScrollVBlankCallback(double timestampSec)
{
    juce::ignoreUnused(timestampSec);

    if (!isShowing() || !isPlaying_.load(std::memory_order_relaxed))
        return;

    const double playheadTime = readPlayheadTime();
    playheadOverlay_.setPlayheadSeconds(playheadTime);

    if (scrollMode_ == ScrollMode::Continuous) {
        const double pixelsPerSecond = 100.0 * zoomLevel_;
        const float playheadAbsX = static_cast<float>(playheadTime * pixelsPerSecond);

        const float viewCenter = pianoKeyWidth_ + (getWidth() - pianoKeyWidth_) / 2.0f;
        float targetScroll = playheadAbsX + pianoKeyWidth_ - viewCenter;
        if (targetScroll < 0.0f)
            targetScroll = 0.0f;

        const bool isEditingNow = userIsInteracting_ || isDragging_ || isDrawingNote_ || isResizingNote_ || isPanning_;
        if (snapNextScroll_) {
            smoothScrollCurrent_ = targetScroll;
            snapNextScroll_ = false;
        }

        if (isEditingNow) {
            smoothScrollCurrent_ = targetScroll;
        } else {
            const float diff = targetScroll - smoothScrollCurrent_;
            if (std::abs(diff) < 1.0f) {
                smoothScrollCurrent_ = targetScroll;
            } else {
                smoothScrollCurrent_ += diff * 0.2f;
            }
        }

        const int newScrollInt = static_cast<int>(std::llround(smoothScrollCurrent_));
        if (newScrollInt != scrollOffset_) {
            setScrollOffset(newScrollInt);
        }
        return;
    }

    if (scrollMode_ == ScrollMode::Page) {
        double pixelsPerSecond = 100.0 * zoomLevel_;
        int playheadVisualX = static_cast<int>(playheadTime * pixelsPerSecond) - scrollOffset_ + pianoKeyWidth_;

        if (playheadVisualX >= getWidth()) {
            int visibleW = getWidth() - pianoKeyWidth_;
            setScrollOffset(scrollOffset_ + visibleW);
            smoothScrollCurrent_ = static_cast<float>(scrollOffset_);
        } else if (playheadVisualX < pianoKeyWidth_) {
            int absX = static_cast<int>(playheadTime * pixelsPerSecond);

            int visibleW = getWidth() - pianoKeyWidth_;
            int pageIndex = absX / visibleW;
            int newScroll = pageIndex * visibleW;

            setScrollOffset(newScroll);
            smoothScrollCurrent_ = static_cast<float>(newScroll);
        }
    }
}

void PianoRollComponent::setZoomLevel(double zoom) {
    zoomLevel_ = juce::jlimit(0.02, 10.0, zoom);
    timeConverter_.setZoom(zoomLevel_);
    coordSystem_.setZoomLevel(zoomLevel_);
    playheadOverlay_.setZoomLevel(zoomLevel_);
    updateScrollBars();
    requestInteractiveRepaint();
}

void PianoRollComponent::setCurrentTool(ToolId tool) {
    if (isPlacingAnchors_ && tool != ToolId::LineAnchor) {
        if (undoSupport_) undoSupport_->commitF0EditUndo();
        isPlacingAnchors_ = false;
        pendingAnchors_.clear();
    }

    currentTool_ = tool;
    if (toolHandler_) {
        toolHandler_->setTool(tool);
    }

    switch (tool) {
        case ToolId::Select:
            setMouseCursor(juce::MouseCursor::NormalCursor);
            break;
        case ToolId::DrawNote:
        case ToolId::LineAnchor:
            setMouseCursor(juce::MouseCursor::CrosshairCursor);
            break;
        case ToolId::HandDraw:
            setMouseCursor(juce::MouseCursor::CrosshairCursor);
            break;
        case ToolId::AutoTune:
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            break;
    }
}

bool PianoRollComponent::selectToolByContextMenuCommand(int commandId)
{
    switch (commandId) {
        case kContextMenuCommandSelect:
            setCurrentTool(ToolId::Select);
            return true;
        case kContextMenuCommandDrawNote:
            setCurrentTool(ToolId::DrawNote);
            return true;
        case kContextMenuCommandHandDraw:
            setCurrentTool(ToolId::HandDraw);
            return true;
        default:
            return false;
    }
}

void PianoRollComponent::setShowWaveform(bool shouldShow) {
    showWaveform_ = shouldShow;
    repaint();
}

void PianoRollComponent::setShowLanes(bool shouldShow) {
    showLanes_ = shouldShow;
    repaint();
}

void PianoRollComponent::setBpm(double bpm) {
    bpm_ = juce::jlimit(60.0, 240.0, bpm);
    coordSystem_.setBpm(bpm_);
    timeConverter_.setContext(bpm_, timeSigNum_, timeSigDenom_);
    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);
    repaint();
}

void PianoRollComponent::setTimeSignature(int numerator, int denominator) {
    if (numerator <= 0 || denominator <= 0) {
        return;
    }

    timeSigNum_ = numerator;
    timeSigDenom_ = denominator;
    coordSystem_.setTimeSignature(timeSigNum_, timeSigDenom_);
    timeConverter_.setContext(bpm_, timeSigNum_, timeSigDenom_);
    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);
    repaint();
}

void PianoRollComponent::setTimeUnit(TimeUnit unit) {
    timeUnit_ = unit;
    repaint();
}

void PianoRollComponent::addListener(Listener* listener) {
    listeners_.add(listener);
}

void PianoRollComponent::removeListener(Listener* listener) {
    listeners_.remove(listener);
}

void PianoRollComponent::mouseMove(const juce::MouseEvent& e) {
    toolHandler_->mouseMove(e);
}

void PianoRollComponent::mouseDown(const juce::MouseEvent& e) {
    if (juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey)) {
        isPanning_ = true;
        dragStartPos_ = e.getPosition();
        dragStartScrollOffset_ = scrollOffset_;
        dragStartVerticalScrollOffset_ = verticalScrollOffset_;
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }
    toolHandler_->mouseDown(e);
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent& e) {
    if (isPanning_) {
        int deltaX = e.x - dragStartPos_.x;
        int deltaY = e.y - dragStartPos_.y;
        int newScrollX = dragStartScrollOffset_ - deltaX;
        setScrollOffset(newScrollX);
        float newScrollY = dragStartVerticalScrollOffset_ - (float)deltaY;
        float maxScroll = getTotalHeight() - getHeight();
        verticalScrollOffset_ = juce::jlimit(0.0f, std::max(0.0f, maxScroll), newScrollY);
        requestInteractiveRepaint();
        return;
    }
    toolHandler_->mouseDrag(e);
    requestInteractiveRepaint();
}

void PianoRollComponent::mouseUp(const juce::MouseEvent& e) {
    if (isPanning_) {
        isPanning_ = false;
        setCurrentTool(currentTool_);
        return;
    }

    toolHandler_->mouseUp(e);
}

void PianoRollComponent::handleVerticalZoomWheel(const juce::MouseEvent& e, float deltaY) {
    const auto& settings = ZoomSensitivityConfig::getSettings();
    float zoomFactor = 1.0f + (deltaY * settings.verticalZoomFactor);
    float mouseMidi = yToMidi((float)e.y);
    
    pixelsPerSemitone_ *= zoomFactor;
    pixelsPerSemitone_ = juce::jlimit(5.0f, 60.0f, pixelsPerSemitone_);
    userHasManuallyZoomed_ = true;

    float targetY = (maxMidi_ - mouseMidi) * pixelsPerSemitone_;
    verticalScrollOffset_ = targetY - (float)e.y;
    
    float totalHeight = getTotalHeight();
    float visibleHeight = static_cast<float>(getHeight() - rulerHeight_ - 15);
    float maxScroll = totalHeight - visibleHeight;
    if (maxScroll > 0.0f) {
        verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    } else {
        verticalScrollOffset_ = 0.0f;
    }
    
    updateScrollBars();
    repaint();
}

void PianoRollComponent::handleHorizontalScrollWheel(float deltaX, float deltaY) {
    const auto& settings = ZoomSensitivityConfig::getSettings();
    float scrollDelta = (deltaX != 0 ? deltaX : deltaY);
    int pixelDelta = static_cast<int>(scrollDelta * settings.scrollSpeed);
    setScrollOffset(scrollOffset_ - pixelDelta);
}

void PianoRollComponent::handleVerticalScrollWheel(float deltaY) {
    const auto& settings = ZoomSensitivityConfig::getSettings();
    float scrollDelta = deltaY * settings.scrollSpeed;
    verticalScrollOffset_ -= scrollDelta;
    float totalHeight = getTotalHeight();
    float visibleHeight = static_cast<float>(getHeight() - rulerHeight_ - 15);
    float maxScroll = totalHeight - visibleHeight;
    if (maxScroll > 0.0f) {
        verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    } else {
        verticalScrollOffset_ = 0.0f;
    }
    updateScrollBars();
    repaint();
}

void PianoRollComponent::handleHorizontalZoomWheel(const juce::MouseEvent& e, float deltaY) {
    const auto& settings = ZoomSensitivityConfig::getSettings();
    double zoomFactor = 1.0 + deltaY * settings.horizontalZoomFactor;
    double newZoom = zoomLevel_ * zoomFactor;
    newZoom = std::max(0.02, std::min(10.0, newZoom));

    int mouseX = e.x - pianoKeyWidth_;
    double mouseTime = timeConverter_.pixelToTime(mouseX);

    setZoomLevel(newZoom);
    userHasManuallyZoomed_ = true;

    setScrollOffset(0); 
    int absolutePixel = timeConverter_.timeToPixel(mouseTime);
    int newScrollOffset = absolutePixel - mouseX;
    setScrollOffset(newScrollOffset);
}

void PianoRollComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    if (wheel.deltaY == 0.0f && wheel.deltaX == 0.0f) return;
    
    if (e.mods.isShiftDown()) {
        handleVerticalZoomWheel(e, wheel.deltaY);
    } else if (e.mods.isCtrlDown()) {
        handleHorizontalZoomWheel(e, wheel.deltaY);
    } else if (e.mods.isAltDown()) {
        handleHorizontalScrollWheel(wheel.deltaX, wheel.deltaY);
    } else {
        handleVerticalScrollWheel(wheel.deltaY);
    }
}

bool PianoRollComponent::keyPressed(const juce::KeyPress& key) {
    return toolHandler_->keyPressed(key);
}

PianoRollRenderer::RenderContext PianoRollComponent::buildRenderContext() const
{
    PianoRollRenderer::RenderContext ctx;
    ctx.width = getWidth();
    ctx.height = getHeight();
    ctx.pianoKeyWidth = pianoKeyWidth_;
    ctx.rulerHeight = rulerHeight_;
    ctx.zoomLevel = zoomLevel_;
    ctx.scrollOffset = scrollOffset_;
    ctx.pixelsPerSemitone = pixelsPerSemitone_;
    ctx.minMidi = minMidi_;
    ctx.maxMidi = maxMidi_;
    ctx.bpm = bpm_;
    ctx.timeSigNum = timeSigNum_;
    ctx.timeSigDenom = timeSigDenom_;
    ctx.trackOffsetSeconds = trackOffsetSeconds_;
    ctx.audioSampleRate = PianoRollComponent::kAudioSampleRate;
    ctx.hopSize = hopSize_;
    ctx.f0SampleRate = f0SampleRate_;
    ctx.scaleRootNote = scaleRootNote_;
    ctx.scaleType = scaleType_;
    ctx.showWaveform = showWaveform_;
    ctx.showLanes = showLanes_;
    ctx.showOriginalF0 = showOriginalF0_;
    ctx.showCorrectedF0 = showCorrectedF0_;
    ctx.isRendering = isRendering_;
    ctx.renderingProgress = renderingProgress_;
    ctx.hasUserAudio = hasUserAudio_;
    ctx.timeUnit = (timeUnit_ == TimeUnit::Bars)
        ? PianoRollRenderer::RenderContext::TimeUnit::Bars
        : PianoRollRenderer::RenderContext::TimeUnit::Seconds;

    ctx.midiToY = [this](float midi) { return midiToY(midi); };
    ctx.freqToY = [this](float freq) { return freqToY(freq); };
    ctx.freqToMidi = [this](float freq) { return freqToMidi(freq); };
    ctx.xToTime = [this](int x) { return xToTime(x); };
    ctx.timeToX = [this](double seconds) { return timeToX(seconds); };
    ctx.clipSecondsToFrameIndex = [this](double seconds) -> double {
        const double frameDuration = hopSize_ / f0SampleRate_;
        return seconds / frameDuration;
    };
    ctx.frameIndexToClipSeconds = [this](int frame) -> double {
        const double frameDuration = hopSize_ / f0SampleRate_;
        return frame * frameDuration;
    };

    return ctx;
}

void PianoRollComponent::setHasUserAudio(bool hasAudio) {
    hasUserAudio_ = hasAudio;
    if (hasUserAudio_) {
        fitToScreen();
    }
    repaint();
}

void PianoRollComponent::setScale(int rootNote, int scaleType)
{
    scaleRootNote_ = juce::jlimit(0, 11, rootNote);
    scaleType_ = juce::jlimit(1, 3, scaleType);
    repaint();
}

void PianoRollComponent::fitToScreen() {
    // 如果用户已手动调整过缩放，不自动覆盖
    if (userHasManuallyZoomed_) {
        return;
    }

    // 1. Vertical Fit: Show C1 to C8 (minMidi_ to maxMidi_)
    // Total range: maxMidi_ - minMidi_
    // Available height: getHeight()
    float range = maxMidi_ - minMidi_;
    if (range > 0 && getHeight() > 0) {
        pixelsPerSemitone_ = static_cast<float>(getHeight()) / range;
        
        // Reset scroll to show top
        verticalScrollOffset_ = 0; 
    }

    // 2. Horizontal Fit:
    // If has audio: fit audio length
    // If no audio: fit 16 seconds
    double duration = 16.0;
    if (hasUserAudio_ && audioBuffer_ && PianoRollComponent::kAudioSampleRate > 0) {
        duration = static_cast<double>(audioBuffer_->getNumSamples()) / PianoRollComponent::kAudioSampleRate;
    }
    
    // Available width: getWidth() - pianoKeyWidth_
    int viewWidth = getWidth() - pianoKeyWidth_;
    if (viewWidth > 0 && duration > 0) {
        // pixelsPerSecond * duration = viewWidth
        // pixelsPerSecond = viewWidth / duration
        double pixelsPerSecond = static_cast<double>(viewWidth) / duration;
        
        // zoomLevel = pixelsPerSecond / 100.0 (base scale)
        zoomLevel_ = pixelsPerSecond / 100.0;
        timeConverter_.setZoom(zoomLevel_);
        // 同步 zoomLevel 到 playheadOverlay
        playheadOverlay_.setZoomLevel(zoomLevel_);
    }

    if (hasUserAudio_ && audioBuffer_ && PianoRollComponent::kAudioSampleRate > 0) {
        int newScroll = (int) std::llround(trackOffsetSeconds_ * 100.0 * zoomLevel_);
        setScrollOffset(newScroll);
    } else {
        setScrollOffset(0);
    }
    
    repaint();
}

// HachiTune-style MIDI-based coordinate conversion
float PianoRollComponent::midiToY(float midiNote) const {
    return (maxMidi_ - midiNote) * pixelsPerSemitone_ - verticalScrollOffset_;
}

float PianoRollComponent::yToMidi(float y) const {
    return maxMidi_ - ((y + verticalScrollOffset_) / pixelsPerSemitone_);
}

float PianoRollComponent::getTotalHeight() const {
    return (maxMidi_ - minMidi_) * pixelsPerSemitone_;
}

float PianoRollComponent::freqToMidi(float frequency) const {
    if (frequency <= 0.0f) return 0.0f;
    // 统一语义：频率↔MIDI 以“半音中心线”为锚点（不是键边界）。
    return 12.0f * std::log2(frequency / 440.0f) + 69.0f - 0.5f;
}

float PianoRollComponent::midiToFreq(float midiNote) const {
    // 与 freqToMidi 保持严格互逆的中心线锚点约定。
    return 440.0f * std::pow(2.0f, (midiNote + 0.5f - 69.0f) / 12.0f);
}

float PianoRollComponent::yToFreq(float y) const {
    return midiToFreq(yToMidi(y));
}

float PianoRollComponent::freqToY(float freq) const {
    return midiToY(freqToMidi(freq));
}

int PianoRollComponent::timeToX(double seconds) const {
    return timeConverter_.timeToPixel(seconds - trackOffsetSeconds_ + alignmentOffsetSeconds_) + pianoKeyWidth_;
}

double PianoRollComponent::xToTime(int x) const {
    return timeConverter_.pixelToTime(x - pianoKeyWidth_) + trackOffsetSeconds_ - alignmentOffsetSeconds_;
}

Note* PianoRollComponent::findNoteAtPixel(int pixelX, int pixelY) {
    if (noteSequence_.isEmpty()) {
        return nullptr;
    }
    
    for (auto& note : noteSequence_.getNotes()) {
        float adjustedPitch = note.getAdjustedPitch();
        if (adjustedPitch <= 0.0f) {
            continue;
        }
        
        float midi = freqToMidi(adjustedPitch);
        float noteY = midiToY(midi) - (pixelsPerSemitone_ * 0.5f);
        float noteH = pixelsPerSemitone_;
        
        int noteX1 = timeToX(note.startTime + trackOffsetSeconds_);
        int noteX2 = timeToX(note.endTime + trackOffsetSeconds_);
        
        if (pixelX >= noteX1 && pixelX <= noteX2 &&
            pixelY >= noteY && pixelY <= noteY + noteH) {
            return &note;
        }
    }
    
    return nullptr;
}

float PianoRollComponent::recalculatePIP(Note& note) {
    if (!currentCurve_) return -1.0f;

    if (note.endTime <= note.startTime) return -1.0f;

    auto snapshot = currentCurve_->getSnapshot();
    const auto& originalF0 = snapshot->getOriginalF0();
    
    const double frameDuration = hopSize_ / f0SampleRate_;
    int startFrame = static_cast<int>(note.startTime / frameDuration);
    int endFrame = static_cast<int>(note.endTime / frameDuration);
    
    if (startFrame < 0) startFrame = 0;
    if (endFrame > static_cast<int>(originalF0.size())) endFrame = static_cast<int>(originalF0.size());
    
    if (startFrame >= endFrame) return -1.0f;

    int numFrames = endFrame - startFrame;
    
    std::vector<float> noteF0(numFrames);
    std::copy(originalF0.begin() + startFrame, originalF0.begin() + endFrame, noteF0.begin());

    std::vector<float> voicedF0;
    voicedF0.reserve(noteF0.size());
    for (float f : noteF0) {
        if (f > 0.0f) voicedF0.push_back(f);
    }

    if (voicedF0.empty()) {
        return -1.0f;
    }

    std::sort(voicedF0.begin(), voicedF0.end());
    float medianF0 = voicedF0[voicedF0.size() / 2];
    return medianF0;
}

bool PianoRollComponent::applyAutoTuneToSelection()
{
    AppLogger::log("AutoTuneTrace: applyAutoTuneToSelection called");
    DBG("PianoRollComponent::applyAutoTuneToSelection - called");
    if (!currentCurve_) {
        AppLogger::log("AutoTuneTrace: applyAutoTuneToSelection - currentCurve_ is null");
        DBG("PianoRollComponent::applyAutoTuneToSelection - currentCurve_ is null");
        return false;
    }

    if (currentTrackId_ < 0 || currentClipId_ == 0) {
        AppLogger::log("AutoTuneTrace: AUTO failed - missing valid clip context trackId="
            + juce::String(currentTrackId_) + " clipId=" + juce::String(static_cast<juce::int64>(currentClipId_)));
        DBG("PianoRoll: AUTO failed - missing valid clip context (trackId="
            + juce::String(currentTrackId_) + ", clipId=" + juce::String(static_cast<juce::int64>(currentClipId_)) + ")");
        return false;
    }

    if (isAutoTuneProcessing_.exchange(true)) {
        AppLogger::log("AutoTuneTrace: AUTO processing, ignoring duplicate request");
        DBG("PianoRoll: AUTO processing, ignoring duplicate request");
        return false;
    }

    AppLogger::log("AutoTuneTrace: proceeding with AUTO trackId=" + juce::String(currentTrackId_)
        + " clipId=" + juce::String(static_cast<juce::int64>(currentClipId_)));
    DBG("PianoRollComponent::applyAutoTuneToSelection - proceeding with AUTO");

    repaint();

    auto snapshot = currentCurve_->getSnapshot();
    const double frameDuration = hopSize_ / f0SampleRate_;

    if (!hasSelectionArea_) {
        const size_t f0Length = snapshot->size();
        if (f0Length >= 2) {
            selectionStartTime_ = 0.0;
            selectionStartMidi_ = minMidi_;
            selectionEndMidi_ = maxMidi_;
            int endFrame = static_cast<int>(f0Length) - 1;
            selectionEndTime_ = (endFrame + 1) * frameDuration;
            if (audioBuffer_ != nullptr) {
                double maxTime = static_cast<double>(audioBuffer_->getNumSamples()) / PianoRollComponent::kAudioSampleRate;
                selectionEndTime_ = std::min(selectionEndTime_, maxTime);
            }
            selectionEndTime_ = std::max(0.0, selectionEndTime_);
            hasSelectionArea_ = true;
        }
    }

    if (!hasSelectionArea_) {
        isAutoTuneProcessing_.store(false);
        if (onRenderComplete_) onRenderComplete_();
        return false;
    }

    double startTime = std::min(selectionStartTime_, selectionEndTime_);
    double endTime = std::max(selectionStartTime_, selectionEndTime_);
    if (endTime <= startTime) {
        isAutoTuneProcessing_.store(false);
        if (onRenderComplete_) onRenderComplete_();
        return false;
    }

    int startFrame = static_cast<int>(startTime / frameDuration);
    int endFrame = static_cast<int>(endTime / frameDuration);
    startFrame = std::max(0, startFrame);

    const auto& originalF0 = snapshot->getOriginalF0();
    if (originalF0.empty()) {
        isAutoTuneProcessing_.store(false);
        if (onRenderComplete_) onRenderComplete_();
        return false;
    }

    endFrame = std::min(static_cast<int>(originalF0.size()) - 1, endFrame);
    if (endFrame <= startFrame) {
        isAutoTuneProcessing_.store(false);
        if (onRenderComplete_) onRenderComplete_();
        return false;
    }

    const bool useScaleSnap = (scaleType_ != 3);
    Scale snapScale = Scale::Major;
    if (scaleType_ == 2) {
        snapScale = Scale::Minor;
    }

    DBG("AutoTuneTrace: trackId=" + juce::String(currentTrackId_)
        + " clipId=" + juce::String(static_cast<juce::int64>(currentClipId_))
        + " root=" + juce::String(scaleRootNote_)
        + " scaleType=" + juce::String(scaleType_)
        + " useScaleSnap=" + juce::String(useScaleSnap ? 1 : 0));

    NoteGeneratorParams genParams;
    genParams.policy = segmentationPolicy_;
    genParams.retuneSpeed = currentRetuneSpeed_;
    genParams.vibratoDepth = currentVibratoDepth_;
    genParams.vibratoRate = currentVibratoRate_;
    if (useScaleSnap) {
        ScaleSnapConfig snapCfg;
        snapCfg.root = scaleRootNote_ % 12;
        snapCfg.mode = (snapScale == Scale::Minor) ? ScaleMode::Minor : ScaleMode::Major;
        genParams.scaleSnap = snapCfg;
    }

    auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
    request->kind = PianoRollCorrectionWorker::AsyncCorrectionRequest::Kind::AutoTuneGenerate;
    request->curve = currentCurve_;
    request->startFrame = startFrame;
    request->endFrameExclusive = endFrame + 1;
    request->retuneSpeed = currentRetuneSpeed_;
    request->vibratoDepth = currentVibratoDepth_;
    request->vibratoRate = currentVibratoRate_;
    request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);
    request->releaseAutoTuneOnDiscard = true;

    request->autoOriginalF0Full = originalF0;
    request->autoHopSize = hopSize_;
    request->autoF0SampleRate = f0SampleRate_;
    request->autoStartFrame = startFrame;
    request->autoEndFrame = endFrame;
    request->autoStartTime = startTime;
    request->autoEndTime = endTime;
    request->autoGenParams = genParams;

    request->clipContextGenerationSnapshot = clipContextGeneration_.load(std::memory_order_acquire);
    request->trackIdSnapshot = currentTrackId_;
    request->clipIdSnapshot = currentClipId_;

    request->autoNotesBefore = noteSequence_.getNotes();
    if (currentCurve_) {
        auto snapshotForSegments = currentCurve_->getSnapshot();
        const auto& segments = snapshotForSegments->getCorrectedSegments();
        request->autoF0Before.reserve(segments.size());
        for (const auto& seg : segments) {
            request->autoF0Before.emplace_back(seg);
        }
    }

    request->onApplied = [this, startFrame, endFrame]() {
        listeners_.call([startFrame, endFrame](Listener& l) {
            l.pitchCurveEdited(startFrame, endFrame);
        });
    };

    request->onRenderComplete = [this]() {
        if (onRenderComplete_) {
            onRenderComplete_();
        }
    };

    DBG("PianoRollComponent::applyAutoTuneToSelection - enqueuing request: startFrame=" + juce::String(startFrame)
        + " endFrame=" + juce::String(endFrame)
        + " autoStartFrame=" + juce::String(request->autoStartFrame)
        + " autoEndFrame=" + juce::String(request->autoEndFrame)
        + " autoOriginalF0Full.size=" + juce::String(static_cast<int>(request->autoOriginalF0Full.size())));

    correctionWorker_->enqueue(request);

    DBG("PianoRollComponent::applyAutoTuneToSelection - enqueue done, returning true");

    return true;
}

void PianoRollComponent::setNotes(const std::vector<Note>& notes) {
    noteSequence_.setNotesSorted(notes);
    
    updateScrollBars();
    repaint();
}



// ============================================================================
// Undo/Redo 核心实现
// ============================================================================

UndoManager* PianoRollComponent::getCurrentUndoManager() noexcept
{
    // 返回全局 UndoManager 指针
    // 注意：在正常运行环境下，globalUndoManager_ 由 PluginEditor 在初始化时设置
    // 如果为 nullptr 表示组件未正确初始化，调用方需要处理此情况
    return globalUndoManager_;
}

const UndoManager* PianoRollComponent::getCurrentUndoManager() const noexcept
{
    // 返回全局 UndoManager 的 const 指针
    return globalUndoManager_;
}

bool PianoRollComponent::canUndoInternal() const noexcept
{
    // 如果 UndoManager 未初始化，返回 false
    auto* um = getCurrentUndoManager();
    return um != nullptr && um->canUndo();
}

bool PianoRollComponent::canRedoInternal() const noexcept
{
    // 如果 UndoManager 未初始化，返回 false
    auto* um = getCurrentUndoManager();
    return um != nullptr && um->canRedo();
}

void PianoRollComponent::setCurrentClipContext(int trackId, uint64_t clipId)
{
    currentTrackId_ = trackId;
    currentClipId_ = clipId;
    // 使用 release 语义确保与工作线程正确同步
    // 递增 generation 以使任何进行中的 AUTO 请求失效
    clipContextGeneration_.fetch_add(1, std::memory_order_release);
    if (correctionWorker_) {
        correctionWorker_->setClipContext(trackId, clipId);
    }
    // 全局 UndoManager 不需要按 clip 隔离，直接使用全局的
}

void PianoRollComponent::clearClipContext()
{
    currentTrackId_ = -1;
    currentClipId_ = 0;
    // 使用 release 语义确保与工作线程正确同步
    // 递增 generation 以使任何进行中的 AUTO 请求失效
    clipContextGeneration_.fetch_add(1, std::memory_order_release);
    if (correctionWorker_) {
        correctionWorker_->setClipContext(-1, 0);
    }
}

// ============================================================================
// Undo/Redo 执行
// ============================================================================

void PianoRollComponent::refreshAfterUndoRedo() {
    updateScrollBars();
    
    if (currentCurve_) {
        int affectedStartFrame = 0;
        int affectedEndFrame = static_cast<int>(currentCurve_->size()) - 1;
        
        // 只触发渲染更新，不重新计算 correctedF0
        listeners_.call([affectedStartFrame, affectedEndFrame](Listener& l) {
            l.pitchCurveEdited(affectedStartFrame, affectedEndFrame);
        });
    }
    repaint();
}

void PianoRollComponent::undo() {
    auto* um = getCurrentUndoManager();
    if (um && um->canUndo()) {
        DBG("Undo: " + um->getUndoDescription());
        um->undo();
        refreshAfterUndoRedo();
    }
}

void PianoRollComponent::redo() {
    auto* um = getCurrentUndoManager();
    if (um && um->canRedo()) {
        DBG("Redo: " + um->getRedoDescription());
        um->redo();
        refreshAfterUndoRedo();
    }
}

bool PianoRollComponent::canUndo() const {
    return canUndoInternal();
}

bool PianoRollComponent::canRedo() const {
    return canRedoInternal();
}

void PianoRollComponent::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) {
    if (scrollBar == &horizontalScrollBar_) {
        setScrollOffset(static_cast<int>(newRangeStart));
        smoothScrollCurrent_ = (float)newRangeStart;
    } else if (scrollBar == &verticalScrollBar_) {
        verticalScrollOffset_ = static_cast<float>(newRangeStart);
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Normal);
    }
}

void PianoRollComponent::updateScrollBars() {
    double maxTime = 0.0;
    if (audioBuffer_) {
        maxTime = (double)audioBuffer_->getNumSamples() / PianoRollComponent::kAudioSampleRate;
    } else if (!noteSequence_.getNotes().empty()) {
        for (const auto& note : noteSequence_.getNotes()) {
            if (note.endTime > maxTime) maxTime = note.endTime;
        }
    }
    
    // Add some padding
    maxTime = std::max(maxTime, 10.0); // Minimum 10 seconds
    maxTime += 5.0; // Extra padding
    
    double pixelsPerSecond = 100.0 * zoomLevel_;
    int totalContentWidth = static_cast<int>(maxTime * pixelsPerSecond);
    int visibleWidth = getWidth() - pianoKeyWidth_ - 15; // -15 for vertical scrollbar
    visibleWidth = juce::jmax(1, visibleWidth);
    
    horizontalScrollBar_.setRangeLimits(0.0, totalContentWidth + visibleWidth);
    horizontalScrollBar_.setCurrentRange(scrollOffset_, visibleWidth);
    
    // Vertical
    float totalHeight = getTotalHeight();
    int visibleHeight = getHeight() - rulerHeight_ - 15; // -15 for horizontal scrollbar
    visibleHeight = juce::jmax(1, visibleHeight);
    
    verticalScrollBar_.setRangeLimits(0.0, totalHeight);
    verticalScrollBar_.setCurrentRange(verticalScrollOffset_, visibleHeight);
}

} // namespace OpenTune
