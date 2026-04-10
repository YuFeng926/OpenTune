#include "PianoRollComponent.h"
#include "PianoRoll/PianoRollUndoSupport.h"
#include "../Utils/AppLogger.h"
#include "../Utils/PitchUtils.h"
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
    undoCtx.getNotesCopy = [this]() { return getCurrentClipNotesCopy(); };
    undoCtx.getPitchCurve = [this]() { return currentCurve_; };
    undoCtx.getCurrentClipId = [this]() { return currentClipId_; };
    undoCtx.getCurrentTrackId = [this]() { return currentTrackId_; };
    undoCtx.getProcessor = [this]() { return processor_; };
    undoCtx.getUndoManager = [this]() { return globalUndoManager_; };
    undoSupport_ = std::make_unique<PianoRollUndoSupport>(std::move(undoCtx));
}

void PianoRollComponent::initializeRenderer() {
    renderer_ = std::make_unique<PianoRollRenderer>();
}

void PianoRollComponent::initializeCorrectionWorker() {
    correctionWorker_ = std::make_unique<PianoRollCorrectionWorker>();
}

PianoRollToolHandler::Context PianoRollComponent::buildToolHandlerContext() {
    PianoRollToolHandler::Context toolCtx;
    toolCtx.getState = [this]() -> InteractionState& { return interactionState_; };

    toolCtx.xToTime = [this](int x) { return xToTime(x); };
    toolCtx.timeToX = [this](double seconds) { return timeToX(seconds); };
    toolCtx.yToFreq = [this](float y) { return yToFreq(y); };
    toolCtx.freqToY = [this](float f) { return freqToY(f); };

    toolCtx.getNotes = [this]() -> std::vector<Note>& { return getCurrentClipNotes(); };
    toolCtx.getSelectedNotes = [this]() -> std::vector<Note*> {
        std::vector<Note*> selected;
        auto& notes = getCurrentClipNotes();
        for (auto& n : notes) {
            if (n.selected) selected.push_back(&n);
        }
        return selected;
    };
    toolCtx.findNoteAt = [this](double time, float targetPitchHz, float pitchToleranceHz) -> Note* {
        auto& notes = getCurrentClipNotes();
        for (auto& note : notes) {
            if (time >= note.startTime && time < note.endTime) {
                float adjustedPitch = note.getAdjustedPitch();
                if (std::abs(adjustedPitch - targetPitchHz) <= pitchToleranceHz) {
                    return &note;
                }
            }
        }
        return nullptr;
    };
    toolCtx.deselectAllNotes = [this]() {
        auto& notes = getCurrentClipNotes();
        for (auto& n : notes) n.selected = false;
    };
    toolCtx.selectAllNotes = [this]() {
        auto& notes = getCurrentClipNotes();
        for (auto& n : notes) n.selected = true;
    };
    toolCtx.insertNoteSorted = [this](const Note& note) {
        auto& notes = getCurrentClipNotes();
        notes.push_back(note);
        std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
            return a.startTime < b.startTime;
        });
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
    toolCtx.notifyNoteOffsetChanged = [this](size_t noteIndex, float oldOffset, float newOffset) {
        listeners_.call([noteIndex, oldOffset, newOffset](Listener& l) { l.noteOffsetChanged(noteIndex, oldOffset, newOffset); });
    };
    toolCtx.enqueueNoteBasedCorrection = [this](int startFrame,
                                                int endFrameExclusive,
                                                float retuneSpeed,
                                                float vibratoDepth,
                                                float vibratoRate) {
        enqueueNoteBasedCorrectionAsync(
            startFrame,
            endFrameExclusive,
            retuneSpeed,
            vibratoDepth,
            vibratoRate);
    };
    toolCtx.getPianoKeyWidth = [this]() { return pianoKeyWidth_; };
    toolCtx.getTrackOffsetSeconds = [this]() { return trackOffsetSeconds_; };
    toolCtx.getAudioSampleRate = [this]() { return PianoRollComponent::kAudioSampleRate; };
    toolCtx.getAudioBuffer = [this]() -> const juce::AudioBuffer<float>* { return audioBuffer_ ? audioBuffer_.get() : nullptr; };

    toolCtx.getDirtyStartTime = [this]() { return interactionState_.drawing.dirtyStartTime; };
    toolCtx.setDirtyStartTime = [this](double v) { interactionState_.drawing.dirtyStartTime = v; };
    toolCtx.getDirtyEndTime = [this]() { return interactionState_.drawing.dirtyEndTime; };
    toolCtx.setDirtyEndTime = [this](double v) { interactionState_.drawing.dirtyEndTime = v; };

    toolCtx.getDrawingNoteStartTime = [this]() { return interactionState_.drawing.drawingNoteStartTime; };
    toolCtx.setDrawingNoteStartTime = [this](double v) { interactionState_.drawing.drawingNoteStartTime = v; };
    toolCtx.getDrawingNoteEndTime = [this]() { return interactionState_.drawing.drawingNoteEndTime; };
    toolCtx.setDrawingNoteEndTime = [this](double v) { interactionState_.drawing.drawingNoteEndTime = v; };
    toolCtx.getDrawingNotePitch = [this]() { return interactionState_.drawing.drawingNotePitch; };
    toolCtx.setDrawingNotePitch = [this](float v) { interactionState_.drawing.drawingNotePitch = v; };
    toolCtx.getDrawingNoteIndex = [this]() { return interactionState_.drawing.drawingNoteIndex; };
    toolCtx.setDrawingNoteIndex = [this](int v) { interactionState_.drawing.drawingNoteIndex = v; };

    toolCtx.getDrawNoteToolPendingDrag = [this]() { return interactionState_.drawNoteToolPendingDrag; };
    toolCtx.setDrawNoteToolPendingDrag = [this](bool v) { interactionState_.drawNoteToolPendingDrag = v; };
    toolCtx.getDrawNoteToolMouseDownPos = [this]() { return interactionState_.drawNoteToolMouseDownPos; };
    toolCtx.setDrawNoteToolMouseDownPos = [this](juce::Point<int> v) { interactionState_.drawNoteToolMouseDownPos = v; };
    toolCtx.getDragThreshold = [this]() { return dragThreshold_; };

    toolCtx.getNoteDragManualStartTime = [this]() { return interactionState_.noteDrag.manualStartTime; };
    toolCtx.setNoteDragManualStartTime = [this](double v) { interactionState_.noteDrag.manualStartTime = v; };
    toolCtx.getNoteDragManualEndTime = [this]() { return interactionState_.noteDrag.manualEndTime; };
    toolCtx.setNoteDragManualEndTime = [this](double v) { interactionState_.noteDrag.manualEndTime = v; };
    toolCtx.getNoteDragInitialManualTargets = [this]() -> std::vector<std::pair<double, float>>& { return interactionState_.noteDrag.initialManualTargets; };

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
    toolCtx.beginEditTransaction = [this](const juce::String& name) {
        undoSupport_->beginTransaction(name);
    };
    toolCtx.commitEditTransaction = [this]() {
        undoSupport_->commitTransaction();
    };
    toolCtx.isTransactionActive = [this]() { return undoSupport_->isTransactionActive(); };
    toolCtx.applyManualCorrection = [this](std::vector<PianoRollToolHandler::ManualCorrectionOp> ops, int s, int e, bool render) {
        enqueueManualCorrectionPatchAsync(ops, s, e, render);
    };
    return toolCtx;
}

