#include "PianoRollComponent.h"
#include "../../Utils/LocalizationManager.h"
#include "../Utils/AppLogger.h"
#include "../../Utils/PianoRollEditAction.h"
#include "../../Utils/TimeGridEditAction.h"   // ⚡️ vocal-time-stretch §8.7
#include <algorithm>
#include <cmath>
#include <limits>
#include "../DSP/ChromaKeyDetector.h"
#include "../Utils/LegacyNoteGenerator.h"
#include "../Utils/SimdPerceptualPitchEstimator.h"
#include "../Utils/ZoomSensitivityConfig.h"
#include "../../PluginProcessor.h"
#include "FrameScheduler.h"
#include "UiAssets.h"
#include "UiText.h"
#include "ToolbarIcons.h"
#include "../../Utils/AudioEditingScheme.h"
#include "Utils/PianoKeyAudition.h"
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

bool isManualCorrectionSource(CorrectedSegment::Source source) noexcept
{
    return source == CorrectedSegment::Source::HandDraw
        || source == CorrectedSegment::Source::LineAnchor;
}

int64_t secondsToMs(double seconds) noexcept
{
    return static_cast<int64_t>(std::llround(seconds * 1000.0));
}

int quantizeGeometryPx(float value) noexcept
{
    return static_cast<int>(std::lround(value * 1000.0f));
}

uint64_t hashCombine(uint64_t seed, uint64_t value) noexcept
{
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
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
    playheadOverlay_.setPlayheadColour(UIColors::playhead);

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
                                            const std::vector<CorrectedSegment>& segments,
                                            F0FrameRange affectedRange) {
        return commitEditedMaterializationNotesAndSegments(notes, segments, affectedRange);
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
        menu.addItem("Time Tool (T)", [this]() { setCurrentTool(ToolId::TimeTool); });
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
    toolCtx.getMaterializationProjection = [this]() { return activeMaterializationProjection(); };
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
        pendingSeekTime_ = time;
        const bool isPlaying = isPlaying_.load(std::memory_order_relaxed);
        playheadOverlay_.setPlayheadSeconds(time);
            // 播放中 seek：设置 pending，VBlank 用 pending 值居中直到 host 确认
        if (scrollMode_ == ScrollMode::Continuous || isPlaying) {
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

    // ============================================================
    // ⚡️ vocal-time-stretch §8.7 — Time tool / TimeGrid wiring
    // ============================================================
    toolCtx.getMaterializationIdForView = [this]() -> uint64_t {
        return editedMaterializationId_;
    };
    toolCtx.getTimeGridSnapshot = [this]() -> std::shared_ptr<const TimeGridSnapshot> {
        if (processor_ == nullptr || editedMaterializationId_ == 0) return nullptr;
        return processor_->getMaterializationTimeGridById(editedMaterializationId_);
    };
    toolCtx.commitTimeGrid = [this](std::shared_ptr<const TimeGridSnapshot> newSnap,
                                     std::shared_ptr<const TimeGridSnapshot> oldSnap,
                                     int64_t affectedSrcStart,
                                     int64_t affectedSrcEnd,
                                     juce::String description) -> bool {
        if (processor_ == nullptr || editedMaterializationId_ == 0) return false;
        if (newSnap == nullptr) return false;

        auto action = std::make_unique<TimeGridEditAction>(
            *processor_,
            editedMaterializationId_,
            description.isNotEmpty() ? description : juce::String("编辑时间网格"),
            std::move(oldSnap),
            newSnap,
            affectedSrcStart,
            affectedSrcEnd);
        // First publish the new snapshot to the processor (the action's redo()
        // will replay this); then push the action so undo() reverts.
        const bool published = processor_->setMaterializationTimeGridById(
            editedMaterializationId_, newSnap, affectedSrcStart, affectedSrcEnd);
        if (!published) return false;
        processor_->getUndoManager().addAction(std::move(action));
        return true;
    };
    toolCtx.notifyTimeGridChanged = [this]() {
        // §8.6 — wider repaint via VisualInvalidation TimeGrid reason.
        ++visualPrefsRevision_;
        prepareVisibleRenderModel();
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content),
                         PianoRollVisualInvalidationPriority::Interactive);
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
        F0FrameRange notifyRange;
        if (wasAutoTune) {
            notifyRange = PitchCurve::expandNoteBasedCorrectionRange(completed->autoStartFrame,
                                                                     completed->autoEndFrame + 1,
                                                                     currentF0Timeline().endFrameExclusive());
        } else {
            notifyRange = PitchCurve::expandNoteBasedCorrectionRange(completed->startFrame,
                                                                     completed->endFrameExclusive,
                                                                     currentF0Timeline().endFrameExclusive());
        }

        const int notifyEnd = std::max(notifyRange.startFrame, notifyRange.endFrameExclusive - 1);
        listeners_.call([notifyRange, notifyEnd](Listener& l) { l.pitchCurveEdited(notifyRange.startFrame, notifyEnd); });
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
    const auto autoTuneRange = PitchCurve::expandNoteBasedCorrectionRange(
        completed.autoStartFrame,
        completed.autoEndFrame + 1,
        currentF0Timeline().endFrameExclusive());
    recordUndoAction(pendingUndoDescription_, autoTuneRange);
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

    const auto correctionRange = PitchCurve::expandNoteBasedCorrectionRange(
        completed.startFrame,
        completed.endFrameExclusive,
        currentF0Timeline().endFrameExclusive());
    if (!commitEditedMaterializationCorrectedSegments(copyCorrectedSegments(completed.curve),
                                                       correctionRange)) {
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
    prepareVisibleRenderModel();
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

    // Pure note edits do not own a corrected-F0 range, so the undo snapshot covers the materialization.
    const auto success = commitEditedMaterializationNotes(interactionState_.noteDraft.workingNotes,
                                                          currentFullF0Range());
    if (success) {
        interactionState_.noteDraft.clear();
    }
    return success;
}

void PianoRollComponent::clearNoteDraft()
{
    interactionState_.noteDraft.clear();
}

bool PianoRollComponent::commitEditedMaterializationNotes(const std::vector<Note>& notes,
                                                          F0FrameRange affectedRange)
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
    recordUndoAction(pendingUndoDescription_, affectedRange);
    return true;
}

bool PianoRollComponent::commitEditedMaterializationNotesAndSegments(const std::vector<Note>& notes,
                                                             const std::vector<CorrectedSegment>& segments,
                                                             F0FrameRange affectedRange)
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

    recordUndoAction(pendingUndoDescription_, affectedRange);
    return true;
}

bool PianoRollComponent::commitEditedMaterializationCorrectedSegments(const std::vector<CorrectedSegment>& segments,
                                                                       F0FrameRange affectedRange)
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

    recordUndoAction(pendingUndoDescription_, affectedRange);
    return true;
}

std::vector<CorrectedSegment> PianoRollComponent::getCurrentSegments() const
{
    if (!currentCurve_) return {};
    auto snap = currentCurve_->getSnapshot();
    if (!snap) return {};
    return snap->getCorrectedSegments();
}

