#include "PianoRollComponent.h"
#include "../../Utils/LocalizationManager.h"
#include "../Utils/AppLogger.h"
#include "../../Utils/PianoRollEditAction.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "../DSP/ChromaKeyDetector.h"
#include "../Utils/NoteGenerator.h"
#include "../Utils/SimdPerceptualPitchEstimator.h"
#include "../Utils/ZoomSensitivityConfig.h"
#include "../../PluginProcessor.h"
#include "FrameScheduler.h"
#include "UiText.h"
#include "ToolbarIcons.h"
#include "../../Utils/AudioEditingScheme.h"
#include "Utils/PianoKeyAudition.h"
#include "Inference/GameTypes.h"
namespace OpenTune {

namespace {

constexpr uint32_t toInvalidationMask(PianoRollVisualInvalidationReason reason)
{
    return static_cast<uint32_t>(reason);
}

FrameScheduler::Priority toFrameSchedulerPriority(PianoRollVisualInvalidationPriority priority)
{
    return static_cast<FrameScheduler::Priority>(static_cast<int>(priority));
}

std::vector<CorrectedSegment> copyCorrectedSegments(const std::shared_ptr<PitchCurve>& curve)
{
    std::vector<CorrectedSegment> copiedSegments;
    if (curve == nullptr) {
        return copiedSegments;
    }

    const auto snapshot = curve->getSnapshot();
    copiedSegments.reserve(snapshot->getCorrectedSegments().size());
    for (const auto& segment : snapshot->getCorrectedSegments()) {
        copiedSegments.push_back(segment);
    }
    return copiedSegments;
}

} // namespace

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
            setScrollMode(ScrollMode::Continuous);
            scrollModeToggleButton_.setButtonText("Cont");
        } else {
            setScrollMode(ScrollMode::Page);
            scrollModeToggleButton_.setButtonText("Page");
        }
    };
    addAndMakeVisible(scrollModeToggleButton_);
    scrollModeToggleButton_.setTooltip(LOC(kTooltipScrollMode));

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
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport));
    };
    addAndMakeVisible(timeUnitToggleButton_);
    timeUnitToggleButton_.setTooltip(LOC(kTooltipTimeUnit));

    addAndMakeVisible(playheadOverlay_);
    playheadOverlay_.setPianoKeyWidth(pianoKeyWidth_);
    playheadOverlay_.setPlayheadColour(juce::Colour{0xFFE74C3C});

    scrollVBlankAttachment_ = std::make_unique<juce::VBlankAttachment>(
        this, [this](double timestampSec) { onScrollVBlankCallback(timestampSec); });
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

    toolCtx.getCommittedNotes = [this]() -> const std::vector<Note>& { return getCommittedNotes(); };
    toolCtx.getDisplayNotes = [this]() -> const std::vector<Note>& { return getDisplayedNotes(); };
    toolCtx.getNoteDraft = [this]() -> NoteInteractionDraft& { return getNoteDraft(); };
    toolCtx.beginNoteDraft = [this]() { beginNoteDraft(); };
    toolCtx.commitNoteDraft = [this]() { return commitNoteDraft(); };
    toolCtx.clearNoteDraft = [this]() { clearNoteDraft(); };
    toolCtx.commitNotesAndSegments = [this](const std::vector<Note>& notes,
                                            const std::vector<CorrectedSegment>& segments) {
        return commitEditedMaterializationNotesAndSegments(notes, segments);
    };
    toolCtx.getPitchCurve = [this]() { return currentCurve_; };
    toolCtx.getOriginalF0 = [this]() -> std::vector<float> {
        if (!currentCurve_) return {};
        auto snap = currentCurve_->getSnapshot();
        if (!snap) return {};
        return snap->getOriginalF0();
    };
    toolCtx.getF0Timeline = [this]() -> F0Timeline {
        return currentF0Timeline();
    };
    toolCtx.getMinMidi = [this]() { return minMidi_; };
    toolCtx.getMaxMidi = [this]() { return maxMidi_; };
    toolCtx.getRetuneSpeed = [this]() { return currentRetuneSpeed_; };
    toolCtx.getVibratoDepth = [this]() { return currentVibratoDepth_; };
    toolCtx.getVibratoRate = [this]() { return currentVibratoRate_; };
    toolCtx.recalculatePIP = [this](Note& note) -> float { return recalculatePIP(note); };
    toolCtx.getShortcutSettings = [this]() -> const KeyShortcutConfig::KeyShortcutSettings& { return shortcutSettings_; };
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
    toolCtx.getPianoKeyWidth = [this]() { return pianoKeyWidth_; };
    toolCtx.getMaterializationProjection = [this]() { return materializationProjection_; };
    toolCtx.projectTimelineTimeToMaterialization = [this](double timelineSeconds) {
        return projectTimelineTimeToMaterialization(timelineSeconds);
    };
    toolCtx.projectMaterializationTimeToTimeline = [this](double materializationSeconds) {
        return projectMaterializationTimeToTimeline(materializationSeconds);
    };
    toolCtx.getNotesBounds = [this](const std::vector<Note>& notes) { return getNotesBounds(notes); };
    toolCtx.getSelectionBounds = [this]() { return getSelectionBounds(); };
    toolCtx.getHandDrawPreviewBounds = [this]() { return getHandDrawPreviewBounds(); };
    toolCtx.getLineAnchorPreviewBounds = [this]() { return getLineAnchorPreviewBounds(); };
    toolCtx.getNoteDragCurvePreviewBounds = [this]() { return getNoteDragCurvePreviewBounds(); };

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
    toolCtx.getNoteDragPreviewF0 = [this]() -> std::vector<float>& { return interactionState_.noteDrag.previewF0; };
    toolCtx.getNoteDragPreviewStartFrame = [this]() { return interactionState_.noteDrag.previewStartFrame; };
    toolCtx.setNoteDragPreviewStartFrame = [this](int v) { interactionState_.noteDrag.previewStartFrame = v; };
    toolCtx.getNoteDragPreviewEndFrameExclusive = [this]() { return interactionState_.noteDrag.previewEndFrameExclusive; };
    toolCtx.setNoteDragPreviewEndFrameExclusive = [this](int v) { interactionState_.noteDrag.previewEndFrameExclusive = v; };

    toolCtx.invalidateVisual = [this](const juce::Rectangle<int>& dirtyArea) {
        invalidateInteractionArea(dirtyArea);
    };
    toolCtx.setMouseCursor = [this](const juce::MouseCursor& c) { setMouseCursor(c); };
    toolCtx.grabKeyboardFocus = [this]() { grabKeyboardFocus(); };
    toolCtx.getAudioEditingScheme = [this]() { return audioEditingScheme_; };
    toolCtx.notifyPlayheadChange = [this](double time) {
        listeners_.call([time](Listener& l) { l.playheadPositionChangeRequested(time); });
        userScrollHold_ = false;
        if (!isPlaying_.load(std::memory_order_relaxed)) {
            playheadOverlay_.setPlayheadSeconds(time);
        } else {
            // 播放中 seek：设置 pending，VBlank 用 pending 值居中直到 host 确认
            pendingSeekTime_ = time;
            playheadOverlay_.setPlayheadSeconds(time);
            // 立即居中到新位置，不需要 smooth offset
            const auto bounds = getTimelineViewportBounds();
            const int visibleWidth = bounds.getWidth() - pianoKeyWidth_;
            if (visibleWidth > 0) {
                const float absX = static_cast<float>(getPlayheadAbsolutePixelX(time));
                const int centeredScroll = std::max(0, static_cast<int>(std::round(absX - visibleWidth / 2.0f)));
                scrollSeekOffset_ = 0.0f;
                setScrollOffset(centeredScroll);
            }
        }
    };
    toolCtx.notifyPitchCurveEdited = [this](int s, int e) {
        listeners_.call([s, e](Listener& l) { l.pitchCurveEdited(s, e); });
    };

    toolCtx.applyManualCorrection = [this](std::vector<PianoRollToolHandler::ManualCorrectionOp> ops, int s, int e, bool render) {
        return enqueueManualCorrectionPatchAsync(ops, s, e, render);
    };
    toolCtx.selectNotesOverlappingFrames = [this](int startFrame, int endFrameExclusive) {
        return selectNotesOverlappingFrames(startFrame, endFrameExclusive);
    };
    toolCtx.findLineAnchorSegmentNear = [this](int x, int y) { return findLineAnchorSegmentNear(x, y); };
    toolCtx.selectLineAnchorSegment = [this](int idx) { selectLineAnchorSegment(idx); };
    toolCtx.toggleLineAnchorSegmentSelection = [this](int idx) { toggleLineAnchorSegmentSelection(idx); };
    toolCtx.clearLineAnchorSegmentSelection = [this]() { clearLineAnchorSegmentSelection(); };
    toolCtx.setUndoDescription = [this](juce::String desc) { pendingUndoDescription_ = std::move(desc); };

    // Reference note drag support
    toolCtx.hitTestReferenceNote = [this](double time, float pitch) -> bool {
        if (cachedReferenceNotes_.empty()) return false;
        const double refOffset = cachedReferenceTimeOffset_;
        const float pitchMidi = 69.0f + 12.0f * std::log2(pitch / 440.0f) - 0.5f;
        for (const auto& ref : cachedReferenceNotes_) {
            if (!ref.voiced) continue;
            const double onset = ref.onset + refOffset;
            const double offset = ref.offset + refOffset;
            if (time >= onset && time <= offset && std::abs(pitchMidi - ref.midiPitch) < 1.0f) {
                return true;
            }
        }
        return false;
    };
    toolCtx.getReferenceTimeOffset = [this]() -> double {
        if (processor_ == nullptr || editedMaterializationId_ == 0) return 0.0;
        return processor_->getMaterializationStore()->getReferenceTimeOffset(editedMaterializationId_);
    };
    toolCtx.setReferenceTimeOffset = [this](double offset) {
        if (processor_ == nullptr || editedMaterializationId_ == 0) return;
        processor_->getMaterializationStore()->setReferenceTimeOffset(editedMaterializationId_, offset);
    };

    return toolCtx;
}

void PianoRollComponent::initializeToolHandler() {
    toolHandler_ = std::make_unique<PianoRollToolHandler>(buildToolHandlerContext());
}

PianoRollComponent::PianoRollComponent() {
    initializeUIComponents();
    initializeRenderer();
    initializeCorrectionWorker();
    initializeToolHandler();
}

PianoRollComponent::~PianoRollComponent() {
    if (correctionWorker_) {
        correctionWorker_->stop();
    }
    scrollVBlankAttachment_.reset();
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

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) {
        return false;
    }

    auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
    request->curve = currentCurve_;
    request->notes = getCommittedNotes();
    request->startFrame = 0;
    request->endFrameExclusive = f0tl.endFrameExclusive();
    request->retuneSpeed = retuneSpeed;
    request->vibratoDepth = vibratoDepth;
    request->vibratoRate = vibratoRate;
    request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);

    captureBeforeUndoSnapshot();
    pendingUndoDescription_ = TRANS("自动调音");
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

    AppLogger::log("AutoTune: consumeCompleted kind=" + juce::String(static_cast<int>(completed->kind))
        + " success=" + juce::String(completed->success ? "true" : "false"));

    const bool wasAutoTune = completed->kind == PianoRollCorrectionWorker::AsyncCorrectionRequest::Kind::AutoTuneGenerate;

    bool committedSuccessfully = completed->success;
    if (committedSuccessfully && wasAutoTune) {
        committedSuccessfully = commitCompletedAutoTuneResult(*completed);
    } else if (committedSuccessfully) {
        committedSuccessfully = commitCompletedNoteCorrectionResult(*completed);
    }

    if (committedSuccessfully) {
        const int notifyStart = wasAutoTune ? completed->autoStartFrame : completed->startFrame;
        const int notifyEndExclusive = wasAutoTune ? (completed->autoEndFrame + 1) : completed->endFrameExclusive;
        const int notifyEnd = std::max(notifyStart, notifyEndExclusive - 1);
        listeners_.call([notifyStart, notifyEnd](Listener& l) { l.pitchCurveEdited(notifyStart, notifyEnd); });
    }

    if (wasAutoTune) {
        autoTuneInFlight_.store(false, std::memory_order_release);
    }

    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