void PianoRollComponent::initializeToolHandler() {
    toolHandler_ = std::make_unique<PianoRollToolHandler>(buildToolHandlerContext());
}

PianoRollComponent::PianoRollComponent() {
    initializeUIComponents();
    initializeUndoSupport();
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
    if (isAutoTuneProcessing()) {
        return false;
    }

    if (!currentCurve_) {
        return false;
    }

    auto snapshot = currentCurve_->getSnapshot();
    const int maxFrame = static_cast<int>(snapshot->size());
    if (maxFrame <= 0) {
        return false;
    }

    undoSupport_->beginTransaction("Change Parameters");
    auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
    request->curve = currentCurve_;
    request->notes = getCurrentClipNotesCopy();
    request->startFrame = 0;
    request->endFrameExclusive = maxFrame;
    request->retuneSpeed = retuneSpeed;
    request->vibratoDepth = vibratoDepth;
    request->vibratoRate = vibratoRate;
    request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);

    correctionWorker_->enqueue(request);
    return true;
}

void PianoRollComponent::consumeCompletedCorrectionResults()
{
    if (!correctionWorker_) {
        return;
    }

    auto completed = correctionWorker_->takeCompleted();
    if (!completed) {
        return;
    }

    if (undoSupport_ && undoSupport_->isTransactionActive()) {
        undoSupport_->commitTransaction();
    }

    if (completed->success) {
        const int notifyStart = completed->startFrame;
        const int notifyEnd = std::max(notifyStart, completed->endFrameExclusive - 1);
        listeners_.call([notifyStart, notifyEnd](Listener& l) { l.pitchCurveEdited(notifyStart, notifyEnd); });
    }

    if (completed->isAutoTuneRequest) {
        correctionInFlight_.store(false, std::memory_order_release);
        if (onRenderComplete_) {
            onRenderComplete_();
        }
    }

    requestInteractiveRepaint();
}