F0FrameRange PianoRollComponent::currentFullF0Range() const
{
    if (!currentCurve_) return F0FrameRange{0, 0};
    auto snap = currentCurve_->getSnapshot();
    if (!snap) return F0FrameRange{0, 0};
    const int totalFrames = static_cast<int>(snap->getOriginalF0().size());
    return F0FrameRange{0, totalFrames};
}

void PianoRollComponent::captureBeforeUndoSnapshot()
{
    beforeUndoNotes_ = cachedNotes_;
    beforeUndoSegments_ = getCurrentSegments();
    undoSnapshotCaptured_ = true;
}

void PianoRollComponent::recordUndoAction(const juce::String& description, F0FrameRange affectedRange)
{
    if (processor_ == nullptr || editedMaterializationId_ == 0 || !undoSnapshotCaptured_)
        return;

    AppLogger::log("AutoTune: recordUndoAction entry beforeNotes=" + juce::String(static_cast<int>(beforeUndoNotes_.size()))
        + " beforeSegments=" + juce::String(static_cast<int>(beforeUndoSegments_.size()))
        + " cachedNotes=" + juce::String(static_cast<int>(cachedNotes_.size()))
        + " affectedRange=[" + juce::String(affectedRange.startFrame)
        + "," + juce::String(affectedRange.endFrameExclusive) + ")");

    auto afterNotes = cachedNotes_;
    auto afterSegments = getCurrentSegments();

    AppLogger::log("AutoTune: recordUndoAction afterSegments=" + juce::String(static_cast<int>(afterSegments.size()))
        + " constructing PianoRollEditAction");

    // F0FrameRange.endFrameExclusive 是开区间右端；PianoRollEditAction 的
    // affectedEndFrame 是闭区间右端（兼容既有 getter 语义），换算 -1。
    const int affectedStart = std::max(0, affectedRange.startFrame);
    const int affectedEnd = std::max(affectedStart,
                                      affectedRange.endFrameExclusive > 0
                                          ? affectedRange.endFrameExclusive - 1
                                          : 0);

    auto action = std::make_unique<PianoRollEditAction>(
        *processor_,
        editedMaterializationId_,
        description.isNotEmpty() ? description : TRANS("编辑"),
        std::move(beforeUndoNotes_),
        std::move(afterNotes),
        std::move(beforeUndoSegments_),
        std::move(afterSegments),
        affectedStart,
        affectedEnd);

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
        // 选中态变化只动 note.selected，不影响 vocoder 渲染；用 selectionRange 作为
        // 可能的 affected 范围（注意：选中态变化通常不需要 enqueuePartialRender，
        // 但一致性起见仍透传）。
        commitEditedMaterializationNotes(notes, selectionRange);
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

    const auto projection = activeMaterializationProjection();
    if (projection.isValid()
        && (note.endTime <= 0.0
            || note.startTime >= projection.materializationDurationSeconds)) {
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
            op.source);
    }

    // dirtyStartFrame/dirtyEndFrame 是所有 manual ops 的 dirty 帧并集（含端点）。
    const F0FrameRange affectedRange{dirtyStartFrame,
                                      dirtyEndFrame >= dirtyStartFrame ? dirtyEndFrame + 1 : dirtyStartFrame};
    if (!commitEditedMaterializationCorrectedSegments(copyCorrectedSegments(editedCurve), affectedRange)) {
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

void PianoRollComponent::drawHandDrawPreview(juce::Graphics& g) {
    if (!interactionState_.drawing.isDrawingF0 || currentTool_ != ToolId::HandDraw || interactionState_.drawing.handDrawBuffer.empty() || !currentCurve_) return;

    auto snapshot = currentCurve_->getSnapshot();
    const auto& originalF0 = snapshot->getOriginalF0();
    if (originalF0.empty() || interactionState_.drawing.handDrawBuffer.size() != originalF0.size()) return;

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return;
    juce::Colour previewColour = UIColors::correctedF0;
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
    if (audioEditingScheme_ != AudioEditingScheme::Scheme::CorrectedF0Primary
        || !showCorrectedF0_
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
    else if (themeId == ThemeId::Overdose) {
        fill = UIColors::lightPurple;
        stroke = UIColors::accent;
        fillAlpha = 0.15f;
        strokeAlpha = 0.65f;
        strokeThickness = 1.2f;
    }

    g.setColour(fill.withAlpha(fillAlpha));
    g.fillRoundedRectangle(rect, 3.0f);
    g.setColour(stroke.withAlpha(strokeAlpha));
    g.drawRoundedRectangle(rect, 3.0f, strokeThickness);
}

void PianoRollComponent::paintBackgroundOnly(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced(12.0f);
    const auto themeId = UIColors::currentThemeId();

    UIColors::drawShadow(g, bounds);

    juce::Path backgroundPath;
    backgroundPath.addRoundedRectangle(bounds, UIColors::cornerRadius);
    g.reduceClipRegion(backgroundPath);

    if (themeId == ThemeId::DarkBlueGrey)
        UIColors::fillSoothe2SpectrumBackground(g, bounds, UIColors::cornerRadius);
    else if (themeId == ThemeId::Aurora)
        UIColors::fillAuroraTimelineBackground(g, bounds, UIColors::cornerRadius);
    else if (themeId == ThemeId::BlueBreeze)
        UIColors::fillMistedTimelineField(g, bounds, UIColors::cornerRadius);
    else if (themeId == ThemeId::Overdose)
        UiAssets::drawAssetStretch(g, UiAssetId::PanelEditorMain, bounds);
    else
        g.setColour(UIColors::rollBackground);

    if (themeId != ThemeId::DarkBlueGrey && themeId != ThemeId::Aurora && themeId != ThemeId::BlueBreeze && themeId != ThemeId::Overdose)
        g.fillPath(backgroundPath);
}

void PianoRollComponent::paint(juce::Graphics& g) {
    // paint() ONLY consumes the prepared render model — no data preparation.
    // Cache refresh happens from state-change paths before repaint is requested.
    if (!renderModelCache_.isValid()) {
        // Safety net: cache not ready, draw background only. State-change callers
        // should refresh the prepared model before requesting repaint.
        paintBackgroundOnly(g);
        return;
    }
    const auto& ctx = renderModelCache_.getRenderContext();
    const auto timelineViewportBounds = getTimelineViewportBounds();
    auto bounds = getLocalBounds().toFloat().reduced(12.0f);
    const auto themeId = UIColors::currentThemeId();

    UIColors::drawShadow(g, bounds);

    juce::Path backgroundPath;
    backgroundPath.addRoundedRectangle(bounds, UIColors::cornerRadius);
    g.reduceClipRegion(backgroundPath);

    if (themeId == ThemeId::DarkBlueGrey)
        UIColors::fillSoothe2SpectrumBackground(g, bounds, UIColors::cornerRadius);
    else if (themeId == ThemeId::Aurora)
        UIColors::fillAuroraTimelineBackground(g, bounds, UIColors::cornerRadius);
    else if (themeId == ThemeId::BlueBreeze)
        UIColors::fillMistedTimelineField(g, bounds, UIColors::cornerRadius);
    else if (themeId == ThemeId::Overdose)
        UiAssets::drawAssetStretch(g, UiAssetId::PanelEditorMain, bounds);
    else
        g.setColour(UIColors::rollBackground);

    if (themeId != ThemeId::DarkBlueGrey && themeId != ThemeId::Aurora && themeId != ThemeId::BlueBreeze && themeId != ThemeId::Overdose)
        g.fillPath(backgroundPath);

    renderer_->drawTimeRuler(g, ctx);

    {
        const juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(timelineViewportBounds.withTrimmedLeft(pianoKeyWidth_));

        renderer_->drawGridLines(g, ctx);
        for (const auto& item : ctx.materializations)
            renderer_->drawUnvoicedFrameBands(g, ctx, item);

        // Ghost notes & reference anchors overlay（参考 clip 半透明投影）
        if (ctx.referenceOverlay.has_value() && ctx.referenceOverlay->enabled) {
            renderer_->drawGhostNotes(g, ctx, *ctx.referenceOverlay);
            renderer_->drawGhostAnchors(g, ctx, *ctx.referenceOverlay);
        }

        if (showWaveform_) {
            for (const auto& item : ctx.materializations)
                renderer_->drawWaveform(g, ctx, item);
        }

        // ⚡️ vocal-time-stretch §8.5 (Phase J) — Time view hides Pitch
        // furniture (note rows / staves / piano keys / F0 curves) so the user
        // can focus on time anchors.  Lanes / notes / F0 are skipped; waveform
        // + handles + chunk boundaries remain.
        if (!ctx.isTimeView()) {
            renderer_->drawLanes(g, ctx);

            for (const auto& item : ctx.materializations)
                renderer_->drawNotes(g, ctx, item);

            bool drewActivePitch = false;
            for (const auto& item : ctx.materializations) {
                if (item.pitchSnapshot == nullptr)
                    continue;

                if (showOriginalF0_ && !item.originalF0VisualSegments.empty())
                    renderer_->drawPreparedF0Curve(g, item.originalF0VisualSegments, UIColors::originalF0, 0.78f, true, ctx);

                if (showCorrectedF0_ && !item.correctedF0VisualSegments.empty())
                    renderer_->drawPreparedF0Curve(g, item.correctedF0VisualSegments, UIColors::correctedF0, 1.0f, false, ctx);

                if (item.active) {
                    drawNoteDragCurvePreview(g);
                    drawHandDrawPreview(g);
                    drawLineAnchorPreview(g);
                    drewActivePitch = true;
                }
            }

            if (!drewActivePitch && currentCurve_ != nullptr) {
                drawHandDrawPreview(g);
                drawLineAnchorPreview(g);
            }
        }

        for (const auto& item : ctx.materializations)
            renderer_->drawChunkBoundaries(g, ctx, item);
    }

    // ⚡️ §8.5 — paint TimeGrid handles ABOVE chunk boundaries / waveform but
    // BELOW the piano keys (which sit on the left edge).  In Time view we
    // also force-render endpoint handles (even on identity grid) so the user
    // sees ClipStart / ClipEnd as anchor references.
    renderer_->drawTimeGridHandles(g, ctx);

    // §8.5 (Phase J) — Pitch view shows piano keys; Time view replaces the
    // left band with a dim spacer so the timeline aligns visually.
    if (!ctx.isTimeView()) {
        renderer_->drawPianoKeys(g, ctx);
    }

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
        const auto editRange = f0tl.rangeForTimes(dirtyStartTime, dirtyEndTime);
        if (!editRange.isEmpty()) {
            auto clonedCurve = currentCurve_->clone();
            clonedCurve->applyCorrectionToRange(
                notes, editRange.startFrame, editRange.endFrameExclusive,
                retuneSpeed, vibratoDepth, vibratoRate, 44100.0);
            auto snap = clonedCurve->getSnapshot();

            const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(
                editRange.startFrame,
                editRange.endFrameExclusive,
                f0tl.endFrameExclusive());
            if (!commitEditedMaterializationNotesAndSegments(notes, snap->getCorrectedSegments(), affectedRange)) {
                return false;
            }

            listeners_.call([affectedRange](Listener& l) { l.pitchCurveEdited(affectedRange.startFrame, affectedRange.endFrameExclusive - 1); });
            invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
            return true;
        } else {
            if (!commitEditedMaterializationNotes(notes, currentFullF0Range())) {
                return false;
            }
        }
    } else {
        if (!commitEditedMaterializationNotes(notes, currentFullF0Range())) {
            return false;
        }
    }
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
    return true;
}

bool PianoRollComponent::applyParameterToFrameRange(float retuneSpeed, float vibratoDepth, float vibratoRate, int startFrame, int endFrameExclusive) {
    if (!currentCurve_ || endFrameExclusive <= startFrame) return false;
    if (!currentCurve_->hasCorrectionInRange(startFrame, endFrameExclusive)) return false;
    if (hasManualCorrectionInRange(startFrame, endFrameExclusive)) return true;

    enqueueNoteBasedCorrectionAsync(getEditedMaterializationNotesCopy(),
                                    startFrame, endFrameExclusive,
                                    retuneSpeed, vibratoDepth, vibratoRate);

    const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(startFrame,
                                                                          endFrameExclusive,
                                                                          currentF0Timeline().endFrameExclusive());
    const int notifyEndFrame = std::max(affectedRange.startFrame, affectedRange.endFrameExclusive - 1);
    listeners_.call([affectedRange, notifyEndFrame](Listener& l) { l.pitchCurveEdited(affectedRange.startFrame, notifyEndFrame); });
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
    context.hasFrameSelection = hasSelectionAreaRange;
    context.allowWholeClipFallback = false;

    switch (AudioEditingScheme::resolveParameterTarget(
        audioEditingScheme_,
        AudioEditingScheme::ParameterKind::RetuneSpeed,
        context)) {
        case AudioEditingScheme::ParameterTarget::SelectedNotes:
            return applyNoteParameterToSelectedNotes(speed, currentVibratoDepth_, currentVibratoRate_);
        case AudioEditingScheme::ParameterTarget::FrameSelection:
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

bool PianoRollComponent::hasManualCorrectionInRange(int startFrame, int endFrame) const {
    if (currentCurve_ == nullptr || startFrame >= endFrame) {
        return false;
    }

    auto snapshot = currentCurve_->getSnapshot();
    const auto& segments = snapshot->getCorrectedSegments();
    for (const auto& seg : segments) {
        if (!isManualCorrectionSource(seg.source)) {
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
    ++viewportSizeRevision_;

    auto bounds = getLocalBounds().reduced(12);

    // Reserve space for scrollbars
    horizontalScrollBar_.setBounds(bounds.removeFromBottom(UIColors::scrollBarThickness));
    verticalScrollBar_.setBounds(bounds.removeFromRight(UIColors::scrollBarThickness));

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
    prepareVisibleRenderModel();
}

void PianoRollComponent::applyEditedMaterializationCurve(std::shared_ptr<PitchCurve> curve)
{
    currentCurve_ = std::move(curve);

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
}

double PianoRollComponent::getMaterializationDurationSeconds() const
{
    return activeMaterializationProjection().materializationDurationSeconds;
}

const PianoRollComponent::TimelineMaterializationPlacement* PianoRollComponent::findActiveTimelineMaterializationPlacement() const noexcept
{
    const auto activeIt = std::find_if(timelineMaterializationPlacements_.begin(),
                                       timelineMaterializationPlacements_.end(),
                                       [this](const auto& placement) {
                                           return placement.materializationId == editedMaterializationId_
                                               && placement.isValid();
                                       });
    if (activeIt != timelineMaterializationPlacements_.end()) {
        return &(*activeIt);
    }

    const auto firstValidIt = std::find_if(timelineMaterializationPlacements_.begin(),
                                           timelineMaterializationPlacements_.end(),
                                           [](const auto& placement) { return placement.isValid(); });
    return firstValidIt != timelineMaterializationPlacements_.end() ? &(*firstValidIt) : nullptr;
}

MaterializationTimelineProjection PianoRollComponent::activeMaterializationProjection() const noexcept
{
    const auto* activePlacement = findActiveTimelineMaterializationPlacement();
    if (activePlacement != nullptr) {
        return activePlacement->projection;
    }
    return !explicitTimelineMaterializationPlacements_
        ? pendingSingleMaterializationProjection_
        : MaterializationTimelineProjection{};
}

bool PianoRollComponent::applyTimelineMaterializationPlacements(std::vector<TimelineMaterializationPlacement> placements,
                                                               bool explicitContract)
{
    placements.erase(std::remove_if(placements.begin(),
                                    placements.end(),
                                    [](const auto& placement) { return !placement.isValid(); }),
                     placements.end());

    const bool changed = placements.size() != timelineMaterializationPlacements_.size()
        || !std::equal(placements.begin(), placements.end(), timelineMaterializationPlacements_.begin(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.materializationId == rhs.materializationId
                    && std::abs(lhs.projection.timelineStartSeconds - rhs.projection.timelineStartSeconds) <= 1.0e-9
                    && std::abs(lhs.projection.timelineDurationSeconds - rhs.projection.timelineDurationSeconds) <= 1.0e-9
                    && std::abs(lhs.projection.materializationDurationSeconds - rhs.projection.materializationDurationSeconds) <= 1.0e-9;
            });

    explicitTimelineMaterializationPlacements_ = explicitContract;

    if (!changed) {
        return false;
    }

    timelineMaterializationPlacements_ = std::move(placements);

    std::unordered_set<uint64_t> aliveMaterializations;
    aliveMaterializations.reserve(timelineMaterializationPlacements_.size());
    for (const auto& placement : timelineMaterializationPlacements_)
        aliveMaterializations.insert(placement.materializationId);
    waveformMipmapCache_.prune(aliveMaterializations);

    if (!timelineViewDomain_.isValid()) {
        playheadOverlay_.setTimelineStartSeconds(timelineViewOriginSeconds());
    }
    userScrollHold_ = false;
    updateScrollBars();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                     PianoRollVisualInvalidationPriority::Interactive);
    return true;
}

void PianoRollComponent::deriveSingleTimelineMaterializationPlacement()
{
    if (explicitTimelineMaterializationPlacements_) {
        return;
    }

    std::vector<TimelineMaterializationPlacement> placements;
    if (editedMaterializationId_ != 0 && pendingSingleMaterializationProjection_.isValid()) {
        placements.push_back({ editedMaterializationId_, pendingSingleMaterializationProjection_ });
    }

    applyTimelineMaterializationPlacements(std::move(placements), false);
}

void PianoRollComponent::setMaterializationProjection(const MaterializationTimelineProjection& projection)
{
    const bool changed = std::abs(pendingSingleMaterializationProjection_.timelineStartSeconds - projection.timelineStartSeconds) > 1.0e-9
        || std::abs(pendingSingleMaterializationProjection_.timelineDurationSeconds - projection.timelineDurationSeconds) > 1.0e-9
        || std::abs(pendingSingleMaterializationProjection_.materializationDurationSeconds - projection.materializationDurationSeconds) > 1.0e-9;

    if (!changed) {
        return;
    }

    pendingSingleMaterializationProjection_ = projection;
    explicitTimelineMaterializationPlacements_ = false;
    deriveSingleTimelineMaterializationPlacement();
    if (!timelineViewDomain_.isValid()) {
        playheadOverlay_.setTimelineStartSeconds(timelineViewOriginSeconds());
    }
    prepareVisibleRenderModel();
    userScrollHold_ = false;
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                     PianoRollVisualInvalidationPriority::Interactive);
}

void PianoRollComponent::setTimelineMaterializationPlacements(std::vector<TimelineMaterializationPlacement> placements)
{
    if (applyTimelineMaterializationPlacements(std::move(placements), true)) {
        pendingSingleMaterializationProjection_ = activeMaterializationProjection();
        prepareVisibleRenderModel();
    }
}

void PianoRollComponent::setTimelineViewDomain(double viewStartSeconds, double viewEndSeconds)
{
    TimelineViewDomain domain;
    domain.startSeconds = std::max(0.0, viewStartSeconds);
    domain.endSeconds = std::max(domain.startSeconds, viewEndSeconds);

    const bool changed = std::abs(timelineViewDomain_.startSeconds - domain.startSeconds) > 1.0e-9
        || std::abs(timelineViewDomain_.endSeconds - domain.endSeconds) > 1.0e-9;

    if (!changed) {
        return;
    }

    timelineViewDomain_ = domain;
    playheadOverlay_.setTimelineStartSeconds(timelineViewOriginSeconds());
    userScrollHold_ = false;
    updateScrollBars();
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                     PianoRollVisualInvalidationPriority::Interactive);
}

void PianoRollComponent::clearTimelineViewDomain()
{
    if (!timelineViewDomain_.isValid()) {
        return;
    }

    timelineViewDomain_ = {};
    playheadOverlay_.setTimelineStartSeconds(timelineViewOriginSeconds());
    userScrollHold_ = false;
    updateScrollBars();
    prepareVisibleRenderModel();
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
        autoTuneInFlight_.store(false, std::memory_order_release);
        pendingUndoDescription_ = {};
        beforeUndoNotes_.clear();
        beforeUndoSegments_.clear();
        undoSnapshotCaptured_ = false;
    }

    // notes 与 pitchCurve 通过 commitNotesAndPitchCurve 同写到 store；
    // 读侧也必须同读：curveChanged 时必须 refresh notes，否则 undo/redo 会
    // 出现 curve 回退但 notes 视觉残留的不对称（cachedNotes_ 滞后）。
    if (materializationChanged || curveChanged) {
        refreshEditedMaterializationNotes();
    }

    if (materializationChanged || curveChanged) {
        editedMaterializationEpoch_.fetch_add(1, std::memory_order_release);
        applyEditedMaterializationCurve(std::move(curve));
    }

    if (materializationChanged || bufferChanged) {
        applyEditedMaterializationAudioBuffer(std::move(buffer), sampleRate);
    }

    if (materializationChanged || bufferChanged) {
        deriveSingleTimelineMaterializationPlacement();
    }

    if (hasAudio && (materializationChanged || bufferChanged)) {
        fitToScreen();
    }

    prepareVisibleRenderModel();
    userScrollHold_ = false;
    updateScrollBars();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content)
                     | toInvalidationMask(PianoRollVisualInvalidationReason::Decoration));
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
    prepareVisibleRenderModel();
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