bool PianoRollComponent::commitCompletedAutoTuneResult(const PianoRollCorrectionWorker::AsyncCorrectionRequest& completed)
{
    AppLogger::log("AutoTune: commitCompletedAutoTuneResult entry");

    if (processor_ == nullptr || editedMaterializationId_ == 0) {
        AppLogger::log("AutoTune: commitCompleted abort - processor or materializationId null");
        return false;
    }

    const uint64_t currentEpoch = editedMaterializationEpoch_.load(std::memory_order_acquire);
    if (completed.materializationIdSnapshot != editedMaterializationId_
        || completed.materializationEpochSnapshot != currentEpoch) {
        AppLogger::log("AutoTune: commitCompleted abort - epoch mismatch (snapshot="
            + juce::String(completed.materializationEpochSnapshot) + " current=" + juce::String(currentEpoch) + ")");
        return false;
    }

    AppLogger::log("AutoTune: commitCompleted calling commitAutoTuneGeneratedNotes noteCount="
        + juce::String(static_cast<int>(completed.notes.size())));

    if (!processor_->commitAutoTuneGeneratedNotesByMaterializationId(completed.materializationIdSnapshot,
                                                             completed.notes,
                                                             completed.autoStartFrame,
                                                             completed.autoEndFrame + 1,
                                                             completed.retuneSpeed,
                                                             completed.vibratoDepth,
                                                             completed.vibratoRate,
                                                             completed.audioSampleRate)) {
        AppLogger::log("AutoTune: commitAutoTuneGeneratedNotes returned false");
        return false;
    }

    AppLogger::log("AutoTune: commitAutoTuneGeneratedNotes succeeded, refreshing notes");
    refreshEditedMaterializationNotes();

    auto committedCurve = processor_->getMaterializationPitchCurveById(completed.materializationIdSnapshot);
    if (committedCurve == nullptr) {
        AppLogger::log("AutoTune: commitCompleted abort - committedCurve null after commit");
        return false;
    }

    AppLogger::log("AutoTune: setEditedMaterialization + recordUndo");
    setEditedMaterialization(editedMaterializationId_, committedCurve, audioBuffer_, static_cast<int>(audioBufferSampleRate_));
    AppLogger::log("AutoTune: after setEditedMaterialization");
    updateScrollBars();
    AppLogger::log("AutoTune: after outer updateScrollBars");
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
    AppLogger::log("AutoTune: after outer invalidateVisual, before recordUndoAction");
    recordUndoAction(pendingUndoDescription_);
    AppLogger::log("AutoTune: commitCompletedAutoTuneResult done");
    return true;
}

bool PianoRollComponent::commitCompletedNoteCorrectionResult(const PianoRollCorrectionWorker::AsyncCorrectionRequest& completed)
{
    if (processor_ == nullptr || editedMaterializationId_ == 0 || completed.curve == nullptr) {
        return false;
    }

    const uint64_t currentEpoch = editedMaterializationEpoch_.load(std::memory_order_acquire);
    if (completed.materializationIdSnapshot != editedMaterializationId_
        || completed.materializationEpochSnapshot != currentEpoch) {
        return false;
    }

    if (!commitEditedMaterializationCorrectedSegments(copyCorrectedSegments(completed.curve))) {
        return false;
    }

    updateScrollBars();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
    return true;
}

void PianoRollComponent::setProcessor(OpenTuneAudioProcessor* processor)
{
    processor_ = processor;
    refreshEditedMaterializationNotes();
}

void PianoRollComponent::refreshEditedMaterializationNotes()
{
    cachedNotes_.clear();
    if (processor_ == nullptr || editedMaterializationId_ == 0) {
        return;
    }

    auto snapshot = processor_->getMaterializationNotesSnapshotById(editedMaterializationId_);
    cachedNotes_ = std::move(snapshot.notes);
}

const std::vector<Note>& PianoRollComponent::getCommittedNotes() const
{
    return cachedNotes_;
}

const std::vector<Note>& PianoRollComponent::getDisplayedNotes() const
{
    return interactionState_.noteDraft.active ? interactionState_.noteDraft.workingNotes : cachedNotes_;
}

NoteInteractionDraft& PianoRollComponent::getNoteDraft()
{
    return interactionState_.noteDraft;
}

const NoteInteractionDraft& PianoRollComponent::getNoteDraft() const
{
    return interactionState_.noteDraft;
}

void PianoRollComponent::beginNoteDraft()
{
    interactionState_.noteDraft.active = true;
    interactionState_.noteDraft.baselineNotes = cachedNotes_;
    interactionState_.noteDraft.workingNotes = cachedNotes_;
}

bool PianoRollComponent::commitNoteDraft()
{
    if (!interactionState_.noteDraft.active) {
        return true;
    }

    const auto success = commitEditedMaterializationNotes(interactionState_.noteDraft.workingNotes);
    if (success) {
        interactionState_.noteDraft.clear();
    }
    return success;
}

void PianoRollComponent::clearNoteDraft()
{
    interactionState_.noteDraft.clear();
}

bool PianoRollComponent::commitEditedMaterializationNotes(const std::vector<Note>& notes)
{
    if (processor_ == nullptr || editedMaterializationId_ == 0) {
        return false;
    }

    if (!undoSnapshotCaptured_)
        captureBeforeUndoSnapshot();

    if (!processor_->setMaterializationNotesById(editedMaterializationId_, notes)) {
        return false;
    }

    refreshEditedMaterializationNotes();
    recordUndoAction(pendingUndoDescription_);
    return true;
}

bool PianoRollComponent::commitEditedMaterializationNotesAndSegments(const std::vector<Note>& notes,
                                                             const std::vector<CorrectedSegment>& segments)
{
    if (processor_ == nullptr || editedMaterializationId_ == 0) {
        return false;
    }

    if (!undoSnapshotCaptured_)
        captureBeforeUndoSnapshot();

    if (!processor_->commitMaterializationNotesAndSegmentsById(editedMaterializationId_, notes, segments)) {
        return false;
    }

    refreshEditedMaterializationNotes();

    auto committedCurve = processor_->getMaterializationPitchCurveById(editedMaterializationId_);
    if (committedCurve != nullptr) {
        setEditedMaterialization(editedMaterializationId_, committedCurve, audioBuffer_, static_cast<int>(audioBufferSampleRate_));
    }

    recordUndoAction(pendingUndoDescription_);
    return true;
}

bool PianoRollComponent::commitEditedMaterializationCorrectedSegments(const std::vector<CorrectedSegment>& segments)
{
    if (processor_ == nullptr || editedMaterializationId_ == 0) {
        return false;
    }

    if (!undoSnapshotCaptured_)
        captureBeforeUndoSnapshot();

    if (!processor_->setMaterializationCorrectedSegmentsById(editedMaterializationId_, segments)) {
        return false;
    }

    auto committedCurve = processor_->getMaterializationPitchCurveById(editedMaterializationId_);
    if (committedCurve != nullptr) {
        setEditedMaterialization(editedMaterializationId_, committedCurve, audioBuffer_, static_cast<int>(audioBufferSampleRate_));
    }

    recordUndoAction(pendingUndoDescription_);
    return true;
}

std::vector<CorrectedSegment> PianoRollComponent::getCurrentSegments() const
{
    if (!currentCurve_) return {};
    auto snap = currentCurve_->getSnapshot();
    if (!snap) return {};
    return snap->getCorrectedSegments();
}

void PianoRollComponent::captureBeforeUndoSnapshot()
{
    beforeUndoNotes_ = cachedNotes_;
    beforeUndoSegments_ = getCurrentSegments();
    undoSnapshotCaptured_ = true;
}

void PianoRollComponent::recordUndoAction(const juce::String& description)
{
    if (processor_ == nullptr || editedMaterializationId_ == 0 || !undoSnapshotCaptured_)
        return;

    AppLogger::log("AutoTune: recordUndoAction entry beforeNotes=" + juce::String(static_cast<int>(beforeUndoNotes_.size()))
        + " beforeSegments=" + juce::String(static_cast<int>(beforeUndoSegments_.size()))
        + " cachedNotes=" + juce::String(static_cast<int>(cachedNotes_.size())));

    auto afterNotes = cachedNotes_;
    auto afterSegments = getCurrentSegments();

    AppLogger::log("AutoTune: recordUndoAction afterSegments=" + juce::String(static_cast<int>(afterSegments.size()))
        + " constructing PianoRollEditAction");

    auto action = std::make_unique<PianoRollEditAction>(
        *processor_,
        editedMaterializationId_,
        description.isNotEmpty() ? description : TRANS("编辑"),
        std::move(beforeUndoNotes_),
        std::move(afterNotes),
        std::move(beforeUndoSegments_),
        std::move(afterSegments));

    AppLogger::log("AutoTune: recordUndoAction before addAction");
    processor_->getUndoManager().addAction(std::move(action));
    AppLogger::log("AutoTune: recordUndoAction after addAction");
    pendingUndoDescription_ = {};
    undoSnapshotCaptured_ = false;
}

bool PianoRollComponent::selectNotesOverlappingFrames(int startFrame, int endFrameExclusive)
{
    auto notes = getCommittedNotes();
    const auto f0tl = currentF0Timeline();
    if (notes.empty() || f0tl.isEmpty()) {
        interactionState_.selection.hasSelectionArea = false;
        interactionState_.selection.isSelectingArea = false;
        interactionState_.selection.clearF0Selection();
        return false;
    }

    const auto selectionRange = f0tl.rangeForFrames(startFrame, endFrameExclusive);

    bool anyOverlap = false;
    bool selectionChanged = false;
    for (auto& note : notes) {
        const auto noteRange = f0tl.nonEmptyRangeForTimes(note.startTime, note.endTime);
        const bool overlaps = std::min(selectionRange.endFrameExclusive, noteRange.endFrameExclusive)
            > std::max(selectionRange.startFrame, noteRange.startFrame);
        anyOverlap = anyOverlap || overlaps;
        if (note.selected != overlaps) {
            note.selected = overlaps;
            selectionChanged = true;
        }
    }

    if (selectionChanged) {
        commitEditedMaterializationNotes(notes);
    }

    interactionState_.selection.hasSelectionArea = false;
    interactionState_.selection.isSelectingArea = false;
    if (anyOverlap) {
        interactionState_.selection.setF0Range(selectionRange.startFrame, selectionRange.endFrameExclusive);
    } else {
        interactionState_.selection.clearF0Selection();
    }

    return anyOverlap;
}

juce::Rectangle<int> PianoRollComponent::getNoteBounds(const Note& note) const
{
    const float adjustedPitch = note.getAdjustedPitch();
    if (adjustedPitch <= 0.0f) {
        return {};
    }

    if (materializationProjection_.isValid()
        && (note.endTime <= 0.0
            || note.startTime >= materializationProjection_.materializationDurationSeconds)) {
        return {};
    }

    const int x1 = timeToX(projectMaterializationTimeToTimeline(note.startTime));
    const int x2 = timeToX(projectMaterializationTimeToTimeline(note.endTime));
    const int width = std::max(1, x2 - x1);
    const float midi = freqToMidi(adjustedPitch);
    const float y = midiToY(midi) - (pixelsPerSemitone_ * 0.5f);
    const int top = static_cast<int>(std::floor(y));
    const int height = std::max(1, static_cast<int>(std::ceil(pixelsPerSemitone_)));
    return juce::Rectangle<int>(x1, top, width, height)
        .expanded(4)
        .getIntersection(getTimelineViewportBounds());
}