void PianoRollComponent::enqueueManualCorrectionPatchAsync(const std::vector<PianoRollToolHandler::ManualCorrectionOp>& ops,
                                                           int dirtyStartFrame,
                                                           int dirtyEndFrame,
                                                           bool triggerRenderEvent)
{
    if (!currentCurve_ || ops.empty()) {
        return;
    }

    for (const auto& op : ops) {
        if (op.endFrameExclusive <= op.startFrame) {
            continue;
        }

        currentCurve_->setManualCorrectionRange(
            op.startFrame,
            op.endFrameExclusive,
            op.f0Data,
            op.source,
            op.retuneSpeed);
    }

    if (triggerRenderEvent && dirtyEndFrame >= dirtyStartFrame) {
        listeners_.call([dirtyStartFrame, dirtyEndFrame](Listener& l) {
            l.pitchCurveEdited(dirtyStartFrame, dirtyEndFrame);
        });
    }

    requestInteractiveRepaint();
}

void PianoRollComponent::enqueueNoteBasedCorrectionAsync(int startFrame,
                                                         int endFrameExclusive,
                                                         float retuneSpeed,
                                                         float vibratoDepth,
                                                         float vibratoRate,
                                                         bool isAutoTuneRequest)
{
    if (!currentCurve_) {
        return;
    }

    auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
    request->curve = currentCurve_;
    request->notes = getCurrentClipNotesCopy();
    request->startFrame = startFrame;
    request->endFrameExclusive = endFrameExclusive;
    request->retuneSpeed = retuneSpeed;
    request->vibratoDepth = vibratoDepth;
    request->vibratoRate = vibratoRate;
    request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);
    request->isAutoTuneRequest = isAutoTuneRequest;
    correctionWorker_->enqueue(request);
}

void PianoRollComponent::drawSelectedOriginalF0Curve(juce::Graphics& g, const std::vector<float>& originalF0, double offsetSeconds) {
    const auto& notes = getCurrentClipNotes();
    bool hasNoteSelection = false;
    for (const auto& note : notes) {
        if (note.selected) { hasNoteSelection = true; break; }
    }

    if (!hasNoteSelection) return;

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

    if (!selectedPath.isEmpty()) {
        g.setColour(UIColors::originalF0.withAlpha(0.85f));
        juce::PathStrokeType strokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.strokePath(selectedPath, strokeType);
    }
}