void PianoRollComponent::requestContentRedraw() {
    FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Normal);
}

void PianoRollComponent::setScrollOffset(int offset) {
    const int newOffset = juce::jmax(0, offset);
    if (newOffset == scrollOffset_) return;
    
    const int oldOffset = scrollOffset_;
    const int scrollDelta = newOffset - oldOffset;
    scrollOffset_ = newOffset;
    timeConverter_.setScrollOffset(scrollOffset_);
    playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));
    horizontalScrollBar_.setCurrentRangeStart(scrollOffset_, juce::dontSendNotification);
    const auto timelineViewportBounds = getTimelineViewportBounds();
    prepareVisibleRenderModel();

    // Only invalidate the exposed strip on ordinary scroll deltas.
    const int contentWidth = timelineViewportBounds.getWidth() - pianoKeyWidth_;
    if (contentWidth > 0 && std::abs(scrollDelta) < contentWidth) {
        // Exposed strip: the newly revealed band due to scroll delta.
        const int stripWidth = std::abs(scrollDelta);
        const int stripX = (scrollDelta > 0)
            ? (timelineViewportBounds.getWidth() - stripWidth)
            : pianoKeyWidth_;
        const juce::Rectangle<int> dirtyArea(stripX, 0, stripWidth, timelineViewportBounds.getHeight());
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

double PianoRollComponent::projectPlayheadTime(double rawPlayheadTime) const
{
    const auto projection = activeMaterializationProjection();
    if (!projection.isValid())
        return rawPlayheadTime;
    return projection.clampTimelineTime(rawPlayheadTime);
}

double PianoRollComponent::readProjectedPlayheadTime() const
{
    return projectPlayheadTime(readPlayheadTime());
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

    if (showWaveform_) {
        if (inferenceActive_)
            waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 8;
        else
            waveformBuildTickCounter_ = 0;

        const bool shouldBuild = !inferenceActive_ || waveformBuildTickCounter_ == 0;
        const double budgetMs = inferenceActive_ ? 1.0 : 5.0;
        if (shouldBuild && waveformMipmapCache_.buildIncremental(budgetMs))
            invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
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

    const double rawHostTime = readPlayheadTime();
    const double hostTime = projectPlayheadTime(rawHostTime);

    if (!isPlaying_.load(std::memory_order_relaxed)) {
        if (pendingSeekTime_ >= 0.0
            && std::abs(rawHostTime - lastObservedRawPlayheadTime_) > 0.001
            && std::abs(hostTime - pendingSeekTime_) > 0.05) {
            pendingSeekTime_ = -1.0;
        }

        lastObservedRawPlayheadTime_ = rawHostTime;
        // Paused: still mirror the host transport position so DAW timeline seeks
        // (and standalone setPosition writes) appear in the plugin window without
        // requiring playback. The playing-only auto-scroll/centering logic below
        // is intentionally skipped — when paused, the user controls the view.
        const double stoppedPresentationTime = pendingSeekTime_ >= 0.0 ? pendingSeekTime_ : hostTime;
        playheadOverlay_.setPlayheadSeconds(stoppedPresentationTime);
        return;
    }

    // 如果有 pending seek，检查 host 是否已确认（position 接近 pending 值）
    lastObservedRawPlayheadTime_ = rawHostTime;
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
    ++visualPrefsRevision_;
    timeConverter_.setZoom(zoomLevel_);
    playheadOverlay_.setZoomLevel(zoomLevel_);
    updateScrollBars();
    prepareVisibleRenderModel();
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

    // ⚡️ vocal-time-stretch §8.4 (Phase F) — Time tool is mutually exclusive
    // with the Note family of tools.  Switching INTO TimeTool drops any
    // inflight note-side state so the user's next mouseDown is interpreted
    // strictly as a TimeGrid handle action; switching OUT clears Time-tool
    // selection so a stale handle highlight doesn't persist into Note tools.
    if (toolChanged) {
        if (tool == ToolId::TimeTool) {
            interactionState_.noteDrag.clear();
            interactionState_.noteResize.clear();
            interactionState_.noteDraft.clear();
            interactionState_.selection.clearF0Selection();
        } else if (currentTool_ == ToolId::TimeTool) {
            interactionState_.timeTool.clear();
        }
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
        case ToolId::TimeTool:
            // §8.4: Time tool uses normal cursor + per-handle hover hand cursor
            // applied by handleTimeToolMouseMove (via ctx.setMouseCursor).
            setMouseCursor(juce::MouseCursor::NormalCursor);
            break;
    }

    // 通知监听者工具已切换（参数面板需要同步按钮高亮）
    if (toolChanged) {
        listeners_.call([tool](Listener& l) { l.currentToolChanged(tool); });
        ++visualPrefsRevision_;
        prepareVisibleRenderModel();
    }

    if (toolChanged || clearedAnchorPreview) {
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Interaction),
                         getLocalBounds(),
                         PianoRollVisualInvalidationPriority::Interactive);
    }
}