juce::Rectangle<int> PianoRollComponent::getNotesBounds(const std::vector<Note>& notes) const
{
    juce::Rectangle<int> bounds;
    bool hasBounds = false;
    for (const auto& note : notes) {
        const auto noteBounds = getNoteBounds(note);
        if (noteBounds.isEmpty()) {
            continue;
        }

        bounds = hasBounds ? bounds.getUnion(noteBounds) : noteBounds;
        hasBounds = true;
    }

    return hasBounds ? bounds : juce::Rectangle<int>();
}

juce::Rectangle<int> PianoRollComponent::getSelectionBounds() const
{
    if (!interactionState_.selection.hasSelectionArea) {
        return {};
    }

    const double startTime = std::min(interactionState_.selection.selectionStartTime,
                                      interactionState_.selection.selectionEndTime);
    const double endTime = std::max(interactionState_.selection.selectionStartTime,
                                    interactionState_.selection.selectionEndTime);
    const float minMidi = std::min(interactionState_.selection.selectionStartMidi,
                                   interactionState_.selection.selectionEndMidi);
    const float maxMidi = std::max(interactionState_.selection.selectionStartMidi,
                                   interactionState_.selection.selectionEndMidi);

    const int x1 = timeToX(projectMaterializationTimeToTimeline(startTime));
    const int x2 = timeToX(projectMaterializationTimeToTimeline(endTime));
    const int y1 = static_cast<int>(std::floor(midiToY(maxMidi)));
    const int y2 = static_cast<int>(std::ceil(midiToY(minMidi)));
    return juce::Rectangle<int>(std::min(x1, x2),
                                std::min(y1, y2),
                                std::max(1, std::abs(x2 - x1)),
                                std::max(1, std::abs(y2 - y1)))
        .expanded(4)
        .getIntersection(getTimelineViewportBounds());
}

juce::Rectangle<int> PianoRollComponent::getHandDrawPreviewBounds() const
{
    if (!interactionState_.drawing.isDrawingF0
        || interactionState_.drawing.handDrawBuffer.empty()
        || interactionState_.drawing.dirtyStartTime < 0.0
        || interactionState_.drawing.dirtyEndTime < 0.0) {
        return {};
    }

    const int x1 = timeToX(projectMaterializationTimeToTimeline(std::min(interactionState_.drawing.dirtyStartTime,
                                                                 interactionState_.drawing.dirtyEndTime)));
    const int x2 = timeToX(projectMaterializationTimeToTimeline(std::max(interactionState_.drawing.dirtyStartTime,
                                                                 interactionState_.drawing.dirtyEndTime)));
    return juce::Rectangle<int>(std::min(x1, x2),
                                getTimelineViewportBounds().getY(),
                                std::max(1, std::abs(x2 - x1)),
                                getTimelineViewportBounds().getHeight())
        .expanded(4)
        .getIntersection(getTimelineViewportBounds());
}

juce::Rectangle<int> PianoRollComponent::getLineAnchorPreviewBounds() const
{
    if (!interactionState_.drawing.isPlacingAnchors || interactionState_.drawing.pendingAnchors.empty()) {
        return {};
    }

    juce::Rectangle<float> bounds;
    bool hasBounds = false;
    auto includePoint = [&](float x, float y) {
        const auto pointBounds = juce::Rectangle<float>(x - 4.0f, y - 4.0f, 8.0f, 8.0f);
        bounds = hasBounds ? bounds.getUnion(pointBounds) : pointBounds;
        hasBounds = true;
    };

    for (const auto& anchor : interactionState_.drawing.pendingAnchors) {
        includePoint(static_cast<float>(timeToX(projectMaterializationTimeToTimeline(anchor.time))), freqToY(anchor.freq));
    }
    includePoint(interactionState_.drawing.currentMousePos.x, interactionState_.drawing.currentMousePos.y);

    return hasBounds ? bounds.getSmallestIntegerContainer().expanded(4).getIntersection(getTimelineViewportBounds())
                     : juce::Rectangle<int>();
}

juce::Rectangle<int> PianoRollComponent::getNoteDragCurvePreviewBounds() const
{
    if (interactionState_.noteDrag.previewStartFrame < 0
        || interactionState_.noteDrag.previewEndFrameExclusive <= interactionState_.noteDrag.previewStartFrame
        || interactionState_.noteDrag.previewF0.empty()) {
        return {};
    }

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) {
        return {};
    }
    juce::Rectangle<float> bounds;
    bool hasBounds = false;
    for (int frame = interactionState_.noteDrag.previewStartFrame;
         frame < interactionState_.noteDrag.previewEndFrameExclusive;
         ++frame) {
        const int relIndex = frame - interactionState_.noteDrag.previewStartFrame;
        if (relIndex < 0 || relIndex >= static_cast<int>(interactionState_.noteDrag.previewF0.size())) {
            continue;
        }

        const float f0 = interactionState_.noteDrag.previewF0[static_cast<std::size_t>(relIndex)];
        if (f0 <= 0.0f) {
            continue;
        }

        const float x = static_cast<float>(timeToX(projectMaterializationTimeToTimeline(f0tl.timeAtFrame(frame))));
        const float y = freqToY(f0);
        const auto pointBounds = juce::Rectangle<float>(x - 2.0f, y - 2.0f, 4.0f, 4.0f);
        bounds = hasBounds ? bounds.getUnion(pointBounds) : pointBounds;
        hasBounds = true;
    }

    return hasBounds ? bounds.getSmallestIntegerContainer().expanded(4).getIntersection(getTimelineViewportBounds())
                     : juce::Rectangle<int>();
}

void PianoRollComponent::invalidateInteractionArea(const juce::Rectangle<int>& dirtyArea)
{
    if (dirtyArea.isEmpty()) {
        return;
    }

    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Interaction),
                     dirtyArea,
                     PianoRollVisualInvalidationPriority::Interactive);
}

bool PianoRollComponent::enqueueManualCorrectionPatchAsync(const std::vector<PianoRollToolHandler::ManualCorrectionOp>& ops,
                                                           int dirtyStartFrame,
                                                           int dirtyEndFrame,
                                                           bool triggerRenderEvent)
{
    if (!currentCurve_ || ops.empty()) {
        return false;
    }

    auto editedCurve = currentCurve_->clone();
    for (const auto& op : ops) {
        if (op.endFrameExclusive <= op.startFrame) {
            continue;
        }

        editedCurve->setManualCorrectionRange(
            op.startFrame,
            op.endFrameExclusive,
            op.f0Data,
            op.source,
            op.retuneSpeed);
    }

    if (!commitEditedMaterializationCorrectedSegments(copyCorrectedSegments(editedCurve))) {
        return false;
    }

    if (triggerRenderEvent && dirtyEndFrame >= dirtyStartFrame) {
        listeners_.call([dirtyStartFrame, dirtyEndFrame](Listener& l) {
            l.pitchCurveEdited(dirtyStartFrame, dirtyEndFrame);
        });
    }

    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
    return true;
}

void PianoRollComponent::enqueueNoteBasedCorrectionAsync(const std::vector<Note>& notes,
                                                         int startFrame,
                                                         int endFrameExclusive,
                                                         float retuneSpeed,
                                                         float vibratoDepth,
                                                         float vibratoRate)
{
    if (!currentCurve_) {
        return;
    }

    auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
    request->curve = currentCurve_->clone();
    request->notes = notes;
    request->startFrame = startFrame;
    request->endFrameExclusive = endFrameExclusive;
    request->retuneSpeed = retuneSpeed;
    request->vibratoDepth = vibratoDepth;
    request->vibratoRate = vibratoRate;
    request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);
    request->materializationEpochSnapshot = editedMaterializationEpoch_.load(std::memory_order_acquire);
    request->materializationIdSnapshot = editedMaterializationId_;
    if (!undoSnapshotCaptured_)
        captureBeforeUndoSnapshot();

    correctionWorker_->enqueue(request);
}