void PianoRollComponent::drawHandDrawPreview(juce::Graphics& g, double offsetSeconds) {
    if (!interactionState_.drawing.isDrawingF0 || currentTool_ != ToolId::HandDraw || interactionState_.drawing.handDrawBuffer.empty() || !currentCurve_) return;

    auto snapshot = currentCurve_->getSnapshot();
    const auto& originalF0 = snapshot->getOriginalF0();
    if (originalF0.empty() || interactionState_.drawing.handDrawBuffer.size() != originalF0.size()) return;

    const double frameDuration = hopSize_ / f0SampleRate_;
    juce::Colour previewColour = juce::Colour(0xFF00DDDD);
    juce::Path previewPath;
    bool pathStarted = false;

    for (size_t i = 0; i < interactionState_.drawing.handDrawBuffer.size(); ++i) {
        float f0 = interactionState_.drawing.handDrawBuffer[i];
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
    if (!interactionState_.drawing.isPlacingAnchors || currentTool_ != ToolId::LineAnchor || interactionState_.drawing.pendingAnchors.empty()) return;

    juce::Colour anchorColour = UIColors::correctedF0;

    for (size_t i = 0; i < interactionState_.drawing.pendingAnchors.size(); ++i) {
        const auto& anchor = interactionState_.drawing.pendingAnchors[i];
        float x = static_cast<float>(timeToX(anchor.time + offsetSeconds));
        float y = freqToY(anchor.freq);

        g.setColour(anchorColour);
        g.fillEllipse(x - 2.0f, y - 2.0f, 4.0f, 4.0f);

        if (i > 0) {
            const auto& prev = interactionState_.drawing.pendingAnchors[i - 1];
            float prevX = static_cast<float>(timeToX(prev.time + offsetSeconds));
            float prevY = freqToY(prev.freq);
            g.setColour(anchorColour.withAlpha(0.7f));
            g.drawLine(prevX, prevY, x, y, 2.0f);
        }
    }

    if (!interactionState_.drawing.pendingAnchors.empty()) {
        const auto& last = interactionState_.drawing.pendingAnchors.back();
        float lastX = static_cast<float>(timeToX(last.time + offsetSeconds));
        float lastY = freqToY(last.freq);
        g.setColour(anchorColour.withAlpha(0.4f));
        g.drawLine(lastX, lastY, interactionState_.drawing.currentMousePos.x, interactionState_.drawing.currentMousePos.y, 1.5f);
    }
}

void PianoRollComponent::drawSelectionBox(juce::Graphics& g, double offsetSeconds, ThemeId themeId) {
    if (!toolHandler_ || !interactionState_.selection.isSelectingArea) return;

    double startTime = std::min(interactionState_.selection.dragStartTime, interactionState_.selection.dragEndTime);
    double endTime = std::max(interactionState_.selection.dragStartTime, interactionState_.selection.dragEndTime);
    float minMidi = std::min(interactionState_.selection.dragStartMidi, interactionState_.selection.dragEndMidi);
    float maxMidi = std::max(interactionState_.selection.dragStartMidi, interactionState_.selection.dragEndMidi);

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

        renderer_->drawNotes(g, ctx, getCurrentClipNotes(), trackOffsetSeconds_);

        // Highlight notes that overlap the active HandDraw/LineAnchor drawing region
        if (interactionState_.drawing.isDrawingF0 || interactionState_.drawing.isPlacingAnchors) {
            double drawStart = -1.0;
            double drawEnd = -1.0;
            if (interactionState_.drawing.isDrawingF0
                && interactionState_.drawing.dirtyStartTime >= 0.0
                && interactionState_.drawing.dirtyEndTime >= 0.0) {
                drawStart = std::min(interactionState_.drawing.dirtyStartTime, interactionState_.drawing.dirtyEndTime);
                drawEnd = std::max(interactionState_.drawing.dirtyStartTime, interactionState_.drawing.dirtyEndTime);
            } else if (interactionState_.drawing.isPlacingAnchors && !interactionState_.drawing.pendingAnchors.empty()) {
                drawStart = interactionState_.drawing.pendingAnchors.front().time;
                drawEnd = interactionState_.drawing.pendingAnchors.back().time;
                if (drawEnd < drawStart) std::swap(drawStart, drawEnd);
            }
            if (drawStart >= 0.0 && drawEnd > drawStart) {
                for (const auto& note : getCurrentClipNotes()) {
                    if (note.endTime <= drawStart || note.startTime >= drawEnd) continue;
                    float adjustedPitch = note.getAdjustedPitch();
                    if (adjustedPitch <= 0.0f) continue;
                    float midi = ctx.freqToMidi(adjustedPitch);
                    float ny = ctx.midiToY(midi) - (ctx.pixelsPerSemitone * 0.5f);
                    float nh = ctx.pixelsPerSemitone;
                    int nx1 = ctx.timeToX(note.startTime + trackOffsetSeconds_);
                    int nx2 = ctx.timeToX(note.endTime + trackOffsetSeconds_);
                    float nw = static_cast<float>(nx2 - nx1);
                    g.setColour(juce::Colour(0xFFFFD700).withAlpha(0.25f));
                    g.fillRect(static_cast<float>(nx1), ny, nw, nh);
                    g.setColour(juce::Colour(0xFFFFD700).withAlpha(0.6f));
                    g.drawRect(static_cast<float>(nx1), ny, nw, nh, 1.5f);
                }
            }
        }

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
    for (const auto& n : getCurrentClipNotes()) {
        if (n.selected) { hasSelectedNotes = true; break; }
    }
    if (!hasSelectedNotes) return false;

    undoSupport_->beginTransaction("Retune Speed");
    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;
    const double frameDuration = hopSize_ / f0SampleRate_;
    for (auto& n : getCurrentClipNotes()) {
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
        request->notes = getCurrentClipNotesCopy();
        request->startFrame = startFrame;
        request->endFrameExclusive = endFrame + 1;
        request->retuneSpeed = speed;
        request->vibratoDepth = currentVibratoDepth_;
        request->vibratoRate = currentVibratoRate_;
        request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);
        correctionWorker_->enqueue(request);
        undoSupport_->commitTransaction();
        listeners_.call([startFrame, endFrame](Listener& l) { l.pitchCurveEdited(startFrame, endFrame); });
    } else {
        undoSupport_->commitTransaction();
    }
    repaint();
    return true;
}