void PianoRollComponent::setShowWaveform(bool shouldShow) {
    if (showWaveform_ == shouldShow) return;
    showWaveform_ = shouldShow;
    ++visualPrefsRevision_;
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::setShowLanes(bool shouldShow) {
    if (showLanes_ == shouldShow) return;
    showLanes_ = shouldShow;
    ++visualPrefsRevision_;
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::setNoteNameMode(NoteNameMode noteNameMode)
{
    if (noteNameMode_ == noteNameMode) {
        return;
    }

    noteNameMode_ = noteNameMode;
    ++visualPrefsRevision_;
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::setShowChunkBoundaries(bool shouldShow)
{
    if (showChunkBoundaries_ == shouldShow) {
        return;
    }

    showChunkBoundaries_ = shouldShow;
    ++visualPrefsRevision_;
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::setShowUnvoicedFrames(bool shouldShow)
{
    if (showUnvoicedFrames_ == shouldShow) {
        return;
    }

    showUnvoicedFrames_ = shouldShow;
    ++visualPrefsRevision_;
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::setBpm(double bpm) {
    bpm_ = juce::jlimit(60.0, 240.0, bpm);
    ++visualPrefsRevision_;
    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport));
}

void PianoRollComponent::setTimeSignature(int numerator, int denominator) {
    if (numerator <= 0 || denominator <= 0) {
        return;
    }

    timeSigNum_ = numerator;
    timeSigDenom_ = denominator;
    ++visualPrefsRevision_;
    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport));
}

void PianoRollComponent::setTimeUnit(TimeUnit unit) {
    timeUnit_ = unit;
    ++visualPrefsRevision_;
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport));
}