void PianoRollComponent::drawSelectedOriginalF0Curve(juce::Graphics& g, const std::vector<float>& originalF0) {
    const auto& notes = getDisplayedNotes();
    bool hasNoteSelection = false;
    for (const auto& note : notes) {
        if (note.selected) { hasNoteSelection = true; break; }
    }

    if (!hasNoteSelection && !interactionState_.selection.hasSelectionArea) return;

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return;

    juce::Path selectedPath;
    bool pathStarted = false;

    auto appendFrameRange = [&](F0FrameRange range, std::function<bool(float)> acceptsFrame) {
        bool segmentStarted = false;
        for (int frame = range.startFrame; frame < range.endFrameExclusive; ++frame) {
            const float f0 = originalF0[static_cast<std::size_t>(frame)];
            if (f0 <= 0.0f || !acceptsFrame(f0)) {
                segmentStarted = false;
                continue;
            }

            const float y = freqToY(f0);
            const float x = static_cast<float>(timeToX(projectMaterializationTimeToTimeline(f0tl.timeAtFrame(frame))));
            if (!pathStarted || !segmentStarted) {
                selectedPath.startNewSubPath(x, y);
                pathStarted = true;
                segmentStarted = true;
                continue;
            }

            const auto last = selectedPath.getCurrentPosition();
            if (std::abs(x - last.x) > 50.0f) {
                selectedPath.startNewSubPath(x, y);
            } else {
                selectedPath.lineTo(x, y);
            }
        }
    };

    for (const auto& note : notes) {
        if (note.selected) {
            appendFrameRange(f0tl.nonEmptyRangeForTimes(note.startTime, note.endTime), [](float) { return true; });
        }
    }

    if (interactionState_.selection.hasSelectionArea) {
        const double startTime = std::min(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
        const double endTime = std::max(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
        const float minMidi = std::min(interactionState_.selection.selectionStartMidi, interactionState_.selection.selectionEndMidi);
        const float maxMidi = std::max(interactionState_.selection.selectionStartMidi, interactionState_.selection.selectionEndMidi);
        appendFrameRange(f0tl.nonEmptyRangeForTimes(startTime, endTime), [&](float f0) {
            const float midi = freqToMidi(f0);
            return midi >= minMidi && midi <= maxMidi;
        });
    }

    if (!selectedPath.isEmpty()) {
        g.setColour(UIColors::originalF0.withAlpha(0.85f));
        juce::PathStrokeType strokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.strokePath(selectedPath, strokeType);
    }
}

void PianoRollComponent::drawHandDrawPreview(juce::Graphics& g) {
    if (!interactionState_.drawing.isDrawingF0 || currentTool_ != ToolId::HandDraw || interactionState_.drawing.handDrawBuffer.empty() || !currentCurve_) return;

    auto snapshot = currentCurve_->getSnapshot();
    const auto& originalF0 = snapshot->getOriginalF0();
    if (originalF0.empty() || interactionState_.drawing.handDrawBuffer.size() != originalF0.size()) return;

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return;
    juce::Colour previewColour = juce::Colour(0xFF00DDDD);
    juce::Path previewPath;
    bool pathStarted = false;

    for (int i = 0; i < f0tl.endFrameExclusive(); ++i) {
        float f0 = interactionState_.drawing.handDrawBuffer[static_cast<size_t>(i)];
        if (f0 > 0.0f) {
            float y = freqToY(f0);
            double timePos = f0tl.timeAtFrame(i);
            float x = static_cast<float>(timeToX(projectMaterializationTimeToTimeline(timePos)));

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

void PianoRollComponent::drawNoteDragCurvePreview(juce::Graphics& g)
{
    if (!showCorrectedF0_
        || interactionState_.noteDrag.previewStartFrame < 0
        || interactionState_.noteDrag.previewEndFrameExclusive <= interactionState_.noteDrag.previewStartFrame
        || interactionState_.noteDrag.previewF0.empty()) {
        return;
    }

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return;
    juce::Path previewPath;
    bool pathStarted = false;
    for (int frame = interactionState_.noteDrag.previewStartFrame;
         frame < interactionState_.noteDrag.previewEndFrameExclusive;
         ++frame) {
        const int relIndex = frame - interactionState_.noteDrag.previewStartFrame;
        if (relIndex < 0 || relIndex >= static_cast<int>(interactionState_.noteDrag.previewF0.size())) {
            continue;
        }

        const float f0 = interactionState_.noteDrag.previewF0[static_cast<std::size_t>(relIndex)];
        if (f0 <= 0.0f) {
            pathStarted = false;
            continue;
        }

        const float x = static_cast<float>(timeToX(projectMaterializationTimeToTimeline(f0tl.timeAtFrame(frame))));
        const float y = freqToY(f0);
        if (!pathStarted) {
            previewPath.startNewSubPath(x, y);
            pathStarted = true;
        } else {
            previewPath.lineTo(x, y);
        }
    }

    if (!previewPath.isEmpty()) {
        g.setColour(UIColors::correctedF0.withAlpha(0.8f));
        g.strokePath(previewPath,
                     juce::PathStrokeType(2.0f,
                                          juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded));
    }
}

void PianoRollComponent::drawLineAnchorPreview(juce::Graphics& g) {
    if (!interactionState_.drawing.isPlacingAnchors || currentTool_ != ToolId::LineAnchor || interactionState_.drawing.pendingAnchors.empty()) return;

    juce::Colour anchorColour = UIColors::correctedF0;

    for (size_t i = 0; i < interactionState_.drawing.pendingAnchors.size(); ++i) {
        const auto& anchor = interactionState_.drawing.pendingAnchors[i];
        float x = static_cast<float>(timeToX(projectMaterializationTimeToTimeline(anchor.time)));
        float y = freqToY(anchor.freq);

        g.setColour(anchorColour);
        g.fillEllipse(x - 2.0f, y - 2.0f, 4.0f, 4.0f);

        if (i > 0) {
            const auto& prev = interactionState_.drawing.pendingAnchors[i - 1];
            float prevX = static_cast<float>(timeToX(projectMaterializationTimeToTimeline(prev.time)));
            float prevY = freqToY(prev.freq);
            g.setColour(anchorColour.withAlpha(0.7f));
            g.drawLine(prevX, prevY, x, y, 2.0f);
        }
    }

    if (!interactionState_.drawing.pendingAnchors.empty()) {
        const auto& last = interactionState_.drawing.pendingAnchors.back();
        float lastX = static_cast<float>(timeToX(projectMaterializationTimeToTimeline(last.time)));
        float lastY = freqToY(last.freq);
        g.setColour(anchorColour.withAlpha(0.4f));
        g.drawLine(lastX, lastY, interactionState_.drawing.currentMousePos.x, interactionState_.drawing.currentMousePos.y, 1.5f);
    }
}

void PianoRollComponent::drawSelectionBox(juce::Graphics& g, ThemeId themeId) {
    if (!interactionState_.selection.hasSelectionArea) return;
    if (!toolHandler_ || !interactionState_.selection.isSelectingArea) return;

    double startTime = std::min(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
    double endTime = std::max(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
    float minMidi = std::min(interactionState_.selection.selectionStartMidi, interactionState_.selection.selectionEndMidi);
    float maxMidi = std::max(interactionState_.selection.selectionStartMidi, interactionState_.selection.selectionEndMidi);

    int x1 = timeToX(projectMaterializationTimeToTimeline(startTime));
    int x2 = timeToX(projectMaterializationTimeToTimeline(endTime));
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

void PianoRollComponent::paint(juce::Graphics& g) {
    auto ctx = buildRenderContext();
    const auto timelineViewportBounds = getTimelineViewportBounds();
    auto bounds = getLocalBounds().toFloat().reduced(12.0f);
    const auto themeId = UIColors::currentThemeId();

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
        g.reduceClipRegion(timelineViewportBounds.withTrimmedLeft(pianoKeyWidth_));

        renderer_->drawGridLines(g, ctx);
        renderer_->drawUnvoicedFrameBands(g, ctx);

        if (showWaveform_ && audioBuffer_ != nullptr) {
            renderer_->drawWaveform(g, ctx);
        }

        renderer_->drawLanes(g, ctx);

        // Draw reference notes layer (below user notes, above lanes)
        if (!cachedReferenceNotes_.empty()) {
            // Use live drag offset during active drag, cached offset otherwise
            const double refOffset = interactionState_.referenceDrag.isDragging
                ? interactionState_.referenceDrag.currentOffset
                : cachedReferenceTimeOffset_;
            if (std::abs(refOffset) < 1.0e-9) {
                renderer_->drawReferenceNotes(g, ctx, cachedReferenceNotes_);
            } else {
                std::vector<ReferenceNote> offsetNotes;
                offsetNotes.reserve(cachedReferenceNotes_.size());
                for (const auto& note : cachedReferenceNotes_) {
                    ReferenceNote shifted = note;
                    shifted.onset  += refOffset;
                    shifted.offset += refOffset;
                    offsetNotes.push_back(shifted);
                }
                renderer_->drawReferenceNotes(g, ctx, offsetNotes);
            }
        }

        const auto& notes = getDisplayedNotes();
        renderer_->drawNotes(g, ctx, notes);

        if (ctx.pitchSnapshot != nullptr && currentCurve_ != nullptr) {
            if (showOriginalF0_) {
                const auto& originalF0 = ctx.pitchSnapshot->getOriginalF0();
                if (!originalF0.empty()) {
                    renderer_->drawF0Curve(g, originalF0, UIColors::originalF0, 0.55f, true, ctx, currentCurve_);
                    drawSelectedOriginalF0Curve(g, originalF0);
                }
            }

            if (showCorrectedF0_) {
                const int totalFrames = static_cast<int>(ctx.pitchSnapshot->size());
                if (totalFrames > 0 && ctx.pitchSnapshot->hasAnyCorrection()) {
                    renderer_->updateCorrectedF0Cache(ctx.pitchSnapshot);
                    renderer_->drawF0Curve(g, renderer_->getCorrectedF0Cache(), UIColors::correctedF0, 1.0f, false, ctx, currentCurve_, nullptr);
                }

                drawNoteDragCurvePreview(g);
            }

            drawHandDrawPreview(g);
            drawLineAnchorPreview(g);
        }

        renderer_->drawChunkBoundaries(g, ctx);
    }

    renderer_->drawPianoKeys(g, ctx);

    drawSelectionBox(g, themeId);
}

void PianoRollComponent::setInferenceActive(bool active)
{
    inferenceActive_ = active;
    waveformBuildTickCounter_ = 0;
}

bool PianoRollComponent::applyNoteParameterToSelectedNotes(float retuneSpeed, float vibratoDepth, float vibratoRate) {
    auto notes = getEditedMaterializationNotesCopy();
    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return false;

    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;
    bool anySelected = false;

    for (auto& n : notes) {
        if (!n.selected) continue;
        anySelected = true;
        n.retuneSpeed = retuneSpeed;
        n.vibratoDepth = vibratoDepth;
        n.vibratoRate = vibratoRate;
        n.dirty = true;
        dirtyStartTime = std::min(dirtyStartTime, n.startTime);
        dirtyEndTime = std::max(dirtyEndTime, n.endTime);
    }
    if (!anySelected) return false;

    if (dirtyEndTime > dirtyStartTime && currentCurve_) {
        const auto affectedRange = f0tl.rangeForTimes(dirtyStartTime, dirtyEndTime);
        if (!affectedRange.isEmpty()) {
            auto clonedCurve = currentCurve_->clone();
            clonedCurve->applyCorrectionToRange(
                notes, affectedRange.startFrame, affectedRange.endFrameExclusive,
                retuneSpeed, vibratoDepth, vibratoRate, 44100.0);
            auto snap = clonedCurve->getSnapshot();
            if (snap) {
                commitEditedMaterializationNotesAndSegments(notes, snap->getCorrectedSegments());
            } else {
                commitEditedMaterializationNotes(notes);
            }
            listeners_.call([affectedRange](Listener& l) { l.pitchCurveEdited(affectedRange.startFrame, affectedRange.endFrameExclusive - 1); });
        } else {
            commitEditedMaterializationNotes(notes);
        }
    } else {
        commitEditedMaterializationNotes(notes);
    }
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
    return true;
}

bool PianoRollComponent::applyParameterToFrameRange(float retuneSpeed, float vibratoDepth, float vibratoRate, int startFrame, int endFrameExclusive) {
    if (!currentCurve_ || endFrameExclusive <= startFrame) return false;
    if (!currentCurve_->hasCorrectionInRange(startFrame, endFrameExclusive)) return false;

    enqueueNoteBasedCorrectionAsync(getEditedMaterializationNotesCopy(),
                                    startFrame, endFrameExclusive,
                                    retuneSpeed, vibratoDepth, vibratoRate);

    const int notifyEndFrame = std::max(startFrame, endFrameExclusive - 1);
    listeners_.call([startFrame, notifyEndFrame](Listener& l) { l.pitchCurveEdited(startFrame, notifyEndFrame); });
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
    return true;
}

bool PianoRollComponent::getFrameRangeForTimeSpan(double startTime, double endTime, int& startFrame, int& endFrameExclusive) const
{
    startFrame = 0;
    endFrameExclusive = 0;
    if (currentCurve_ == nullptr || endTime <= startTime) return false;
    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return false;
    const auto range = f0tl.nonEmptyRangeForTimes(startTime, endTime);
    startFrame = range.startFrame;
    endFrameExclusive = range.endFrameExclusive;
    return endFrameExclusive > startFrame;
}

bool PianoRollComponent::getSelectedNotesFrameRange(int& startFrame, int& endFrameExclusive) const
{
    const auto notes = getEditedMaterializationNotesCopy();
    double minStart = std::numeric_limits<double>::max();
    double maxEnd = -1.0;
    for (const auto& note : notes)
        if (note.selected) { minStart = std::min(minStart, note.startTime); maxEnd = std::max(maxEnd, note.endTime); }
    return maxEnd > minStart && getFrameRangeForTimeSpan(minStart, maxEnd, startFrame, endFrameExclusive);
}

bool PianoRollComponent::getSelectionAreaFrameRange(int& startFrame, int& endFrameExclusive) const
{
    if (!interactionState_.selection.hasSelectionArea) { startFrame = 0; endFrameExclusive = 0; return false; }
    const double s = std::min(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
    const double e = std::max(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
    return getFrameRangeForTimeSpan(s, e, startFrame, endFrameExclusive);
}

bool PianoRollComponent::getF0SelectionFrameRange(int& startFrame, int& endFrameExclusive) const
{
    startFrame = 0;
    endFrameExclusive = 0;

    if (currentCurve_ == nullptr || !interactionState_.selection.hasF0Selection) {
        return false;
    }

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return false;

    const auto range = f0tl.rangeForFrames(interactionState_.selection.selectedF0StartFrame,
                                           interactionState_.selection.selectedF0EndFrameExclusive);
    startFrame = range.startFrame;
    endFrameExclusive = range.endFrameExclusive;
    return endFrameExclusive > startFrame;
}

bool PianoRollComponent::applyRetuneSpeedToSelection(float speed) {
    if (isAutoTuneProcessing()) {
        return false;
    }

    speed = juce::jlimit(0.0f, 1.0f, speed);
    pendingUndoDescription_ = TRANS("修改调速");
    if (!currentCurve_) return false;

    int selectedNotesStartFrame = 0;
    int selectedNotesEndFrameExclusive = 0;
    const bool hasSelectedNotesRange = getSelectedNotesFrameRange(selectedNotesStartFrame,
                                                                  selectedNotesEndFrameExclusive);

    int frameSelectionStartFrame = 0;
    int frameSelectionEndFrameExclusive = 0;
    const bool hasF0SelectionRange = getF0SelectionFrameRange(frameSelectionStartFrame,
                                                              frameSelectionEndFrameExclusive);
    const bool hasSelectionAreaRange = hasF0SelectionRange
        || getSelectionAreaFrameRange(frameSelectionStartFrame, frameSelectionEndFrameExclusive);

    AudioEditingScheme::ParameterTargetContext context;
    context.hasSelectedNotes = hasSelectedNotesRange;
    context.hasSelectedLineAnchorSegments = !interactionState_.selectedLineAnchorSegmentIds.empty();
    context.hasFrameSelection = hasSelectionAreaRange;
    context.allowWholeClipFallback = false;

    switch (AudioEditingScheme::resolveParameterTarget(
        audioEditingScheme_,
        AudioEditingScheme::ParameterKind::RetuneSpeed,
        context)) {
        case AudioEditingScheme::ParameterTarget::SelectedLineAnchorSegments:
            return applyRetuneSpeedToSelectedLineAnchorSegments(speed);
        case AudioEditingScheme::ParameterTarget::SelectedNotes:
            return applyNoteParameterToSelectedNotes(speed, currentVibratoDepth_, currentVibratoRate_);
        case AudioEditingScheme::ParameterTarget::FrameSelection:
            if (hasHandDrawCorrectionInRange(frameSelectionStartFrame, frameSelectionEndFrameExclusive)) {
                return true;
            }
            return applyParameterToFrameRange(speed,
                                              currentVibratoDepth_,
                                              currentVibratoRate_,
                                              frameSelectionStartFrame,
                                              frameSelectionEndFrameExclusive);
        default:
            break;
    }

    return false;
}

bool PianoRollComponent::applyRetuneSpeedToSelectedLineAnchorSegments(float speed) {
    if (isAutoTuneProcessing()) {
        return false;
    }

    if (currentCurve_ == nullptr || interactionState_.selectedLineAnchorSegmentIds.empty()) {
        return false;
    }

    auto snapshot = currentCurve_->getSnapshot();
    const auto& allSegments = snapshot->getCorrectedSegments();

    int modifiedCount = 0;
    int affectedStartFrame = INT_MAX;
    int affectedEndFrame = -1;

    for (int idx : interactionState_.selectedLineAnchorSegmentIds) {
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

    auto updatedSegments = copyCorrectedSegments(currentCurve_);

    for (int idx : interactionState_.selectedLineAnchorSegmentIds) {
        if (idx < 0 || idx >= static_cast<int>(allSegments.size())) continue;
        const auto& seg = allSegments[idx];
        if (seg.source != CorrectedSegment::Source::LineAnchor) continue;

        updatedSegments[static_cast<size_t>(idx)].retuneSpeed = juce::jlimit(0.0f, 1.0f, speed);
    }

    commitEditedMaterializationCorrectedSegments(updatedSegments);

    if (affectedStartFrame <= affectedEndFrame) {
        listeners_.call([affectedStartFrame, affectedEndFrame](Listener& l) {
            l.pitchCurveEdited(affectedStartFrame, affectedEndFrame);
        });
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
    }

    return true;
}

bool PianoRollComponent::applyVibratoDepthToSelection(float depth) {
    pendingUndoDescription_ = TRANS("修改颤音深度");
    return applyVibratoParameterToSelection(VibratoParam::Depth, depth);
}

bool PianoRollComponent::applyVibratoRateToSelection(float rate) {
    pendingUndoDescription_ = TRANS("修改颤音速率");
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

    int selectedNotesStartFrame = 0;
    int selectedNotesEndFrameExclusive = 0;
    const bool hasSelectedNotesRange = getSelectedNotesFrameRange(selectedNotesStartFrame,
                                                                  selectedNotesEndFrameExclusive);

    int frameSelectionStartFrame = 0;
    int frameSelectionEndFrameExclusive = 0;
    const bool hasF0SelectionRange = getF0SelectionFrameRange(frameSelectionStartFrame,
                                                              frameSelectionEndFrameExclusive);
    const bool hasSelectionAreaRange = hasF0SelectionRange
        || getSelectionAreaFrameRange(frameSelectionStartFrame, frameSelectionEndFrameExclusive);

    AudioEditingScheme::ParameterTargetContext context;
    context.hasSelectedNotes = hasSelectedNotesRange;
    context.hasSelectedLineAnchorSegments = !interactionState_.selectedLineAnchorSegmentIds.empty();
    context.hasFrameSelection = hasSelectionAreaRange;
    context.allowWholeClipFallback = false;

    switch (AudioEditingScheme::resolveParameterTarget(
        audioEditingScheme_,
        param == VibratoParam::Depth
            ? AudioEditingScheme::ParameterKind::VibratoDepth
            : AudioEditingScheme::ParameterKind::VibratoRate,
        context)) {
        case AudioEditingScheme::ParameterTarget::SelectedNotes:
        {
            float effectiveDepth = (param == VibratoParam::Depth) ? value : currentVibratoDepth_;
            float effectiveRate = (param == VibratoParam::Rate) ? value : currentVibratoRate_;
            return applyNoteParameterToSelectedNotes(currentRetuneSpeed_, effectiveDepth, effectiveRate);
        }
        case AudioEditingScheme::ParameterTarget::FrameSelection:
            return applyParameterToFrameRange(
                currentRetuneSpeed_,
                (param == VibratoParam::Depth) ? value : currentVibratoDepth_,
                (param == VibratoParam::Rate) ? value : currentVibratoRate_,
                frameSelectionStartFrame, frameSelectionEndFrameExclusive);
        default:
            return false;
    }
}

bool PianoRollComponent::getSingleSelectedNoteParameters(float& retuneSpeedPercent, float& vibratoDepth, float& vibratoRate) const
{
    const auto notes = getEditedMaterializationNotesCopy();
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
    if (currentCurve_ == nullptr || interactionState_.selectedLineAnchorSegmentIds.empty()) {
        return false;
    }

    int selectedNotesStartFrame = 0;
    int selectedNotesEndFrameExclusive = 0;
    const bool hasSelectedNotesRange = getSelectedNotesFrameRange(selectedNotesStartFrame,
                                                                  selectedNotesEndFrameExclusive);

    int frameSelectionStartFrame = 0;
    int frameSelectionEndFrameExclusive = 0;
    const bool hasF0SelectionRange = getF0SelectionFrameRange(frameSelectionStartFrame,
                                                              frameSelectionEndFrameExclusive);
    const bool hasSelectionAreaRange = hasF0SelectionRange
        || getSelectionAreaFrameRange(frameSelectionStartFrame, frameSelectionEndFrameExclusive);

    AudioEditingScheme::ParameterTargetContext context;
    context.hasSelectedNotes = hasSelectedNotesRange;
    context.hasSelectedLineAnchorSegments = !interactionState_.selectedLineAnchorSegmentIds.empty();
    context.hasFrameSelection = hasSelectionAreaRange;
    context.allowWholeClipFallback = false;

    if (AudioEditingScheme::resolveParameterTarget(
            audioEditingScheme_,
            AudioEditingScheme::ParameterKind::RetuneSpeed,
            context) != AudioEditingScheme::ParameterTarget::SelectedLineAnchorSegments) {
        return false;
    }

    auto snapshot = currentCurve_->getSnapshot();
    const auto& allSegments = snapshot->getCorrectedSegments();

    if (interactionState_.selectedLineAnchorSegmentIds.size() != 1) {
        return false;
    }

    int idx = interactionState_.selectedLineAnchorSegmentIds[0];
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

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return -1;
    const float tolerancePixels = 15.0f;
    const double clickTime = projectTimelineTimeToMaterialization(xToTime(x));

    int bestIdx = -1;
    float bestDist = tolerancePixels;

    for (int i = 0; i < static_cast<int>(allSegments.size()); ++i) {
        const auto& seg = allSegments[i];
        if (seg.source != CorrectedSegment::Source::LineAnchor) continue;
        if (seg.f0Data.empty()) continue;

        const double startTime = f0tl.timeAtFrame(seg.startFrame);
        const double endTime = f0tl.timeAtFrame(seg.endFrame);

        const int startX = timeToX(projectMaterializationTimeToTimeline(startTime));
        const int endX = timeToX(projectMaterializationTimeToTimeline(endTime));

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
    interactionState_.selectedLineAnchorSegmentIds.clear();
    if (idx >= 0) {
        interactionState_.selectedLineAnchorSegmentIds.push_back(idx);
    }
}

void PianoRollComponent::toggleLineAnchorSegmentSelection(int idx)
{
    auto it = std::find(interactionState_.selectedLineAnchorSegmentIds.begin(), interactionState_.selectedLineAnchorSegmentIds.end(), idx);
    if (it != interactionState_.selectedLineAnchorSegmentIds.end()) {
        interactionState_.selectedLineAnchorSegmentIds.erase(it);
    } else {
        interactionState_.selectedLineAnchorSegmentIds.push_back(idx);
    }
}

void PianoRollComponent::clearLineAnchorSegmentSelection()
{
    interactionState_.selectedLineAnchorSegmentIds.clear();
}

void PianoRollComponent::setNoteSplit(float value) {
    // Note Split 控制音高分段阈值（cents）
    segmentationPolicy_.transitionThresholdCents = juce::jlimit(
        OpenTune::PitchControlConfig::kMinNoteSplitCents,
        OpenTune::PitchControlConfig::kMaxNoteSplitCents,
        value);

    // Note Split 仅更新分段策略参数，不触发 AUTO 重新生成。
    // AUTO 操作由用户主动触发，使用当前策略执行分段。
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
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

    playheadOverlay_.setBounds(getLocalBounds());

}

void PianoRollComponent::applyEditedMaterializationCurve(std::shared_ptr<PitchCurve> curve)
{
    currentCurve_ = std::move(curve);
    if (renderer_) {
        renderer_->clearCorrectedF0Cache();
    }

    interactionState_.selection.hasSelectionArea = false;
    interactionState_.selection.selectionStartTime = 0.0;
    interactionState_.selection.selectionEndTime = 0.0;
    interactionState_.selection.selectionStartMidi = 0.0f;
    interactionState_.selection.selectionEndMidi = 0.0f;
}

void PianoRollComponent::applyEditedMaterializationAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                                       int sampleRate)
{
    audioBuffer_ = std::move(buffer);
    audioBufferSampleRate_ = sampleRate > 0 ? static_cast<double>(sampleRate)
                                            : static_cast<double>(PianoRollComponent::kAudioSampleRate);

    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);

    waveformMipmap_.setAudioSource(audioBuffer_);
    renderer_->setWaveformMipmap(&waveformMipmap_);
}

void PianoRollComponent::setMaterializationProjection(const MaterializationTimelineProjection& projection)
{
    const bool changed = std::abs(materializationProjection_.timelineStartSeconds - projection.timelineStartSeconds) > 1.0e-9
        || std::abs(materializationProjection_.timelineDurationSeconds - projection.timelineDurationSeconds) > 1.0e-9
        || std::abs(materializationProjection_.materializationDurationSeconds - projection.materializationDurationSeconds) > 1.0e-9;

    if (!changed) {
        return;
    }

    materializationProjection_ = projection;
    playheadOverlay_.setTimelineStartSeconds(projection.timelineStartSeconds);
    userScrollHold_ = false;
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                     PianoRollVisualInvalidationPriority::Interactive);
}

void PianoRollComponent::setEditedMaterialization(uint64_t materializationId,
                                           std::shared_ptr<PitchCurve> curve,
                                           std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                           int sampleRate)
{
    const double normalizedSampleRate = sampleRate > 0 ? static_cast<double>(sampleRate)
                                                        : static_cast<double>(PianoRollComponent::kAudioSampleRate);
    const bool materializationChanged = editedMaterializationId_ != materializationId;
    const bool curveChanged = currentCurve_ != curve;
    const bool bufferChanged = audioBuffer_ != buffer || audioBufferSampleRate_ != normalizedSampleRate;

    if (!materializationChanged && !curveChanged && !bufferChanged) {
        return;
    }

    const bool hasAudio = (buffer != nullptr);

    if (materializationChanged) {
        editedMaterializationId_ = materializationId;
        clearNoteDraft();
        refreshEditedMaterializationNotes();
        autoTuneInFlight_.store(false, std::memory_order_release);
        pendingUndoDescription_ = {};
        beforeUndoNotes_.clear();
        beforeUndoSegments_.clear();
        undoSnapshotCaptured_ = false;
    }

    if (materializationChanged || curveChanged) {
        editedMaterializationEpoch_.fetch_add(1, std::memory_order_release);
        applyEditedMaterializationCurve(std::move(curve));
    }

    if (materializationChanged || bufferChanged) {
        applyEditedMaterializationAudioBuffer(std::move(buffer), sampleRate);
    }

    if (hasAudio && (materializationChanged || bufferChanged)) {
        fitToScreen();
    }

    userScrollHold_ = false;
    updateScrollBars();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::invalidateVisual(const PianoRollVisualInvalidationRequest& request)
{
    if (request.reasonsMask == 0) {
        return;
    }

    pendingVisualInvalidation_.merge(request);

    if (request.priority != PianoRollVisualInvalidationPriority::Interactive || !isShowing()) {
        return;
    }

    constexpr double minIntervalMs = 1000.0 / 60.0;
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if ((nowMs - lastVisualFlushMs_) >= minIntervalMs) {
        flushPendingVisualInvalidation();
    }
}

void PianoRollComponent::invalidateVisual(uint32_t reasonsMask,
                                          PianoRollVisualInvalidationPriority priority)
{
    PianoRollVisualInvalidationRequest request;
    request.reasonsMask = reasonsMask;
    request.fullRepaint = true;
    request.priority = priority;
    invalidateVisual(request);
}

void PianoRollComponent::invalidateVisual(uint32_t reasonsMask,
                                          const juce::Rectangle<int>& dirtyArea,
                                          PianoRollVisualInvalidationPriority priority)
{
    PianoRollVisualInvalidationRequest request;
    request.reasonsMask = reasonsMask;
    request.fullRepaint = dirtyArea.isEmpty();
    request.hasDirtyArea = !dirtyArea.isEmpty();
    request.dirtyArea = dirtyArea;
    request.priority = priority;
    invalidateVisual(request);
}

void PianoRollComponent::flushPendingVisualInvalidation()
{
    const auto decision = makeVisualFlushDecision(pendingVisualInvalidation_, getLocalBounds());
    pendingVisualInvalidation_.clear();
    if (!decision.shouldRepaint) {
        return;
    }

    lastVisualFlushMs_ = juce::Time::getMillisecondCounterHiRes();

    const auto priority = toFrameSchedulerPriority(decision.priority);
    auto requestInvalidate = [&](auto&&... args) {
        FrameScheduler::instance().requestInvalidate(*this, std::forward<decltype(args)>(args)...);
    };

    if (decision.fullRepaint || !decision.hasDirtyArea) {
        requestInvalidate(priority);
        return;
    }

    requestInvalidate(decision.dirtyArea, priority);
}

void PianoRollComponent::setScrollOffset(int offset) {
    const int newOffset = juce::jmax(0, offset);
    if (newOffset == scrollOffset_) return;
    
    const int oldOffset = scrollOffset_;
    scrollOffset_ = newOffset;
    timeConverter_.setScrollOffset(scrollOffset_);
    playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));
    horizontalScrollBar_.setCurrentRangeStart(scrollOffset_, juce::dontSendNotification);
    const auto timelineViewportBounds = getTimelineViewportBounds();

    const int scrollDelta = scrollOffset_ - oldOffset;
    const int contentWidth = timelineViewportBounds.getWidth() - pianoKeyWidth_;
    if (contentWidth > 0 && std::abs(scrollDelta) < contentWidth) {
        const juce::Rectangle<int> dirtyArea(pianoKeyWidth_, 0, contentWidth, timelineViewportBounds.getHeight());
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                         dirtyArea,
                         PianoRollVisualInvalidationPriority::Interactive);
    } else {
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                         PianoRollVisualInvalidationPriority::Interactive);
    }
}

double PianoRollComponent::readPlayheadTime() const
{
    if (auto source = positionSource_.lock()) {
        return source->load(std::memory_order_relaxed);
    }
    return 0.0;
}

double PianoRollComponent::readProjectedPlayheadTime() const
{
    const double raw = readPlayheadTime();
    if (!materializationProjection_.isValid())
        return raw;
    return materializationProjection_.clampTimelineTime(raw);
}

juce::Rectangle<int> PianoRollComponent::getTimelineViewportBounds() const
{
    constexpr int panelInset = 12;
    const int viewportWidth = juce::jmax(0, getWidth() - panelInset - verticalScrollBar_.getWidth());
    const int viewportHeight = juce::jmax(0, getHeight() - panelInset - horizontalScrollBar_.getHeight());
    return { 0, 0, viewportWidth, viewportHeight };
}

void PianoRollComponent::onHeartbeatTick()
{
    if (!isShowing()) {
        return;
    }

    consumeCompletedCorrectionResults();

    // Refresh cached reference notes on revision change (avoids per-frame lock+copy in paint)
    if (processor_ != nullptr && editedMaterializationId_ != 0) {
        const auto rev = processor_->getMaterializationStore()->getReferenceNotesRevision(editedMaterializationId_);
        if (rev != cachedReferenceNotesRevision_) {
            cachedReferenceNotes_ = processor_->getMaterializationStore()->getReferenceNotes(editedMaterializationId_);
            cachedReferenceTimeOffset_ = processor_->getMaterializationStore()->getReferenceTimeOffset(editedMaterializationId_);
            cachedReferenceNotesRevision_ = rev;
            invalidateVisual(static_cast<uint32_t>(PianoRollVisualInvalidationReason::Content));
        }
    }

    if (!waveformMipmap_.isComplete() && showWaveform_) {
        if (inferenceActive_) {
            waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 8;
            if (waveformBuildTickCounter_ == 0 && waveformMipmap_.buildIncremental(1.0)) {
                invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
            }
        } else {
            waveformBuildTickCounter_ = 0;
            if (waveformMipmap_.buildIncremental(5.0)) {
                invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
            }
        }
    }

    flushPendingVisualInvalidation();
}

void PianoRollComponent::onScrollVBlankCallback(double timestampSec)
{
    juce::ignoreUnused(timestampSec);

    if (!isShowing()) {
        pendingSeekTime_ = -1.0;
        return;
    }

    const double hostTime = readProjectedPlayheadTime();

    if (!isPlaying_.load(std::memory_order_relaxed)) {
        // Paused: still mirror the host transport position so DAW timeline seeks
        // (and standalone setPosition writes) appear in the plugin window without
        // requiring playback. The playing-only auto-scroll/centering logic below
        // is intentionally skipped — when paused, the user controls the view.
        playheadOverlay_.setPlayheadSeconds(hostTime);
        pendingSeekTime_ = -1.0;
        return;
    }

    // 如果有 pending seek，检查 host 是否已确认（position 接近 pending 值）
    double playheadTime;
    if (pendingSeekTime_ >= 0.0) {
        if (std::abs(hostTime - pendingSeekTime_) < 0.05) {
            // Host 已确认 seek
            pendingSeekTime_ = -1.0;
            playheadTime = hostTime;
        } else {
            // Host 尚未确认，使用 pending 值（模拟播放头从 seek 点前进）
            playheadTime = pendingSeekTime_;
        }
    } else {
        playheadTime = hostTime;
    }

    playheadOverlay_.setPlayheadSeconds(playheadTime);

    const auto timelineViewportBounds = getTimelineViewportBounds();
    const int visibleWidth = timelineViewportBounds.getWidth() - pianoKeyWidth_;
    if (visibleWidth <= 0) return;

    if (scrollMode_ == ScrollMode::Continuous) {
        if (userScrollHold_) return;

        const float playheadAbsX = static_cast<float>(getPlayheadAbsolutePixelX(playheadTime));
        const float viewCenter = visibleWidth / 2.0f;
        const int centeredScroll = std::max(0, static_cast<int>(std::round(playheadAbsX - viewCenter)));

        // Smooth seek: decay offset toward 0
        if (std::abs(scrollSeekOffset_) > 0.5f)
            scrollSeekOffset_ *= 0.95f;
        else
            scrollSeekOffset_ = 0.0f;

        const int targetScroll = std::max(0, centeredScroll + static_cast<int>(std::round(scrollSeekOffset_)));
        if (targetScroll != scrollOffset_)
            setScrollOffset(targetScroll);
    } else if (scrollMode_ == ScrollMode::Page) {
        const int playheadVisualX = timeToX(playheadTime);

        if (playheadVisualX >= timelineViewportBounds.getRight()) {
            setScrollOffset(scrollOffset_ + visibleWidth);
        } else if (playheadVisualX < pianoKeyWidth_) {
            const int absX = static_cast<int>(std::llround(getPlayheadAbsolutePixelX(playheadTime)));

            int pageIndex = absX / visibleWidth;
            int newScroll = pageIndex * visibleWidth;

            setScrollOffset(newScroll);
        }
    }
}

void PianoRollComponent::setZoomLevel(double zoom) {
    zoomLevel_ = juce::jlimit(0.02, 10.0, zoom);
    timeConverter_.setZoom(zoomLevel_);
    playheadOverlay_.setZoomLevel(zoomLevel_);
    updateScrollBars();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                     PianoRollVisualInvalidationPriority::Interactive);
}

void PianoRollComponent::setCurrentTool(ToolId tool) {
    const bool toolChanged = currentTool_ != tool;
    bool clearedAnchorPreview = false;
    if (interactionState_.drawing.isPlacingAnchors && tool != ToolId::LineAnchor) {
        interactionState_.drawing.isPlacingAnchors = false;
        interactionState_.drawing.pendingAnchors.clear();
        clearedAnchorPreview = true;
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

    if (toolChanged || clearedAnchorPreview) {
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Interaction),
                         getLocalBounds(),
                         PianoRollVisualInvalidationPriority::Interactive);
    }
}

void PianoRollComponent::setShowWaveform(bool shouldShow) {
    showWaveform_ = shouldShow;
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::setShowLanes(bool shouldShow) {
    showLanes_ = shouldShow;
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::setNoteNameMode(NoteNameMode noteNameMode)
{
    if (noteNameMode_ == noteNameMode) {
        return;
    }

    noteNameMode_ = noteNameMode;
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::setShowChunkBoundaries(bool shouldShow)
{
    if (showChunkBoundaries_ == shouldShow) {
        return;
    }

    showChunkBoundaries_ = shouldShow;
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::setShowUnvoicedFrames(bool shouldShow)
{
    if (showUnvoicedFrames_ == shouldShow) {
        return;
    }

    showUnvoicedFrames_ = shouldShow;
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::setBpm(double bpm) {
    bpm_ = juce::jlimit(60.0, 240.0, bpm);
    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport));
}

void PianoRollComponent::setTimeSignature(int numerator, int denominator) {
    if (numerator <= 0 || denominator <= 0) {
        return;
    }

    timeSigNum_ = numerator;
    timeSigDenom_ = denominator;
    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport));
}

void PianoRollComponent::setTimeUnit(TimeUnit unit) {
    timeUnit_ = unit;
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport));
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

    // Piano key audition: click in piano key area triggers note preview
    if (e.x < pianoKeyWidth_) {
        int midiNote = static_cast<int>(std::ceil(yToMidi(static_cast<float>(e.y))));
        midiNote = juce::jlimit(0, 127, midiNote);
        pressedPianoKey_ = midiNote;
        if (pianoKeyAudition_ != nullptr)
            pianoKeyAudition_->noteOn(midiNote);
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                         PianoRollVisualInvalidationPriority::Interactive);
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
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                         PianoRollVisualInvalidationPriority::Interactive);
        return;
    }

    // Piano key glissando: dragging across keys changes the note
    if (pressedPianoKey_ >= 0) {
        int midiNote = static_cast<int>(std::ceil(yToMidi(static_cast<float>(e.y))));
        midiNote = juce::jlimit(0, 127, midiNote);
        if (midiNote != pressedPianoKey_) {
            if (pianoKeyAudition_ != nullptr) {
                pianoKeyAudition_->noteOff(pressedPianoKey_);
                pianoKeyAudition_->noteOn(midiNote);
            }
            pressedPianoKey_ = midiNote;
            invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                             PianoRollVisualInvalidationPriority::Interactive);
        }
        return;
    }

    toolHandler_->mouseDrag(e);
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Interaction),
                     getLocalBounds(),
                     PianoRollVisualInvalidationPriority::Interactive);
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

    if (pressedPianoKey_ >= 0) {
        if (pianoKeyAudition_ != nullptr)
            pianoKeyAudition_->noteOff(pressedPianoKey_);
        pressedPianoKey_ = -1;
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                         PianoRollVisualInvalidationPriority::Interactive);
        return;
    }

    toolHandler_->mouseUp(e);
    grabKeyboardFocus();
}