bool PianoRollComponent::applyRetuneSpeedToSelection(float speed) {
    if (isAutoTuneProcessing()) {
        return false;
    }

    speed = juce::jlimit(0.0f, 1.0f, speed);
    if (!currentCurve_) return false;

    return applyRetuneSpeedToSelectedNotes(speed);
}

bool PianoRollComponent::hasSelectionRange() const {
    const auto& notes = const_cast<PianoRollComponent*>(this)->getCurrentClipNotes();
    for (const auto& n : notes) {
        if (n.selected) return true;
    }
    return false;
}

std::pair<double, double> PianoRollComponent::getSelectionTimeRange() const {
    double minTime = 1e30;
    double maxTime = -1e30;
    const auto& notes = const_cast<PianoRollComponent*>(this)->getCurrentClipNotes();
    for (const auto& n : notes) {
        if (n.selected) {
            minTime = std::min(minTime, n.startTime);
            maxTime = std::max(maxTime, n.endTime);
        }
    }
    if (minTime > maxTime) return {0.0, 0.0};
    return {minTime, maxTime};
}

bool PianoRollComponent::applyVibratoDepthToSelection(float depth) {
    return applyVibratoParameterToSelection(VibratoParam::Depth, depth);
}

bool PianoRollComponent::applyVibratoRateToSelection(float rate) {
    return applyVibratoParameterToSelection(VibratoParam::Rate, rate);
}

bool PianoRollComponent::applyVibratoParameterToSelection(VibratoParam param, float value) {
    if (isAutoTuneProcessing()) {
        return false;
    }

    auto clampValue = [&]() -> float {
        return (param == VibratoParam::Depth) ? juce::jlimit(0.0f, 100.0f, value)
                                              : juce::jlimit(0.1f, 30.0f, value);
    };
    
    value = clampValue();
    if (!currentCurve_) return false;
    
    bool hasSelectedNotes = false;
    for (const auto& n : getCurrentClipNotes()) {
        if (n.selected) { hasSelectedNotes = true; break; }
    }
    if (!hasSelectedNotes) return false;
    
    undoSupport_->beginTransaction(param == VibratoParam::Depth ? "Vibrato Depth" : "Vibrato Rate");
    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;
    const double frameDuration = hopSize_ / f0SampleRate_;
    
    for (auto& n : getCurrentClipNotes()) {
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
        request->notes = getCurrentClipNotesCopy();
        request->startFrame = startFrame;
        request->endFrameExclusive = endFrame + 1;
        request->retuneSpeed = currentRetuneSpeed_;
        request->vibratoDepth = (param == VibratoParam::Depth) ? value : currentVibratoDepth_;
        request->vibratoRate = (param == VibratoParam::Rate) ? value : currentVibratoRate_;
        request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);
        correctionWorker_->enqueue(request);
    } else {
        undoSupport_->commitTransaction();
    }
    
    if (param == VibratoParam::Depth) {
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Normal);
    } else {
        repaint();
    }
    return true;
}

bool PianoRollComponent::getSingleSelectedNoteParameters(float& retuneSpeedPercent, float& vibratoDepth, float& vibratoRate) const
{
    const auto notes = getCurrentClipNotesCopy();
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

    // Deselect all notes when pitch curve changes
    interactionState_.selection.isSelectingArea = false;
    interactionState_.selection.clearF0Selection();
    for (auto& n : getCurrentClipNotes()) {
        n.selected = false;
    }

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
    }
    
    snapNextScroll_ = true;
    repaint();
}