void PianoRollComponent::addListener(Listener* listener) {
    listeners_.add(listener);
}

void PianoRollComponent::removeListener(Listener* listener) {
    listeners_.remove(listener);
}

void PianoRollComponent::mouseMove(const juce::MouseEvent& e) {
    if (e.mods.isCtrlDown()) {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }
    toolHandler_->mouseMove(e);
}

void PianoRollComponent::mouseDoubleClick(const juce::MouseEvent& e) {
    // ⚡️ vocal-time-stretch §8.4 — Time tool double-click forwarded to handler.
    // Other tools currently have no double-click semantics, so the handler
    // ignores them by switching on currentTool_.
    toolHandler_->mouseDoubleClick(e);
}

void PianoRollComponent::mouseDown(const juce::MouseEvent& e) {
    if (isAutoTuneProcessing()) {
        return;
    }

    // Ctrl+drag panning — only on non-interactive area, so existing
    // Ctrl+click behaviors (note toggle selection, context menu) work.
    if (e.mods.isCtrlDown() && !e.mods.isPopupMenu() && e.x >= pianoKeyWidth_) {
        bool onNote = false;
        for (const auto& note : getCommittedNotes()) {
            if (getNoteBounds(note).contains(e.getPosition())) {
                onNote = true;
                break;
            }
        }
        if (!onNote) {
            interactionState_.isPanning = true;
            interactionState_.dragStartPos = e.getPosition();
            dragStartScrollOffset_ = scrollOffset_;
            dragStartVerticalScrollOffset_ = verticalScrollOffset_;
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            return;
        }
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
        refreshVerticalViewportGeometry();
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
    float visibleHeight = static_cast<float>(getHeight() - rulerHeight_ - UIColors::scrollBarThickness);
    float maxScroll = totalHeight - visibleHeight;
    if (maxScroll > 0.0f) {
        verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    } else {
        verticalScrollOffset_ = 0.0f;
    }
    refreshVerticalViewportGeometry();
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
    float visibleHeight = static_cast<float>(getHeight() - rulerHeight_ - UIColors::scrollBarThickness);
    float maxScroll = totalHeight - visibleHeight;
    if (maxScroll > 0.0f) {
        verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    } else {
        verticalScrollOffset_ = 0.0f;
    }
    refreshVerticalViewportGeometry();
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

PianoRollRenderer::MaterializationRenderItem PianoRollComponent::buildMaterializationRenderItem(
    const TimelineMaterializationPlacement& placement,
    double visibleTimeStart,
    double visibleTimeEnd,
    int viewportStartX,
    int viewportEndX) const
{
    PianoRollRenderer::MaterializationRenderItem item;
    item.materializationId = placement.materializationId;
    item.projection = placement.projection;
    item.active = placement.materializationId == editedMaterializationId_;

    std::shared_ptr<PitchCurve> curve;
    if (item.active) {
        curve = currentCurve_;
        item.audioBuffer = audioBuffer_;
        item.displayNotes = getDisplayedNotes();
    } else if (processor_ != nullptr) {
        curve = processor_->getMaterializationPitchCurveById(placement.materializationId);
        item.audioBuffer = processor_->getMaterializationAudioBufferById(placement.materializationId);
        item.displayNotes = processor_->getMaterializationNotesById(placement.materializationId);
    }

    if (curve != nullptr) {
        item.pitchSnapshot = curve->getSnapshot();
        if (item.pitchSnapshot != nullptr && item.pitchSnapshot->size() > 0)
            item.f0Timeline = { item.pitchSnapshot->getHopSize(),
                                item.pitchSnapshot->getSampleRate(),
                                static_cast<int>(item.pitchSnapshot->size()) };

        if (item.pitchSnapshot != nullptr && !item.f0Timeline.isEmpty()) {
            const double visibleMaterializationStart = juce::jlimit(0.0,
                                                                    placement.projection.materializationDurationSeconds,
                                                                    visibleTimeStart - placement.projection.timelineStartSeconds);
            const double visibleMaterializationEnd = juce::jlimit(visibleMaterializationStart,
                                                                  placement.projection.materializationDurationSeconds,
                                                                  visibleTimeEnd - placement.projection.timelineStartSeconds);
            const auto visibleFrames = item.f0Timeline.rangeForTimesWithMargin(visibleMaterializationStart,
                                                                               visibleMaterializationEnd,
                                                                               10);
            const int startFrame = juce::jlimit(0,
                                                static_cast<int>(item.pitchSnapshot->size()),
                                                visibleFrames.startFrame);
            const int endFrame = juce::jlimit(startFrame,
                                              static_cast<int>(item.pitchSnapshot->size()),
                                              visibleFrames.endFrameExclusive);

            double secondsPerFrame = 0.01;
            if (item.f0Timeline.endFrameExclusive() > 1)
                secondsPerFrame = item.f0Timeline.timeAtFrame(1) - item.f0Timeline.timeAtFrame(0);

            PianoRollRenderer::F0VisualBuildOptions visualOptions;
            visualOptions.startFrame = startFrame;
            visualOptions.endFrameExclusive = endFrame;
            visualOptions.viewportStartX = viewportStartX;
            visualOptions.viewportEndX = viewportEndX;
            visualOptions.pixelsPerSecond = getTimelinePixelsPerSecond();
            visualOptions.secondsPerFrame = secondsPerFrame;

            const auto& originalF0 = item.pitchSnapshot->getOriginalF0();
            const auto& originalEnergy = item.pitchSnapshot->getOriginalEnergy();
            item.originalF0VisualSegments = PianoRollRenderer::buildF0VisualSegments(
                originalF0,
                &originalEnergy,
                nullptr,
                visualOptions,
                [&item, this](int frame) -> float {
                    return static_cast<float>(timeToX(
                        item.projection.timelineStartSeconds + item.f0Timeline.timeAtFrame(frame)));
                },
                [this](int, float frequency) -> float {
                    return midiToY(freqToMidi(frequency));
                });

            if (item.pitchSnapshot->hasAnyCorrection()) {
                item.correctedF0.assign(item.pitchSnapshot->size(), 0.0f);
                item.pitchSnapshot->renderCorrectedOnlyRange(
                    startFrame,
                    endFrame,
                    [&item](int offsetFrame, const float* data, int length) {
                        std::copy(data, data + length, item.correctedF0.begin() + offsetFrame);
                    });

                item.correctedF0VisualSegments = PianoRollRenderer::buildF0VisualSegments(
                    item.correctedF0,
                    &originalEnergy,
                    nullptr,
                    visualOptions,
                    [&item, this](int frame) -> float {
                        return static_cast<float>(timeToX(
                            item.projection.timelineStartSeconds + item.f0Timeline.timeAtFrame(frame)));
                    },
                    [this](int, float frequency) -> float {
                        return midiToY(freqToMidi(frequency));
                    });
            }
        }
    }

    if (item.audioBuffer != nullptr) {
        auto& mipmap = waveformMipmapCache_.getOrCreate(placement.materializationId);
        if (mipmap.isSourceChanged(item.audioBuffer))
            mipmap.setAudioSource(item.audioBuffer);
        item.waveformMipmap = &mipmap;
    }

    if (showChunkBoundaries_ && processor_ != nullptr)
        processor_->getMaterializationChunkBoundariesById(placement.materializationId, item.chunkBoundaries);

    return item;
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

void PianoRollComponent::setReferenceOverlay(std::optional<PianoRollRenderer::ReferenceOverlay> overlay)
{
    referenceOverlay_ = std::move(overlay);
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content),
                     PianoRollVisualInvalidationPriority::Interactive);
}

PianoRollRenderer::RenderContext PianoRollComponent::buildRenderContext(double visibleTimeStart,
                                                                        double visibleTimeEnd,
                                                                        int viewportStartX,
                                                                        int viewportEndX) const
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
    ctx.scaleRootNote = scaleRootNote_;
    ctx.scaleType = scaleType_;
    ctx.noteNameMode = noteNameMode_;
    ctx.showLanes = showLanes_;
    ctx.showChunkBoundaries = showChunkBoundaries_;
    ctx.showUnvoicedFrames = showUnvoicedFrames_;
    ctx.timeUnit = (timeUnit_ == TimeUnit::Bars)
        ? PianoRollRenderer::RenderContext::TimeUnit::Bars
        : PianoRollRenderer::RenderContext::TimeUnit::Seconds;

    const float snapshotPixelsPerSemitone = pixelsPerSemitone_;
    const float snapshotVerticalScrollOffset = verticalScrollOffset_;
    const float snapshotMaxMidi = maxMidi_;

    ctx.materializations.reserve(timelineMaterializationPlacements_.size());
    for (const auto& placement : timelineMaterializationPlacements_)
        if (placement.isValid())
            ctx.materializations.push_back(buildMaterializationRenderItem(placement,
                                                                         visibleTimeStart,
                                                                         visibleTimeEnd,
                                                                         viewportStartX,
                                                                         viewportEndX));

    ctx.midiToY = [snapshotPixelsPerSemitone, snapshotVerticalScrollOffset, snapshotMaxMidi](float midi) {
        return (snapshotMaxMidi - midi) * snapshotPixelsPerSemitone - snapshotVerticalScrollOffset;
    };
    ctx.freqToY = [snapshotPixelsPerSemitone, snapshotVerticalScrollOffset, snapshotMaxMidi](float freq) {
        if (freq <= 0.0f) {
            return (snapshotMaxMidi * snapshotPixelsPerSemitone) - snapshotVerticalScrollOffset;
        }
        const float midi = 12.0f * std::log2(freq / 440.0f) + 69.0f - 0.5f;
        return (snapshotMaxMidi - midi) * snapshotPixelsPerSemitone - snapshotVerticalScrollOffset;
    };
    ctx.freqToMidi = [](float freq) {
        if (freq <= 0.0f) {
            return 0.0f;
        }
        return 12.0f * std::log2(freq / 440.0f) + 69.0f - 0.5f;
    };
    ctx.xToTime = [this](int x) { return xToTime(x); };
    ctx.timeToX = [this](double seconds) { return timeToX(seconds); };

    ctx.hasF0Selection = interactionState_.selection.hasF0Selection;
    ctx.f0SelectionStartFrame = interactionState_.selection.selectedF0StartFrame;
    ctx.f0SelectionEndFrameExclusive = interactionState_.selection.selectedF0EndFrameExclusive;

    // ⚡️ vocal-time-stretch §8.7 — inject TimeGrid snapshot for §8.5 renderer.
    // During an active drag, prefer the working snapshot for live preview;
    // otherwise pull from the processor's published TimeGrid.
    if (interactionState_.timeTool.isDraggingHandle
        && interactionState_.timeTool.dragWorkingSnapshot != nullptr) {
        ctx.timeGridSnapshot = interactionState_.timeTool.dragWorkingSnapshot;
    } else if (processor_ != nullptr && editedMaterializationId_ != 0) {
        ctx.timeGridSnapshot = processor_->getMaterializationTimeGridById(editedMaterializationId_);
    }
    ctx.timeGridHoveredHandleId  = interactionState_.timeTool.hoveredHandleId;
    ctx.timeGridSelectedHandleId = interactionState_.timeTool.selectedHandleId;
    // §8.5 (Phase J) — currentTool drives view-mode in renderer.
    ctx.currentTool = currentTool_;

    // §8.5 (Phase I): TimeGrid handles 使用 materialization-local
    // output_seconds，需要投影到 timeline time 再通过 timeToX 转屏幕坐标。
    ctx.materializationTimeToTimeline = [this](double materializationSeconds) {
        return projectMaterializationTimeToTimeline(materializationSeconds);
    };

    ctx.referenceOverlay = referenceOverlay_;

    return ctx;
}