void PianoRollComponent::handleVerticalZoomWheel(const juce::MouseEvent& e, float deltaY) {
    const auto& settings = zoomSensitivity_;
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
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                     PianoRollVisualInvalidationPriority::Interactive);
}

void PianoRollComponent::handleHorizontalScrollWheel(float deltaX, float deltaY) {
    const auto& settings = zoomSensitivity_;
    float scrollDelta = (deltaX != 0 ? deltaX : deltaY);
    int pixelDelta = static_cast<int>(scrollDelta * settings.scrollSpeed);
    setScrollOffset(scrollOffset_ - pixelDelta);
    userScrollHold_ = true;
}

void PianoRollComponent::handleVerticalScrollWheel(float deltaY) {
    const auto& settings = zoomSensitivity_;
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
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                     PianoRollVisualInvalidationPriority::Interactive);
}

void PianoRollComponent::handleHorizontalZoomWheel(const juce::MouseEvent& e, float deltaY) {
    const auto& settings = zoomSensitivity_;
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
    float deltaX = wheel.deltaX;
    float deltaY = wheel.deltaY;

    // macOS swaps scroll axes when Shift is held at the OS level.
    // Undo this transformation so our modifier-based dispatch works correctly.
#if JUCE_MAC
    if (e.mods.isShiftDown() && deltaY == 0.0f && deltaX != 0.0f) {
        deltaY = deltaX;
        deltaX = 0.0f;
    }
#endif

    if (deltaY == 0.0f && deltaX == 0.0f) return;

    if (e.mods.isShiftDown()) {
        handleVerticalZoomWheel(e, deltaY);
    } else if (e.mods.isCtrlDown()) {
        handleHorizontalZoomWheel(e, deltaY);
    } else if (e.mods.isAltDown()) {
        handleHorizontalScrollWheel(deltaX, deltaY);
    } else {
        handleVerticalScrollWheel(deltaY);
    }
}