void PianoRollComponent::setAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, int sampleRate) {
    audioBuffer_ = buffer;
    
    timeConverter_.setContext(bpm_, timeSigNum_, timeSigDenom_);
    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);
    
    playheadOverlay_.setZoomLevel(zoomLevel_);
    playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));

    waveformMipmap_.setAudioSource(buffer);
    renderer_->setWaveformMipmap(&waveformMipmap_);

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

    consumeCompletedCorrectionResults();

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
    if (!waveformMipmap_.isComplete() && showWaveform_) {
        if (inferenceActive_) {
            waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 8;
            if (waveformBuildTickCounter_ == 0 && waveformMipmap_.buildIncremental(1.0)) {
                repaint();
            }
        } else {
            waveformBuildTickCounter_ = 0;
            if (waveformMipmap_.buildIncremental(5.0)) {
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

        const bool isEditingNow = interactionState_.drawing.isDrawingF0
            || interactionState_.drawing.isDrawingNote
            || interactionState_.noteDrag.isDraggingNotes
            || interactionState_.noteResize.isResizing
            || interactionState_.isPanning;
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
    playheadOverlay_.setZoomLevel(zoomLevel_);
    updateScrollBars();
    requestInteractiveRepaint();
}

void PianoRollComponent::setCurrentTool(ToolId tool) {
    if (interactionState_.drawing.isPlacingAnchors && tool != ToolId::LineAnchor) {
        if (undoSupport_ && undoSupport_->isTransactionActive()) undoSupport_->commitTransaction();
        interactionState_.drawing.isPlacingAnchors = false;
        interactionState_.drawing.pendingAnchors.clear();
    }

    // Deselect all notes when switching tools
    if (tool != currentTool_) {
        auto& notes = getCurrentClipNotes();
        for (auto& n : notes) n.selected = false;
        interactionState_.selection.clearF0Selection();
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
    if (isAutoTuneProcessing()) {
        return;
    }

    if (juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey)) {
        interactionState_.isPanning = true;
        interactionState_.dragStartPos = e.getPosition();
        dragStartScrollOffset_ = scrollOffset_;
        dragStartVerticalScrollOffset_ = verticalScrollOffset_;
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }
    toolHandler_->mouseDown(e);
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent& e) {
    if (isAutoTuneProcessing()) {
        return;
    }

    if (interactionState_.isPanning) {
        int deltaX = e.x - interactionState_.dragStartPos.x;
        int deltaY = e.y - interactionState_.dragStartPos.y;
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
    if (isAutoTuneProcessing()) {
        return;
    }

    if (interactionState_.isPanning) {
        interactionState_.isPanning = false;
        setCurrentTool(currentTool_);
        grabKeyboardFocus();
        return;
    }

    toolHandler_->mouseUp(e);
    grabKeyboardFocus();
}

void PianoRollComponent::mouseDoubleClick(const juce::MouseEvent& e) {
    // Ignore double-clicks in piano key area
    if (e.x <= pianoKeyWidth_)
        return;

    // Ignore if panning
    if (juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey))
        return;

    constexpr int inset = 12;
    constexpr int timelineExtendedHitArea = 20;
    const int timelineBottomExtended = inset + rulerHeight_ + timelineExtendedHitArea;

    if (e.y < timelineBottomExtended) {
        // Double-click in Timeline area: play from this position
        double clickedTime = xToTime(e.x);
        if (clickedTime >= 0) {
            listeners_.call([clickedTime](Listener& l) { l.playFromPositionRequested(clickedTime); });
        }
        return;
    }

    // Double-click in editing area: check if we hit a Note
    double clickedTime = xToTime(e.x);
    double trackRelativeTime = clickedTime - trackOffsetSeconds_;
    float clickedPitch = yToFreq(static_cast<float>(e.y));

    if (trackRelativeTime >= 0) {
        auto& notes = getCurrentClipNotes();
        for (auto& note : notes) {
            if (trackRelativeTime >= note.startTime && trackRelativeTime < note.endTime) {
                float adjustedPitch = note.getAdjustedPitch();
                if (std::abs(adjustedPitch - clickedPitch) <= 100.0f) {
                    // Double-clicked on a Note: do NOT play (reserved for future use)
                    return;
                }
            }
        }
    }

    // Double-click on empty area: play from this position
    if (clickedTime >= 0) {
        listeners_.call([clickedTime](Listener& l) { l.playFromPositionRequested(clickedTime); });
    }
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
    if (isAutoTuneProcessing()) {
        return false;
    }
    return toolHandler_->keyPressed(key);
}

void PianoRollComponent::visibilityChanged()
{
    // 当组件变为可见时，自动获取键盘焦点
    // 这确保用户无需手动点击即可使用快捷键（如 Ctrl+A 全选）
    if (isShowing() && isVisible())
    {
        // 使用 callAfterDelay 确保在消息循环处理完成后获取焦点
        // 这是必要的，因为组件刚刚显示时可能还不能立即接收焦点
        juce::Component::SafePointer<PianoRollComponent> safeThis(this);
        juce::Timer::callAfterDelay(10, [safeThis]() {
            if (safeThis != nullptr && safeThis->isShowing())
            {
                safeThis->grabKeyboardFocus();
            }
        });
    }
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

    ctx.hasF0Selection = interactionState_.selection.hasF0Selection;
    ctx.f0SelectionStartFrame = interactionState_.selection.selectedF0StartFrame;
    ctx.f0SelectionEndFrame = interactionState_.selection.selectedF0EndFrame;

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
        return false;
    }

    if (currentTrackId_ < 0 || currentClipId_ == 0) {
        AppLogger::log("AutoTuneTrace: AUTO failed - missing valid clip context");
        return false;
    }

    // Check for selected notes -- AutoTune now requires a selection
    auto& clipNotes = getCurrentClipNotes();
    bool hasSelected = false;
    double selMinTime = 1e30;
    double selMaxTime = -1e30;
    for (auto& n : clipNotes) {
        if (n.selected) {
            hasSelected = true;
            selMinTime = std::min(selMinTime, n.startTime);
            selMaxTime = std::max(selMaxTime, n.endTime);
        }
    }

    if (!hasSelected) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon,
            "Auto Tune",
            juce::CharPointer_UTF8("\xe8\xaf\xb7\xe5\x85\x88\xe9\x80\x89\xe4\xb8\xad\xe9\x9c\x80\xe8\xa6\x81\xe8\x87\xaa\xe5\x8a\xa8\xe8\xb0\x83\xe9\x9f\xb3\xe7\x9a\x84\xe9\x9f\xb3\xe7\xac\xa6\xe3\x80\x82"),  // "请先选中需要自动调音的音符。"
            "OK");
        return false;
    }

    if (correctionInFlight_.exchange(true, std::memory_order_acq_rel)) {
        AppLogger::log("AutoTuneTrace: AUTO processing, ignoring duplicate request");
        return false;
    }

    AppLogger::log("AutoTuneTrace: proceeding with AUTO on selected notes");

    // Apply ScaleSnap to selected notes' pitches
    const bool useScaleSnap = (scaleType_ != 3);
    if (useScaleSnap) {
        ScaleSnapConfig snapCfg;
        snapCfg.root = scaleRootNote_ % 12;
        Scale snapScale = Scale::Major;
        if (scaleType_ == 2) {
            snapScale = Scale::Minor;
        }
        snapCfg.mode = (snapScale == Scale::Minor) ? ScaleMode::Minor : ScaleMode::Major;

        for (auto& n : clipNotes) {
            if (n.selected) {
                float midi = PitchUtils::freqToMidi(n.pitch);
                float snappedMidi = snapCfg.snapMidi(midi);
                n.pitch = Note::midiToFrequency(static_cast<int>(std::round(snappedMidi)));
            }
        }
    }

    // Set retune/vibrato parameters on selected notes
    for (auto& n : clipNotes) {
        if (n.selected) {
            n.retuneSpeed = currentRetuneSpeed_;
            n.vibratoDepth = currentVibratoDepth_;
            n.vibratoRate = currentVibratoRate_;
        }
    }

    // Compute frame range from selected notes
    const double frameDuration = hopSize_ / f0SampleRate_;
    int startFrame = static_cast<int>(selMinTime / frameDuration);
    int endFrame = static_cast<int>(selMaxTime / frameDuration);
    startFrame = std::max(0, startFrame);

    auto snapshot = currentCurve_->getSnapshot();
    const auto& originalF0 = snapshot->getOriginalF0();
    if (originalF0.empty()) {
        correctionInFlight_.store(false, std::memory_order_release);
        if (onRenderComplete_) onRenderComplete_();
        return false;
    }
    endFrame = std::min(static_cast<int>(originalF0.size()) - 1, endFrame);
    if (endFrame <= startFrame) {
        correctionInFlight_.store(false, std::memory_order_release);
        if (onRenderComplete_) onRenderComplete_();
        return false;
    }

    undoSupport_->beginTransaction("Auto Tune");

    DBG("PianoRollComponent::applyAutoTuneToSelection - enqueuing correction: startFrame="
        + juce::String(startFrame) + " endFrame=" + juce::String(endFrame));

    enqueueNoteBasedCorrectionAsync(startFrame, endFrame + 1,
        currentRetuneSpeed_, currentVibratoDepth_, currentVibratoRate_, true);

    repaint();

    return true;
}