void PianoRollComponent::prepareVisibleRenderModel() const {
    const auto timelineViewportBounds = getTimelineViewportBounds();
    const int viewportStartX = pianoKeyWidth_;
    const int viewportEndX = timelineViewportBounds.getRight();
    const double visibleTimeStart = xToTime(viewportStartX);
    const double visibleTimeEnd = xToTime(viewportEndX);

    uint64_t placementRevision = 1469598103934665603ull;
    for (const auto& placement : timelineMaterializationPlacements_) {
        placementRevision = hashCombine(placementRevision, placement.materializationId);
        placementRevision = hashCombine(placementRevision, static_cast<uint64_t>(secondsToMs(placement.projection.timelineStartSeconds)));
        placementRevision = hashCombine(placementRevision, static_cast<uint64_t>(secondsToMs(placement.projection.timelineDurationSeconds)));
        placementRevision = hashCombine(placementRevision, static_cast<uint64_t>(secondsToMs(placement.projection.materializationDurationSeconds)));
    }

    PianoRollRenderModelCache::Key cacheKey;
    cacheKey.materializationId = editedMaterializationId_;
    cacheKey.pitchEpoch = editedMaterializationEpoch_.load(std::memory_order_relaxed);
    cacheKey.notesEpoch = editedMaterializationEpoch_.load(std::memory_order_relaxed);
    cacheKey.visualPrefsRevision = visualPrefsRevision_;
    cacheKey.timeGridRevision = (processor_ != nullptr && editedMaterializationId_ != 0)
        ? processor_->getMaterializationTimeGridRevisionById(editedMaterializationId_)
        : 0;
    cacheKey.visibleStartMs = secondsToMs(visibleTimeStart);
    cacheKey.visibleEndMs = secondsToMs(visibleTimeEnd);
    const auto projection = activeMaterializationProjection();
    cacheKey.projectionStartMs = secondsToMs(projection.timelineStartSeconds);
    cacheKey.projectionDurationMs = secondsToMs(projection.timelineDurationSeconds);
    cacheKey.placementProjectionRevision = placementRevision;
    cacheKey.zoomBucket = static_cast<int>(zoomLevel_ * 100.0 + 0.5);
    cacheKey.verticalZoomBucket = quantizeGeometryPx(pixelsPerSemitone_);
    cacheKey.verticalScrollBucket = quantizeGeometryPx(verticalScrollOffset_);
    cacheKey.viewportSizeRevision = viewportSizeRevision_;
    if (!renderModelCache_.isValid() || renderModelCache_.getCurrentKey() != cacheKey) {
        auto ctx = buildRenderContext(visibleTimeStart, visibleTimeEnd, viewportStartX, viewportEndX);
        renderModelCache_.rebuild(cacheKey, std::move(ctx));
        FrameScheduler::instance().recordRenderModelRebuild(FrameScheduler::TimelineReason::ContentModelInvalid);
    }
}