bool PianoRollComponent::keyPressed(const juce::KeyPress& key) {
    if (isAutoTuneProcessing()) {
        return false;
    }
    if (key == juce::KeyPress('z', juce::ModifierKeys::ctrlModifier, 0)) {
        listeners_.call([](Listener& l) { l.undoRequested(); });
        return true;
    }
    if (key == juce::KeyPress('y', juce::ModifierKeys::ctrlModifier, 0)) {
        listeners_.call([](Listener& l) { l.redoRequested(); });
        return true;
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
    const auto timelineViewportBounds = getTimelineViewportBounds();
    PianoRollRenderer::RenderContext ctx;
    ctx.width = timelineViewportBounds.getRight();
    ctx.height = timelineViewportBounds.getBottom();
    ctx.pianoKeyWidth = pianoKeyWidth_;
    ctx.pressedPianoKey = pressedPianoKey_;
    ctx.rulerHeight = rulerHeight_;

    ctx.pixelsPerSecond = getTimelinePixelsPerSecond();
    ctx.pixelsPerSemitone = pixelsPerSemitone_;
    ctx.minMidi = minMidi_;
    ctx.maxMidi = maxMidi_;
    ctx.bpm = bpm_;
    ctx.materializationProjection = materializationProjection_;
    ctx.scaleRootNote = scaleRootNote_;
    ctx.scaleType = scaleType_;
    ctx.noteNameMode = noteNameMode_;
    ctx.showLanes = showLanes_;
    ctx.showChunkBoundaries = showChunkBoundaries_;
    ctx.showUnvoicedFrames = showUnvoicedFrames_;
    ctx.hasUserAudio = (audioBuffer_ != nullptr);
    ctx.pitchSnapshot = currentCurve_ != nullptr ? currentCurve_->getSnapshot() : nullptr;
    ctx.f0Timeline = currentF0Timeline();
    ctx.timeUnit = (timeUnit_ == TimeUnit::Bars)
        ? PianoRollRenderer::RenderContext::TimeUnit::Bars
        : PianoRollRenderer::RenderContext::TimeUnit::Seconds;

    if (ctx.showChunkBoundaries && processor_ != nullptr && editedMaterializationId_ != 0) {
        processor_->getMaterializationChunkBoundariesById(editedMaterializationId_, ctx.chunkBoundaries);
    }

    ctx.midiToY = [this](float midi) { return midiToY(midi); };
    ctx.freqToY = [this](float freq) { return freqToY(freq); };
    ctx.freqToMidi = [this](float freq) { return freqToMidi(freq); };
    ctx.xToTime = [this](int x) { return xToTime(x); };
    ctx.timeToX = [this](double seconds) { return timeToX(seconds); };

    ctx.hasF0Selection = interactionState_.selection.hasF0Selection;
    ctx.f0SelectionStartFrame = interactionState_.selection.selectedF0StartFrame;
    ctx.f0SelectionEndFrameExclusive = interactionState_.selection.selectedF0EndFrameExclusive;

    return ctx;
}

void PianoRollComponent::setScale(int rootNote, int scaleType)
{
    scaleRootNote_ = juce::jlimit(0, 11, rootNote);
    scaleType_ = juce::jlimit(1, 8, scaleType);
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::fitToScreen() {
    // 如果用户已手动调整过缩放，不自动覆盖
    if (userHasManuallyZoomed_) {
        return;
    }

    // 1. Vertical Fit: Show C1 to C8 (minMidi_ to maxMidi_)
    // Total range: maxMidi_ - minMidi_
    // Available height: getHeight()
    const auto timelineViewportBounds = getTimelineViewportBounds();
    float range = maxMidi_ - minMidi_;
    if (range > 0 && timelineViewportBounds.getHeight() > 0) {
        pixelsPerSemitone_ = static_cast<float>(timelineViewportBounds.getHeight()) / range;
        
        // Reset scroll to show top
        verticalScrollOffset_ = 0; 
    }

    // 2. Horizontal Fit:
    // If has audio: fit audio length
    // If no audio: fit 16 seconds
    double projectedStartSeconds = materializationProjection_.timelineStartSeconds;
    double duration = 16.0;
    const bool hasProjectedClipTimeline = materializationProjection_.isValid();
    if (hasProjectedClipTimeline) {
        duration = materializationProjection_.timelineDurationSeconds;
    }
    if (!hasProjectedClipTimeline && audioBuffer_ && audioBufferSampleRate_ > 0.0) {
        duration = static_cast<double>(audioBuffer_->getNumSamples()) / audioBufferSampleRate_;
    }
    
    // Available width: getWidth() - pianoKeyWidth_
    int viewWidth = timelineViewportBounds.getWidth() - pianoKeyWidth_;
    if (viewWidth > 0 && duration > 0) {
        // pixelsPerSecond * duration = viewWidth
        // pixelsPerSecond = viewWidth / duration
        double pixelsPerSecond = static_cast<double>(viewWidth) / duration;
        
        // zoomLevel = pixelsPerSecond / base scale
        setZoomLevel(pixelsPerSecond / TimeConverter::kBasePixelsPerSecond);
    }

    if (hasProjectedClipTimeline) {
        setScrollOffset(0);
    } else if (audioBuffer_ && audioBufferSampleRate_ > 0.0) {
        int newScroll = (int) std::llround(projectedStartSeconds * getTimelinePixelsPerSecond());
        setScrollOffset(newScroll);
    } else {
        setScrollOffset(0);
    }
    
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                     PianoRollVisualInvalidationPriority::Interactive);
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

double PianoRollComponent::toVisibleTimelineSeconds(double absoluteSeconds) const {
    return absoluteSeconds - materializationProjection_.timelineStartSeconds;
}

double PianoRollComponent::toAbsoluteTimelineSeconds(double visibleSeconds) const {
    return visibleSeconds + materializationProjection_.timelineStartSeconds;
}

double PianoRollComponent::projectTimelineTimeToMaterialization(double timelineSeconds) const {
    return materializationProjection_.isValid()
        ? materializationProjection_.projectTimelineTimeToMaterialization(timelineSeconds)
        : timelineSeconds;
}

double PianoRollComponent::projectMaterializationTimeToTimeline(double contentSeconds) const {
    return materializationProjection_.isValid()
        ? materializationProjection_.projectMaterializationTimeToTimeline(contentSeconds)
        : contentSeconds;
}

double PianoRollComponent::getTimelinePixelsPerSecond() const {
    return timeConverter_.getPixelsPerSecond();
}

double PianoRollComponent::getPlayheadAbsolutePixelX(double playheadTimeSeconds) const {
    return juce::jmax(0.0, toVisibleTimelineSeconds(playheadTimeSeconds)) * getTimelinePixelsPerSecond();
}

int PianoRollComponent::timeToX(double seconds) const {
    return timeConverter_.timeToPixel(toVisibleTimelineSeconds(seconds)) + pianoKeyWidth_;
}

double PianoRollComponent::xToTime(int x) const {
    return toAbsoluteTimelineSeconds(timeConverter_.pixelToTime(x - pianoKeyWidth_));
}

float PianoRollComponent::recalculatePIP(Note& note) {
    if (!currentCurve_) return -1.0f;

    if (note.endTime <= note.startTime) return -1.0f;

    auto snapshot = currentCurve_->getSnapshot();
    const auto& originalF0 = snapshot->getOriginalF0();

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return -1.0f;
    const auto noteRange = f0tl.nonEmptyRangeForTimes(note.startTime, note.endTime);
    const int startFrame = noteRange.startFrame;
    const int endFrameExclusive = noteRange.endFrameExclusive;
    
    if (startFrame >= endFrameExclusive) return -1.0f;

    int numFrames = endFrameExclusive - startFrame;
    
    std::vector<float> noteF0(static_cast<std::size_t>(numFrames));
    std::copy(originalF0.begin() + startFrame, originalF0.begin() + endFrameExclusive, noteF0.begin());

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
    AppLogger::log("AutoTune: applyAutoTuneToSelection entry"
        " materializationId=" + juce::String(static_cast<juce::int64>(editedMaterializationId_))
        + " curve=" + juce::String(currentCurve_ != nullptr ? 1 : 0)
        + " processor=" + juce::String(processor_ != nullptr ? 1 : 0));

    if (!currentCurve_) {
        return false;
    }

    if (!processor_) {
        return false;
    }

    if (editedMaterializationId_ == 0) {
        return false;
    }

    const auto originalF0State = processor_->getMaterializationOriginalF0StateById(editedMaterializationId_);
    if (originalF0State != OriginalF0State::Ready) {
        return false;
    }

    if (autoTuneInFlight_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }

    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Decoration));

    auto snapshot = currentCurve_->getSnapshot();
    if (!snapshot) {
        AppLogger::log("AutoTune: apply failed reason=null_snapshot");
        autoTuneInFlight_.store(false, std::memory_order_release);
        return false;
    }

    AppLogger::log("AutoTune: snapshot frames=" + juce::String(static_cast<int>(snapshot->size()))
        + " hop=" + juce::String(snapshot->getHopSize())
        + " sampleRate=" + juce::String(snapshot->getSampleRate())
        + " duration=" + juce::String(materializationProjection_.materializationDurationSeconds));

    const auto& originalF0 = snapshot->getOriginalF0();
    if (originalF0.empty()) {
        autoTuneInFlight_.store(false, std::memory_order_release);

        return false;
    }
    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) {
        autoTuneInFlight_.store(false, std::memory_order_release);
        return false;
    }

    int selectedNotesStartFrame = 0;
    int selectedNotesEndFrameExclusive = 0;
    const bool hasSelectedNotesRange = getSelectedNotesFrameRange(selectedNotesStartFrame,
                                                                  selectedNotesEndFrameExclusive);

    int selectionAreaStartFrame = 0;
    int selectionAreaEndFrameExclusive = 0;
    const bool hasSelectionAreaRange = getSelectionAreaFrameRange(selectionAreaStartFrame,
                                                                  selectionAreaEndFrameExclusive);

    int f0SelectionStartFrame = 0;
    int f0SelectionEndFrameExclusive = 0;
    const bool hasF0SelectionRange = getF0SelectionFrameRange(f0SelectionStartFrame,
                                                              f0SelectionEndFrameExclusive);

    AudioEditingScheme::AutoTuneTargetContext targetContext;
    targetContext.totalFrameCount = f0tl.endFrameExclusive();
    if (hasSelectedNotesRange) {
        targetContext.selectedNotesRange = { selectedNotesStartFrame, selectedNotesEndFrameExclusive };
    }
    if (hasSelectionAreaRange) {
        targetContext.selectionAreaRange = { selectionAreaStartFrame, selectionAreaEndFrameExclusive };
    }
    if (hasF0SelectionRange) {
        targetContext.f0SelectionRange = { f0SelectionStartFrame, f0SelectionEndFrameExclusive };
    }

    const auto targetDecision = AudioEditingScheme::resolveAutoTuneRange(audioEditingScheme_, targetContext);
    if (targetDecision.target == AudioEditingScheme::AutoTuneTarget::None) {
        autoTuneInFlight_.store(false, std::memory_order_release);

        return false;
    }

    const auto targetRange = f0tl.rangeForFrames(targetDecision.range.startFrame,
                                                targetDecision.range.endFrameExclusive);
    if (targetRange.isEmpty()) {
        autoTuneInFlight_.store(false, std::memory_order_release);
        return false;
    }

    const int startFrame = targetRange.startFrame;
    const int endFrameExclusive = targetRange.endFrameExclusive;
    const int endFrame = targetRange.endFrameExclusive - 1;

    const bool useScaleSnap = (scaleType_ != 3);

    NoteGeneratorParams genParams;
    genParams.policy = segmentationPolicy_;
    genParams.retuneSpeed = currentRetuneSpeed_;
    genParams.vibratoDepth = currentVibratoDepth_;
    genParams.vibratoRate = currentVibratoRate_;
    if (useScaleSnap) {
        ScaleSnapConfig snapCfg;
        snapCfg.root = scaleRootNote_ % 12;
        switch (scaleType_) {
            case 1: snapCfg.mode = ScaleMode::Major; break;
            case 2: snapCfg.mode = ScaleMode::Minor; break;
            case 4: snapCfg.mode = ScaleMode::HarmonicMinor; break;
            case 5: snapCfg.mode = ScaleMode::Dorian; break;
            case 6: snapCfg.mode = ScaleMode::Mixolydian; break;
            case 7: snapCfg.mode = ScaleMode::PentatonicMajor; break;
            case 8: snapCfg.mode = ScaleMode::PentatonicMinor; break;
            default: snapCfg.mode = ScaleMode::Major; break;
        }
        genParams.scaleSnap = snapCfg;
    }

    auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
    request->kind = PianoRollCorrectionWorker::AsyncCorrectionRequest::Kind::AutoTuneGenerate;
    request->startFrame = startFrame;
    request->endFrameExclusive = endFrameExclusive;
    request->retuneSpeed = currentRetuneSpeed_;
    request->vibratoDepth = currentVibratoDepth_;
    request->vibratoRate = currentVibratoRate_;
    request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);

    request->autoOriginalF0Full = originalF0;
    request->autoHopSize = currentCurve_ ? currentCurve_->getHopSize() : 512;
    request->autoF0SampleRate = currentCurve_ ? currentCurve_->getSampleRate() : 16000.0;
    request->autoStartFrame = startFrame;
    request->autoEndFrame = endFrame;
    request->autoGenParams = genParams;

    request->materializationEpochSnapshot = editedMaterializationEpoch_.load(std::memory_order_acquire);
    request->materializationIdSnapshot = editedMaterializationId_;

    captureBeforeUndoSnapshot();
    pendingUndoDescription_ = TRANS("自动调音");

    correctionWorker_->enqueue(request);

    AppLogger::log("AutoTune: enqueued materializationId=" + juce::String(static_cast<juce::int64>(editedMaterializationId_))
        + " startFrame=" + juce::String(startFrame) + " endFrame=" + juce::String(endFrame));

    return true;
}