void PianoRollComponent::setNotes(const std::vector<Note>& notes) {
    auto& clipNotes = getCurrentClipNotes();
    clipNotes = notes;
    std::sort(clipNotes.begin(), clipNotes.end(), [](const Note& a, const Note& b) {
        return a.startTime < b.startTime;
    });
    for (size_t i = 1; i < clipNotes.size(); ++i) {
        if (clipNotes[i - 1].endTime > clipNotes[i].startTime) {
            clipNotes[i - 1].endTime = clipNotes[i].startTime;
        }
    }
    clipNotes.erase(
        std::remove_if(clipNotes.begin(), clipNotes.end(), [](const Note& n) {
            return n.endTime <= n.startTime;
        }),
        clipNotes.end());
    
    updateScrollBars();
    repaint();
}



// ============================================================================
// Undo/Redo 相关上下文
// ============================================================================

void PianoRollComponent::setCurrentClipContext(int trackId, uint64_t clipId)
{
    currentTrackId_ = trackId;
    currentClipId_ = clipId;
    clipContextGeneration_.fetch_add(1, std::memory_order_release);
    if (correctionWorker_) {
        correctionWorker_->setClipContext(trackId, clipId);
    }
}

void PianoRollComponent::clearClipContext()
{
    currentTrackId_ = -1;
    currentClipId_ = 0;
    correctionInFlight_.store(false, std::memory_order_release);
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
    refreshAfterUndoRedoWithRange(-1, -1);
}