void PianoRollComponent::refreshVerticalViewportGeometry(PianoRollVisualInvalidationPriority priority)
{
    updateScrollBars();
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport), priority);
}

void PianoRollComponent::setScale(int rootNote, int scaleType)
{
    scaleRootNote_ = juce::jlimit(0, 11, rootNote);
    scaleType_ = juce::jlimit(1, 8, scaleType);
    ++visualPrefsRevision_;
    prepareVisibleRenderModel();
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
        refreshVerticalViewportGeometry();
    }

    // 2. Horizontal Fit:
    // If has audio: fit audio length
    // If no audio: fit 16 seconds
    double duration = 16.0;
    const auto activeProjection = activeMaterializationProjection();
    const bool hasProjectedClipTimeline = activeProjection.isValid();
    if (hasProjectedClipTimeline) {
        duration = activeProjection.timelineDurationSeconds;
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
        const auto projectedStartPixels = static_cast<int>(std::llround(
            toVisibleTimelineSeconds(activeProjection.timelineStartSeconds) * getTimelinePixelsPerSecond()));
        setScrollOffset(projectedStartPixels);
    } else if (audioBuffer_ && audioBufferSampleRate_ > 0.0) {
        int newScroll = (int) std::llround(toVisibleTimelineSeconds(timelineViewOriginSeconds()) * getTimelinePixelsPerSecond());
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
    return absoluteSeconds - timelineViewOriginSeconds();
}