bool PianoRollComponent::applyAutoSnap()
{
    if (processor_ == nullptr || editedMaterializationId_ == 0 || currentCurve_ == nullptr)
        return false;

    auto refNotes = processor_->getMaterializationStore()->getReferenceNotes(editedMaterializationId_);
    if (refNotes.empty()) return false;

    // Apply reference time offset to notes for matching
    const double refOffset = processor_->getMaterializationStore()->getReferenceTimeOffset(editedMaterializationId_);
    if (std::abs(refOffset) > 1.0e-9) {
        for (auto& ref : refNotes) {
            ref.onset  += refOffset;
            ref.offset += refOffset;
        }
    }

    auto notes = getEditedMaterializationNotesCopy();
    if (notes.empty()) return false;

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return false;

    // Capture undo before state
    captureBeforeUndoSnapshot();

    auto snapshot = currentCurve_->getSnapshot();
    if (snapshot == nullptr) {
        undoSnapshotCaptured_ = false;
        return false;
    }
    auto segments = snapshot->getCorrectedSegments();
    bool anyChanged = false;

    for (auto& note : notes) {
        // Find best matching reference note (max time overlap)
        const ReferenceNote* bestRef = nullptr;
        double bestOverlap = 0.0;

        for (const auto& ref : refNotes) {
            if (!ref.voiced) continue;
            const double overlapStart = std::max(note.startTime, ref.onset);
            const double overlapEnd = std::min(note.endTime, ref.offset);
            const double overlap = overlapEnd - overlapStart;
            if (overlap > bestOverlap) {
                bestOverlap = overlap;
                bestRef = &ref;
            }
        }

        if (bestRef == nullptr) continue;

        // Skip notes with no valid pitch
        if (note.getAdjustedPitch() <= 0.0f) continue;

        // Compute pitch difference in semitones
        const float currentMidi = 12.0f * std::log2(note.getAdjustedPitch() / 440.0f) + 69.0f;
        const float diff = bestRef->midiPitch - currentMidi;
        if (std::abs(diff) < 0.05f) continue; // Already close enough

        // Snap note pitch to reference
        note.pitch = 440.0f * std::pow(2.0f, (bestRef->midiPitch - 69.0f) / 12.0f);
        note.pitchOffset = 0.0f;
        note.dirty = true;
        anyChanged = true;

        // Compute note frame range
        const auto noteRange = f0tl.rangeForTimes(note.startTime, note.endTime);
        if (noteRange.isEmpty()) continue;

        // Shift CorrectedF0 in this note's frame range
        const float ratio = std::pow(2.0f, diff / 12.0f);
        for (auto& seg : segments) {
            const int overlapStart = std::max(seg.startFrame, noteRange.startFrame);
            const int overlapEnd = std::min(seg.endFrame, noteRange.endFrameExclusive);
            if (overlapEnd <= overlapStart) continue;

            const int dataOffset = overlapStart - seg.startFrame;
            const int dataEnd = overlapEnd - seg.startFrame;
            for (int i = dataOffset; i < dataEnd && i < static_cast<int>(seg.f0Data.size()); ++i) {
                if (seg.f0Data[i] > 0.0f) {
                    seg.f0Data[i] *= ratio;
                }
            }
        }
    }

    if (!anyChanged) {
        undoSnapshotCaptured_ = false;
        return false;
    }

    // Commit changes (handles undo recording internally)
    pendingUndoDescription_ = TRANS("Auto-Snap");
    const bool committed = commitEditedMaterializationNotesAndSegments(notes, segments);
    if (committed) {
        invalidateVisual(static_cast<uint32_t>(PianoRollVisualInvalidationReason::Content));
    }
    return committed;
}