void PianoRollComponent::refreshAfterUndoRedoWithRange(int startFrame, int endFrame) {
    updateScrollBars();
    
    if (currentCurve_) {
        int affectedStartFrame = startFrame;
        int affectedEndFrame = endFrame;
        
        // If no range provided (diff found no changes), skip render
        if (affectedStartFrame < 0 || affectedEndFrame < 0) {
            AppLogger::log("refreshAfterUndoRedo: no diff range (no changes detected), skipping render");
            repaint();
            return;
        }
        
        AppLogger::log("refreshAfterUndoRedo: using provided range [" + juce::String(affectedStartFrame) + ", " + juce::String(affectedEndFrame) + "]");
        
        listeners_.call([affectedStartFrame, affectedEndFrame](Listener& l) {
            l.pitchCurveEdited(affectedStartFrame, affectedEndFrame);
        });
    }
    repaint();
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

std::vector<Note>& PianoRollComponent::getCurrentClipNotes() {
    if (!processor_ || currentTrackId_ < 0 || currentClipId_ == 0) {
        static std::vector<Note> empty;
        return empty;
    }
    int clipIndex = processor_->getClipIndexById(currentTrackId_, currentClipId_);
    if (clipIndex < 0) {
        static std::vector<Note> empty;
        return empty;
    }
    return processor_->getClipNotesRef(currentTrackId_, clipIndex);
}

std::vector<Note> PianoRollComponent::getCurrentClipNotesCopy() const {
    if (!processor_ || currentTrackId_ < 0 || currentClipId_ == 0) {
        return {};
    }
    int clipIndex = processor_->getClipIndexById(currentTrackId_, currentClipId_);
    if (clipIndex < 0) {
        return {};
    }
    return processor_->getClipNotes(currentTrackId_, clipIndex);
}

bool PianoRollComponent::isAutoTuneProcessing() const
{
    return correctionInFlight_.load(std::memory_order_acquire);
}

void PianoRollComponent::updateScrollBars() {
    double maxTime = 0.0;
    if (audioBuffer_) {
        maxTime = (double)audioBuffer_->getNumSamples() / PianoRollComponent::kAudioSampleRate;
    } else {
        const auto& notes = getCurrentClipNotes();
        for (const auto& note : notes) {
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