double PianoRollComponent::toAbsoluteTimelineSeconds(double visibleSeconds) const {
    return visibleSeconds + timelineViewOriginSeconds();
}

double PianoRollComponent::timelineViewOriginSeconds() const noexcept
{
    if (timelineViewDomain_.isValid()) {
        return timelineViewDomain_.startSeconds;
    }

    const auto projection = activeMaterializationProjection();
    return projection.isValid() ? projection.timelineStartSeconds : 0.0;
}

double PianoRollComponent::timelineViewEndSeconds() const noexcept
{
    if (timelineViewDomain_.isValid()) {
        return timelineViewDomain_.endSeconds;
    }

    const auto projection = activeMaterializationProjection();
    return projection.isValid() ? projection.timelineEndSeconds() : 0.0;
}

bool PianoRollComponent::hasExplicitTimelineViewDomain() const noexcept
{
    return timelineViewDomain_.isValid();
}

double PianoRollComponent::projectTimelineTimeToMaterialization(double timelineSeconds) const {
    const auto projection = activeMaterializationProjection();
    return projection.isValid()
        ? projection.projectTimelineTimeToMaterialization(timelineSeconds)
        : timelineSeconds;
}

double PianoRollComponent::projectMaterializationTimeToTimeline(double contentSeconds) const {
    const auto projection = activeMaterializationProjection();
    return projection.isValid()
        ? projection.projectMaterializationTimeToTimeline(contentSeconds)
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
        + " duration=" + juce::String(getMaterializationDurationSeconds()));

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

    // Build scale-snap config separately from generation params. The snap is
    // applied post-generation by the worker; note generation itself stays
    // chromatic so the two responsibilities (segmentation vs scale theory)
    // do not bleed into each other.
    std::optional<ScaleSnapConfig> postSnapCfg;
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
        postSnapCfg = snapCfg;
    }

    auto request = std::make_shared<PianoRollCorrectionWorker::AsyncCorrectionRequest>();
    request->kind = PianoRollCorrectionWorker::AsyncCorrectionRequest::Kind::AutoTuneGenerate;
    request->startFrame = startFrame;
    request->endFrameExclusive = endFrameExclusive;
    request->retuneSpeed = currentRetuneSpeed_;
    request->vibratoDepth = currentVibratoDepth_;
    request->vibratoRate = currentVibratoRate_;
    request->postSnap = postSnapCfg;
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

void PianoRollComponent::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) {
    if (scrollBar == &horizontalScrollBar_) {
        setScrollOffset(static_cast<int>(newRangeStart));
        userScrollHold_ = true;
    } else if (scrollBar == &verticalScrollBar_) {
        verticalScrollOffset_ = static_cast<float>(newRangeStart);
        refreshVerticalViewportGeometry(PianoRollVisualInvalidationPriority::Normal);
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
    
    double contentEndSeconds = timelineViewEndSeconds();
    for (const auto& placement : timelineMaterializationPlacements_) {
        if (placement.isValid()) {
            contentEndSeconds = std::max(contentEndSeconds, placement.projection.timelineEndSeconds());
        }
    }
    if (contentEndSeconds <= timelineViewOriginSeconds()) {
        contentEndSeconds = timelineViewOriginSeconds() + maxTime;
    }
    maxTime = contentEndSeconds - timelineViewOriginSeconds();

    maxTime = std::max(maxTime, 10.0);
    maxTime += 5.0;
    
    double pixelsPerSecond = getTimelinePixelsPerSecond();
    int totalContentWidth = static_cast<int>(maxTime * pixelsPerSecond);
    int visibleWidth = getWidth() - pianoKeyWidth_ - UIColors::scrollBarThickness;
    visibleWidth = juce::jmax(1, visibleWidth);
    
    horizontalScrollBar_.setRangeLimits(0.0, totalContentWidth + visibleWidth, juce::dontSendNotification);
    horizontalScrollBar_.setCurrentRange(scrollOffset_, visibleWidth, juce::dontSendNotification);
    
    // Vertical
    float totalHeight = getTotalHeight();
    int visibleHeight = getHeight() - rulerHeight_ - UIColors::scrollBarThickness;
    visibleHeight = juce::jmax(1, visibleHeight);
    
    verticalScrollBar_.setRangeLimits(0.0, totalHeight, juce::dontSendNotification);
    verticalScrollBar_.setCurrentRange(verticalScrollOffset_, visibleHeight, juce::dontSendNotification);
}

} // namespace OpenTune