void PianoRollComponent::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) {
    if (scrollBar == &horizontalScrollBar_) {
        setScrollOffset(static_cast<int>(newRangeStart));
        userScrollHold_ = true;
    } else if (scrollBar == &verticalScrollBar_) {
        verticalScrollOffset_ = static_cast<float>(newRangeStart);
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport));
    }
}

std::vector<Note> PianoRollComponent::getEditedMaterializationNotesCopy() const {
    return cachedNotes_;
}

bool PianoRollComponent::isAutoTuneProcessing() const
{
    return autoTuneInFlight_.load(std::memory_order_acquire);
}

void PianoRollComponent::updateScrollBars() {
    double maxTime = 0.0;
    if (audioBuffer_) {
        maxTime = static_cast<double>(audioBuffer_->getNumSamples()) / audioBufferSampleRate_;
    } else {
        const auto& notes = getCommittedNotes();
        for (const auto& note : notes) {
            if (note.endTime > maxTime) maxTime = note.endTime;
        }
    }
    
    // Add some padding
    maxTime = std::max(maxTime, 10.0); // Minimum 10 seconds
    maxTime += 5.0; // Extra padding
    
    double pixelsPerSecond = getTimelinePixelsPerSecond();
    int totalContentWidth = static_cast<int>(maxTime * pixelsPerSecond);
    int visibleWidth = getWidth() - pianoKeyWidth_ - 15; // -15 for vertical scrollbar
    visibleWidth = juce::jmax(1, visibleWidth);
    
    horizontalScrollBar_.setRangeLimits(0.0, totalContentWidth + visibleWidth, juce::dontSendNotification);
    horizontalScrollBar_.setCurrentRange(scrollOffset_, visibleWidth, juce::dontSendNotification);
    
    // Vertical
    float totalHeight = getTotalHeight();
    int visibleHeight = getHeight() - rulerHeight_ - 15; // -15 for horizontal scrollbar
    visibleHeight = juce::jmax(1, visibleHeight);
    
    verticalScrollBar_.setRangeLimits(0.0, totalHeight, juce::dontSendNotification);
    verticalScrollBar_.setCurrentRange(verticalScrollOffset_, visibleHeight, juce::dontSendNotification);
}

} // namespace OpenTune
