#include "PianoRollComponent.h"
#include "../../Utils/LocalizationManager.h"
#include "../Utils/AppLogger.h"
#include "../../Utils/PianoRollEditAction.h"
#include "../../Utils/PianoRollNotePatchAction.h"
#include "../../Utils/TimeGridEditAction.h"   // 鈿★?vocal-time-stretch ?.7
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include "../DSP/ChromaKeyDetector.h"
#include "../Utils/LegacyNoteGenerator.h"
#include "../Utils/SimdPerceptualPitchEstimator.h"
#include "../Utils/ZoomSensitivityConfig.h"
#include "../../PluginProcessor.h"
#include "UiAssets.h"
#include "UiText.h"
#include "ToolbarIcons.h"
#include "../../Utils/AudioEditingScheme.h"
#include "Utils/PianoKeyAudition.h"
#include "TimelineViewportPolicy.h"
namespace OpenTune {

namespace {

std::vector<PitchCorrectionSegment> copyPitchCorrectionSegments(const std::shared_ptr<PitchCurve>& curve)
{
    std::vector<PitchCorrectionSegment> copiedSegments;
    if (curve == nullptr) {
        return copiedSegments;
    }

    const auto snapshot = curve->getSnapshot();
    copiedSegments.reserve(snapshot->getCorrectionSegments().size());
    for (const auto& segment : snapshot->getCorrectionSegments()) {
        copiedSegments.push_back(segment);
    }
    return copiedSegments;
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

uint64_t hashCombineContentKey(uint64_t seed, ContentKey key) noexcept
{
    seed = hashCombine(seed, static_cast<uint64_t>(key.domainKind));
    seed = hashCombine(seed, key.objectId);
    return hashCombine(seed, key.sourceWindowDiscriminator);
}

constexpr double kPianoRollPinnedViewportRatio = 0.5;

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
            setTimeUnit(TimeUnit::Bars);
            timeUnitToggleButton_.setButtonText("BPM");
        } else {
            setTimeUnit(TimeUnit::Seconds);
            timeUnitToggleButton_.setButtonText("Time");
        }
    };
    addAndMakeVisible(timeUnitToggleButton_);
    timeUnitToggleButton_.setTooltip(LOC(kTooltipTimeUnit));

    addAndMakeVisible(previewOverlay_);
    addAndMakeVisible(fixedPlayhead_);
    timeUnitToggleButton_.toFront(false);
    scrollModeToggleButton_.toFront(false);
    fixedPlayhead_.setColour(playheadColour_);

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

    toolCtx.getViewMapper = [this]() -> ViewMapper { return makeViewMapper(); };

    toolCtx.getCommittedNotes = [this]() -> const std::vector<Note>& { return getCommittedNotes(); };
    toolCtx.getDisplayNotes = [this]() -> const std::vector<Note>& { return getDisplayedNotes(); };
    toolCtx.getNoteDraft = [this]() -> NoteInteractionDraft& { return getNoteDraft(); };
    toolCtx.beginNoteDraft = [this]() { beginNoteDraft(); };
    toolCtx.commitNoteDraft = [this]() { return commitNoteDraft(); };
    toolCtx.clearNoteDraft = [this]() { clearNoteDraft(); };
    toolCtx.commitNotesAndSegments = [this](const std::vector<Note>& notes,
                                            const std::vector<PitchCorrectionSegment>& segments,
                                            F0FrameRange affectedRange) {
        return commitEditedContentNotesAndSegments(notes, segments, affectedRange);
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
        if (experimentalFeaturesEnabled_) {
            menu.addItem("Time Tool (T)", [this]() { setCurrentTool(ToolId::TimeTool); });
        }
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
    toolCtx.getContentProjection = [this]() { return activeContentProjection(); };
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

    toolCtx.getNoteDragManualStartFrame = [this]() { return interactionState_.noteDrag.manualStartFrame; };
    toolCtx.setNoteDragManualStartFrame = [this](int v) { interactionState_.noteDrag.manualStartFrame = v; };
    toolCtx.getNoteDragManualEndFrameExclusive = [this]() { return interactionState_.noteDrag.manualEndFrameExclusive; };
    toolCtx.setNoteDragManualEndFrameExclusive = [this](int v) { interactionState_.noteDrag.manualEndFrameExclusive = v; };
    toolCtx.getNoteDragInitialManualTargets = [this]() -> std::vector<NoteDragManualTarget>& { return interactionState_.noteDrag.initialManualTargets; };
    toolCtx.getNoteDragPreviewF0 = [this]() -> std::vector<float>& { return interactionState_.noteDrag.previewF0; };
    toolCtx.getNoteDragPreviewStartFrame = [this]() { return interactionState_.noteDrag.previewStartFrame; };
    toolCtx.setNoteDragPreviewStartFrame = [this](int v) { interactionState_.noteDrag.previewStartFrame = v; };
    toolCtx.getNoteDragPreviewEndFrameExclusive = [this]() { return interactionState_.noteDrag.previewEndFrameExclusive; };
    toolCtx.setNoteDragPreviewEndFrameExclusive = [this](int v) { interactionState_.noteDrag.previewEndFrameExclusive = v; };

    toolCtx.invalidateVisual = [this](const juce::Rectangle<int>& dirtyArea) {
        invalidateInteractionArea(dirtyArea);
    };
    toolCtx.invalidateInteractionVisual = [this]() {
        repaint();
    };
    toolCtx.repaintPreviewOverlay = [this]() { previewOverlay_.repaint(); };
    toolCtx.setMouseCursor = [this](const juce::MouseCursor& c) { setMouseCursor(c); };
    toolCtx.grabKeyboardFocus = [this]() { grabKeyboardFocus(); };
    toolCtx.getAudioEditingScheme = [this]() { return audioEditingScheme_; };
    toolCtx.notifyPlayheadChange = [this](double time) {
        listeners_.call([time](Listener& l) { l.playheadPositionChangeRequested(time); });
        userScrollHold_ = false;
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
    // 鈿★�?vocal-time-stretch �?.7 �?Time tool / TimeGrid wiring
    // ============================================================
    toolCtx.getTimeGridSnapshot = [this]() -> std::shared_ptr<const TimeGridSnapshot> {
        auto snap = readEditedSnapshot();
        return snap ? snap->timeGrid : nullptr;
    };
    toolCtx.commitTimeGrid = [this](std::shared_ptr<const TimeGridSnapshot> newSnap,
                                    std::shared_ptr<const TimeGridSnapshot> oldSnap,
                                    juce::String description) -> bool {
        if (processor_ == nullptr || !editedContentKey_.isValid()) return false;
        if (newSnap == nullptr || oldSnap == nullptr) return false;

        auto action = std::make_unique<TimeGridEditAction>(
            contentCommands_,
            editedContentKey_,
            description.isNotEmpty() ? description : juce::String("编辑时间网格"),
            std::move(oldSnap),
            newSnap);
        const bool published = contentCommands_->setTimeGrid(
            editedContentKey_, newSnap);
        if (!published) return false;
        processor_->getUndoManager().addAction(std::move(action));
        repaint();
        repaint();
        return true;
    };
    toolCtx.repaintTimeGridHandles = [this]() {
        repaint();
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

    repaint();
}

bool PianoRollComponent::commitCompletedAutoTuneResult(const PianoRollCorrectionWorker::AsyncCorrectionRequest& completed)
{
    AppLogger::log("AutoTune: commitCompletedAutoTuneResult entry");

    if (processor_ == nullptr || !editedContentKey_.isValid()) {
        AppLogger::log("AutoTune: commitCompleted abort - processor or content key invalid");
        return false;
    }

    const uint64_t currentEpoch = editedContentEpoch_.load(std::memory_order_acquire);
    if (completed.contentKeySnapshot != editedContentKey_
        || completed.contentEpochSnapshot != currentEpoch) {
        AppLogger::log("AutoTune: commitCompleted abort - epoch mismatch (snapshot="
            + juce::String(completed.contentEpochSnapshot) + " current=" + juce::String(currentEpoch) + ")");
        return false;
    }

    AppLogger::log("AutoTune: commitCompleted calling commitAutoTuneGeneratedNotes noteCount="
        + juce::String(static_cast<int>(completed.notes.size())));

    if (!contentCommands_->commitAutoTuneGeneratedNotes(
            editedContentKey_,
            completed.notes,
            completed.autoStartFrame,
            completed.autoEndFrame + 1,
            completed.retuneSpeed,
            completed.vibratoDepth,
            completed.vibratoRate)) {
        AppLogger::log("AutoTune: commitAutoTuneGeneratedNotes returned false");
        return false;
    }

    AppLogger::log("AutoTune: commitAutoTuneGeneratedNotes succeeded, refreshing notes");
    refreshEditedContentNotes();

    auto committedCurve = readEditedSnapshot();
    if (committedCurve == nullptr || committedCurve->pitchCurve == nullptr) {
        AppLogger::log("AutoTune: commitCompleted abort - committedCurve null after commit");
        return false;
    }

    AppLogger::log("AutoTune: setEditedContent + recordUndo");
    setEditedContent(editedContentKey_, committedCurve->pitchCurve, audioBuffer_, static_cast<int>(audioBufferSampleRate_));
    AppLogger::log("AutoTune: after setEditedContent");
    updateScrollBars();
    AppLogger::log("AutoTune: after outer updateScrollBars");
    repaint();
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
    if (processor_ == nullptr || !editedContentKey_.isValid() || completed.curve == nullptr) {
        return false;
    }

    const uint64_t currentEpoch = editedContentEpoch_.load(std::memory_order_acquire);
    if (completed.contentKeySnapshot != editedContentKey_
        || completed.contentEpochSnapshot != currentEpoch) {
        return false;
    }

    const auto correctionRange = PitchCurve::expandNoteBasedCorrectionRange(
        completed.startFrame,
        completed.endFrameExclusive,
        currentF0Timeline().endFrameExclusive());
    if (!commitEditedContentPitchCorrectionSegments(copyPitchCorrectionSegments(completed.curve),
                                                       correctionRange)) {
        return false;
    }

    updateScrollBars();
    repaint();
    return true;
}

void PianoRollComponent::setProcessor(OpenTuneAudioProcessor* processor)
{
    processor_ = processor;
    refreshEditedContentNotes();
}

void PianoRollComponent::setContentCommands(std::shared_ptr<ContentEditCommands> commands)
{
    contentCommands_ = std::move(commands);
}


void PianoRollComponent::refreshEditedContentNotes()
{
    cachedNotes_.clear();

    if (processor_ != nullptr && editedContentKey_.isValid()) {
        if (auto snap = readEditedSnapshot()) {
            cachedNotes_ = snap->notes;
        }
    }
    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
    syncF0SelectionToSelectedNotes();
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
    interactionState_.noteDraft.contentDirty = false;
    interactionState_.noteDraft.baselineNotes = cachedNotes_;
    interactionState_.noteDraft.workingNotes = cachedNotes_;
}

bool PianoRollComponent::commitNoteDraft()
{
    if (!interactionState_.noteDraft.active) {
        return true;
    }

    if (!interactionState_.noteDraft.contentDirty) {
        interactionState_.noteDraft.clear();
        pendingUndoDescription_ = {};
        undoSnapshotCaptured_ = false;
        return true;
    }

    if (processor_ == nullptr || !editedContentKey_.isValid()) {
        return false;
    }

    // Build ContentNoteRangePatch via merge-based diff (content-based, not index-based)
    const auto& baseline = interactionState_.noteDraft.baselineNotes;
    const auto& working = interactionState_.noteDraft.workingNotes;

    ContentNoteRangePatch patch;
    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;

    // Notes are sorted by startTime. Walk both arrays simultaneously.
    // When startTime matches �?same note, compare content.
    // When startTime differs �?deletion or insertion.
    auto notesContentEqual = [](const Note& a, const Note& b) {
        return a.endTime == b.endTime
            && a.pitch == b.pitch
            && a.pitchOffset == b.pitchOffset
            && a.retuneSpeed == b.retuneSpeed
            && a.vibratoDepth == b.vibratoDepth
            && a.vibratoRate == b.vibratoRate;
    };

    size_t i = 0, j = 0;
    while (i < baseline.size() || j < working.size()) {
        const bool bHas = i < baseline.size();
        const bool wHas = j < working.size();

        if (bHas && wHas && baseline[i].startTime == working[j].startTime) {
            // Same position �?compare content for modification
            if (!notesContentEqual(baseline[i], working[j])) {
                dirtyStartTime = std::min(dirtyStartTime, baseline[i].startTime);
                dirtyEndTime = std::max(dirtyEndTime, baseline[i].endTime);
            }
            i++; j++;
        } else if (!wHas || (bHas && baseline[i].startTime < working[j].startTime)) {
            // Baseline note at earlier position was deleted
            dirtyStartTime = std::min(dirtyStartTime, baseline[i].startTime);
            dirtyEndTime = std::max(dirtyEndTime, baseline[i].endTime);
            i++;
        } else {
            // Working note at earlier position was inserted
            dirtyStartTime = std::min(dirtyStartTime, working[j].startTime);
            dirtyEndTime = std::max(dirtyEndTime, working[j].endTime);
            j++;
        }
    }

    // No actual changes �?skip commit, not a failure
    if (dirtyEndTime <= dirtyStartTime) {
        interactionState_.noteDraft.clear();
        return true;
    }

    patch.affectedRange.startSeconds = dirtyStartTime;
    patch.affectedRange.endSeconds = dirtyEndTime;

    // Extract after notes overlapping the dirty time range
    auto overlapsRange = [dirtyStartTime, dirtyEndTime](const Note& n) {
        return n.endTime > dirtyStartTime && n.startTime < dirtyEndTime;
    };
    for (const auto& n : working) {
        if (overlapsRange(n)) patch.afterNotesInRange.push_back(n);
    }

    if (!contentCommands_->commitNotePatch(editedContentKey_, patch)) {
        return false;
    }

    refreshEditedContentNotes();

    // Build the before-patch from baseline notes in the same seconds range.
    // Note-only undo uses seconds-based PianoRollNotePatchAction �?no frame
    // conversion, no segment involvement, same coordinate system as commitNotePatch().
    ContentNoteRangePatch beforePatch;
    beforePatch.affectedRange = patch.affectedRange;
    for (const auto& n : baseline) {
        if (overlapsRange(n)) beforePatch.afterNotesInRange.push_back(n);
    }

    auto action = std::make_unique<PianoRollNotePatchAction>(
        contentCommands_,
        editedContentKey_,
        pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : TRANS("编辑"),
        std::move(beforePatch),
        std::move(patch));

    if (processor_ != nullptr)
        processor_->getUndoManager().addAction(std::move(action));

    pendingUndoDescription_ = {};
    undoSnapshotCaptured_ = false;
    interactionState_.noteDraft.clear();
    return true;
}

void PianoRollComponent::clearNoteDraft()
{
    interactionState_.noteDraft.clear();
}

ContentCommitSnapshot PianoRollComponent::commitEditedContentPitchCorrectionSegments(const std::vector<PitchCorrectionSegment>& segments,
                                                                         F0FrameRange affectedRange)
{
    // Delegate to the range-scoped merge path.  setPitchCorrectionSegments does
    // full replacement which would discard segments outside affectedRange.
    // commitEditedContentNotesAndSegments �?commitContentNotesAndSegments
    // performs range-scoped merge (keptBefore + incoming + keptAfter).
    return commitEditedContentNotesAndSegments(cachedNotes_, segments, affectedRange);
}

ContentCommitSnapshot PianoRollComponent::commitEditedContentNotesAndSegments(const std::vector<Note>& notes,
                                                               const std::vector<PitchCorrectionSegment>& segments,
                                                               F0FrameRange affectedRange)
{
    if (processor_ == nullptr || !editedContentKey_.isValid()) {
        return {};
    }

    // Capture range-scoped before data directly �?no full snapshot.
    const auto f0tl = currentF0Timeline();
    const double rangeStartSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(affectedRange.startFrame);
    const double rangeEndSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(affectedRange.endFrameExclusive);

    auto extractNotesInRange = [](const std::vector<Note>& notes, double startSec, double endSec) {
        std::vector<Note> result;
        for (const auto& note : notes) {
            if (note.startTime < endSec && note.endTime > startSec)
                result.push_back(note);
        }
        return result;
    };

    auto extractSegmentsInRange = [](const std::vector<PitchCorrectionSegment>& segs, int startFrame, int endFrame) {
        std::vector<PitchCorrectionSegment> result;
        for (const auto& seg : segs) {
            if (seg.endFrame <= startFrame || seg.startFrame >= endFrame)
                continue;  // Outside range
            
            // Clip to range boundaries (split-preserve for boundary-crossing segments)
            const int clipStart = std::max(seg.startFrame, startFrame);
            const int clipEnd = std::min(seg.endFrame, endFrame);
            if (clipEnd <= clipStart)
                continue;  // Empty after clip
            
            PitchCorrectionSegment clipped = seg;
            const int startOffset = clipStart - seg.startFrame;
            const int clipLen = clipEnd - clipStart;
            if (startOffset >= 0 && clipLen > 0 && startOffset + clipLen <= static_cast<int>(seg.f0Data.size())) {
                clipped.startFrame = clipStart;
                clipped.endFrame = clipEnd;
                clipped.f0Data.assign(seg.f0Data.begin() + startOffset, seg.f0Data.begin() + startOffset + clipLen);
                result.push_back(std::move(clipped));
            }
        }
        return result;
    };

    auto beforeNotes = extractNotesInRange(cachedNotes_, rangeStartSec, rangeEndSec);
    auto beforeSegments = extractSegmentsInRange(getCurrentSegments(), affectedRange.startFrame, affectedRange.endFrameExclusive);

    // Enforce range-scoped contract: filter incoming data so sink never
    // receives notes/segments outside the affected range.
    auto scopedNotes = extractNotesInRange(notes, rangeStartSec, rangeEndSec);
    auto scopedSegments = extractSegmentsInRange(segments, affectedRange.startFrame, affectedRange.endFrameExclusive);

    ContentEditRangeFrames editRange;
    editRange.startFrame = affectedRange.startFrame;
    editRange.endFrameExclusive = affectedRange.endFrameExclusive;

    const auto committedSnap =
        contentCommands_->commitNotesAndSegments(editedContentKey_,
                                                 std::move(scopedNotes),
                                                 std::move(scopedSegments),
                                                 editRange);
    if (!committedSnap) {
        return {};
    }

    // Update local caches from committed state
    cachedNotes_ = committedSnap->notes;
    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
    syncF0SelectionToSelectedNotes();

    if (committedSnap->pitchCurve) {
        applyEditedContentCurve(committedSnap->pitchCurve);
    }

    // Capture range-scoped after data from committed snapshot
    auto afterNotes = extractNotesInRange(committedSnap->notes, rangeStartSec, rangeEndSec);
    auto afterSegments = extractSegmentsInRange(
        committedSnap->correctionSegments,
        affectedRange.startFrame,
        affectedRange.endFrameExclusive);

    auto action = std::make_unique<PianoRollEditAction>(
        contentCommands_,
        editedContentKey_,
        pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : TRANS("编辑"),
        std::move(beforeNotes),
        std::move(afterNotes),
        std::move(beforeSegments),
        std::move(afterSegments),
        ContentEditRangeFrames{affectedRange.startFrame, affectedRange.endFrameExclusive});

    processor_->getUndoManager().addAction(std::move(action));
    pendingUndoDescription_ = {};
    undoSnapshotCaptured_ = false;
    return committedSnap;
}

std::vector<PitchCorrectionSegment> PianoRollComponent::getCurrentSegments() const
{
    if (!currentCurve_) return {};
    auto snap = currentCurve_->getSnapshot();
    if (!snap) return {};
    return snap->getCorrectionSegments();
}

void PianoRollComponent::captureBeforeUndoSnapshot()
{
    beforeUndoNotes_ = cachedNotes_;
    beforeUndoSegments_ = getCurrentSegments();
    undoSnapshotCaptured_ = true;
}

void PianoRollComponent::recordUndoAction(const juce::String& description, F0FrameRange affectedRange)
{
    if (processor_ == nullptr || !editedContentKey_.isValid() || !undoSnapshotCaptured_)
        return;

    AppLogger::log("AutoTune: recordUndoAction entry beforeNotes=" + juce::String(static_cast<int>(beforeUndoNotes_.size()))
        + " beforeSegments=" + juce::String(static_cast<int>(beforeUndoSegments_.size()))
        + " cachedNotes=" + juce::String(static_cast<int>(cachedNotes_.size()))
        + " affectedRange=[" + juce::String(affectedRange.startFrame)
        + "," + juce::String(affectedRange.endFrameExclusive) + ")");

    // Extract range-scoped notes and segments for memory-efficient undo.
    // Notes use seconds; segments use frames. Convert frame range to seconds.
    const auto f0tl = currentF0Timeline();
    const double rangeStartSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(affectedRange.startFrame);
    const double rangeEndSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(affectedRange.endFrameExclusive);

    auto notesInRange = [](const std::vector<Note>& notes, double startSec, double endSec) {
        std::vector<Note> result;
        for (const auto& note : notes) {
            if (note.startTime < endSec && note.endTime > startSec)
                result.push_back(note);
        }
        return result;
    };

    auto segmentsInRange = [](const std::vector<PitchCorrectionSegment>& segments, int startFrame, int endFrameExclusive) {
        std::vector<PitchCorrectionSegment> result;
        for (const auto& seg : segments) {
            if (seg.endFrame <= startFrame || seg.startFrame >= endFrameExclusive)
                continue;  // Outside range
            
            // Clip to range boundaries (split-preserve for boundary-crossing segments)
            const int clipStart = std::max(seg.startFrame, startFrame);
            const int clipEnd = std::min(seg.endFrame, endFrameExclusive);
            if (clipEnd <= clipStart)
                continue;  // Empty after clip
            
            PitchCorrectionSegment clipped = seg;
            const int startOffset = clipStart - seg.startFrame;
            const int clipLen = clipEnd - clipStart;
            if (startOffset >= 0 && clipLen > 0 && startOffset + clipLen <= static_cast<int>(seg.f0Data.size())) {
                clipped.startFrame = clipStart;
                clipped.endFrame = clipEnd;
                clipped.f0Data.assign(seg.f0Data.begin() + startOffset, seg.f0Data.begin() + startOffset + clipLen);
                result.push_back(std::move(clipped));
            }
        }
        return result;
    };

    auto beforeNotes = notesInRange(beforeUndoNotes_, rangeStartSec, rangeEndSec);
    auto afterNotes = notesInRange(cachedNotes_, rangeStartSec, rangeEndSec);
    auto beforeSegments = segmentsInRange(beforeUndoSegments_, affectedRange.startFrame, affectedRange.endFrameExclusive);
    auto afterSegments = segmentsInRange(getCurrentSegments(), affectedRange.startFrame, affectedRange.endFrameExclusive);

    AppLogger::log("AutoTune: recordUndoAction range-scoped beforeNotes=" + juce::String(static_cast<int>(beforeNotes.size()))
        + " afterNotes=" + juce::String(static_cast<int>(afterNotes.size()))
        + " beforeSegments=" + juce::String(static_cast<int>(beforeSegments.size()))
        + " afterSegments=" + juce::String(static_cast<int>(afterSegments.size())));

    auto action = std::make_unique<PianoRollEditAction>(
        contentCommands_,
        editedContentKey_,
        description.isNotEmpty() ? description : TRANS("编辑"),
        std::move(beforeNotes),
        std::move(afterNotes),
        std::move(beforeSegments),
        std::move(afterSegments),
        ContentEditRangeFrames{affectedRange.startFrame, affectedRange.endFrameExclusive});

    AppLogger::log("AutoTune: recordUndoAction before addAction");
    processor_->getUndoManager().addAction(std::move(action));
    AppLogger::log("AutoTune: recordUndoAction after addAction");
    pendingUndoDescription_ = {};
    undoSnapshotCaptured_ = false;
    beforeUndoNotes_.clear();
    beforeUndoSegments_.clear();
}

bool PianoRollComponent::selectNotesOverlappingFrames(int startFrame, int endFrameExclusive)
{
    const auto& notes = getCommittedNotes();
    const auto f0tl = currentF0Timeline();
    if (notes.empty() || f0tl.isEmpty()) {
        interactionState_.noteSelection.clear();
        interactionState_.selection.hasSelectionArea = false;
        interactionState_.selection.isSelectingArea = false;
        interactionState_.selection.clearF0Selection();
        repaint();
        previewOverlay_.repaint();
        return false;
    }

    const auto selectionRange = f0tl.rangeForFrames(startFrame, endFrameExclusive);

    bool anyOverlap = false;
    std::vector<int> selectedIndices;
    selectedIndices.reserve(notes.size());
    for (int noteIndex = 0; noteIndex < static_cast<int>(notes.size()); ++noteIndex) {
        const auto& note = notes[static_cast<size_t>(noteIndex)];
        const auto noteRange = f0tl.nonEmptyRangeForTimes(note.startTime, note.endTime);
        const bool overlaps = std::min(selectionRange.endFrameExclusive, noteRange.endFrameExclusive)
            > std::max(selectionRange.startFrame, noteRange.startFrame);
        if (overlaps) {
            anyOverlap = true;
            selectedIndices.push_back(noteIndex);
        }
    }

    interactionState_.noteSelection.setFromIndices(std::move(selectedIndices),
                                                   static_cast<int>(notes.size()));
    interactionState_.selection.hasSelectionArea = false;
    interactionState_.selection.isSelectingArea = false;
    if (anyOverlap) {
        interactionState_.selection.setF0Range(selectionRange.startFrame, selectionRange.endFrameExclusive);
    } else {
        interactionState_.selection.clearF0Selection();
    }
    repaint();
    previewOverlay_.repaint();

    return anyOverlap;
}

juce::Rectangle<int> PianoRollComponent::getNoteBounds(const Note& note) const
{
    const float adjustedPitch = note.getAdjustedPitch();
    if (adjustedPitch <= 0.0f) {
        return {};
    }

    const SourceEditRange sourceRange = sourceEditRange();
    if (note.endTime <= sourceRange.startSeconds
        || note.startTime >= sourceRange.endSeconds) {
        return {};
    }

    const int x1 = sourceTimeToX(note.startTime);
    const int x2 = sourceTimeToX(note.endTime);
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

    const int x1 = sourceTimeToX(startTime);
    const int x2 = sourceTimeToX(endTime);
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

    const int x1 = sourceTimeToX(std::min(interactionState_.drawing.dirtyStartTime,
                                          interactionState_.drawing.dirtyEndTime));
    const int x2 = sourceTimeToX(std::max(interactionState_.drawing.dirtyStartTime,
                                          interactionState_.drawing.dirtyEndTime));
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
        includePoint(static_cast<float>(sourceTimeToX(anchor.time)), freqToY(anchor.freq));
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

        const float x = static_cast<float>(sourceTimeToX(f0tl.timeAtFrame(frame)));
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

    if (interactionState_.noteDraft.active) {
        ++interactionRevision_;
    }

    repaint(dirtyArea);
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

    // dirtyStartFrame/dirtyEndFrame 鏄墍鏈?manual ops �?dirty 甯у苟闆嗭紙鍚鐐癸級銆?
    const F0FrameRange affectedRange{dirtyStartFrame,
                                      dirtyEndFrame >= dirtyStartFrame ? dirtyEndFrame + 1 : dirtyStartFrame};
    if (!commitEditedContentPitchCorrectionSegments(copyPitchCorrectionSegments(editedCurve), affectedRange)) {
        return false;
    }

    if (triggerRenderEvent && dirtyEndFrame >= dirtyStartFrame) {
        listeners_.call([dirtyStartFrame, dirtyEndFrame](Listener& l) {
            l.pitchCurveEdited(dirtyStartFrame, dirtyEndFrame);
        });
    }

    repaint();
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
    request->contentEpochSnapshot = editedContentEpoch_.load(std::memory_order_acquire);
    request->contentKeySnapshot = editedContentKey_;
    if (!undoSnapshotCaptured_)
        captureBeforeUndoSnapshot();

    correctionWorker_->enqueue(request);
}

// ============================================================================
// PianoRollPreviewOverlay �?paint transient interaction previews
// ============================================================================

void PianoRollPreviewOverlay::paint(juce::Graphics& g)
{
    // ⚡️ P0-2: Draft notes 绘制 �?绘制正在被拖拽或缩放�?notes
    // 避免 detail cache 旧位�?+ selection highlights 新位�?= 重影
    auto& interaction = owner_.interactionState_;
    
    // 绘制单个 draft note �?lambda
    auto drawDraftNote = [&](const Note& note) {
        auto bounds = owner_.getNoteBounds(note);
        if (bounds.isEmpty()) return;
        
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.fillRect(bounds);
        g.setColour(juce::Colours::white.withAlpha(0.8f));
        g.drawRect(bounds, 1);
    };
    
    // Note drag: 绘制被拖拽的 notes
    if (interaction.noteDraft.active && interaction.noteDrag.isDraggingNotes) {
        const auto& workingNotes = interaction.noteDraft.workingNotes;
        for (int idx : interaction.noteDrag.draggedNoteIndices) {
            if (idx >= 0 && idx < static_cast<int>(workingNotes.size())) {
                drawDraftNote(workingNotes[idx]);
            }
        }
    }
    
    // Note resize: 绘制被缩放的 note
    if (interaction.noteDraft.active && interaction.noteResize.noteIndex >= 0) {
        const auto& workingNotes = interaction.noteDraft.workingNotes;
        int idx = interaction.noteResize.noteIndex;
        if (idx < static_cast<int>(workingNotes.size())) {
            drawDraftNote(workingNotes[idx]);
        }
    }
    
    // Ghost overlay (reference content) �?drawn in overlay layer, not tiles
    if (owner_.referenceOverlay_.has_value() && owner_.referenceOverlay_->enabled) {
        auto ctx = owner_.makePresentationRenderContext();
        owner_.renderer_->drawGhostNotes(g, ctx, *owner_.referenceOverlay_);
        owner_.renderer_->drawGhostAnchors(g, ctx, *owner_.referenceOverlay_);
    }

    const auto themeId = UIColors::currentThemeId();

    if (owner_.currentTool_ != ToolId::TimeTool) {
        if (owner_.currentCurve_ != nullptr) {
            owner_.drawNoteDragCurvePreview(g);
            owner_.drawHandDrawPreview(g);
            owner_.drawLineAnchorPreview(g);
        }
    }

    if (owner_.interactionState_.drawing.isDrawingNote
        && owner_.currentTool_ == ToolId::DrawNote) {
        double startTime = std::min(owner_.interactionState_.drawing.drawingNoteStartTime,
                                    owner_.interactionState_.drawing.drawingNoteEndTime);
        double endTime = std::max(owner_.interactionState_.drawing.drawingNoteStartTime,
                                  owner_.interactionState_.drawing.drawingNoteEndTime);
        float pitch = owner_.interactionState_.drawing.drawingNotePitch;

        if (pitch > 0.0f && endTime > startTime) {
            int x1 = owner_.sourceTimeToX(startTime);
            int x2 = owner_.sourceTimeToX(endTime);
            float midiNote = 69.0f + 12.0f * std::log2(pitch / 440.0f);
            float y = owner_.midiToY(midiNote);
            float noteHeight = owner_.pixelsPerSemitone_;

            juce::Rectangle<float> noteRect(static_cast<float>(std::min(x1, x2)),
                                            y,
                                            static_cast<float>(std::abs(x2 - x1)),
                                            noteHeight);

            g.setColour(UIColors::noteBlockSelected.withAlpha(0.5f));
            g.fillRoundedRectangle(noteRect, 3.0f);
            g.setColour(UIColors::noteBlockSelected.withAlpha(0.8f));
            g.drawRoundedRectangle(noteRect, 3.0f, 1.5f);
        }
    }

    owner_.drawSelectionBox(g, themeId);

    // Selected note highlights �?drawn in overlay, not baked into detail cache
    if (!owner_.interactionState_.noteSelection.selectedIndices.empty()) {
        auto renderCtx = owner_.makePresentationRenderContext();
        if (auto* placement = owner_.findEditedPlacement()) {
            auto renderItem = owner_.buildContentRenderItem(*placement);
            owner_.renderer_->drawSelectedNoteHighlights(
                g, renderCtx, renderItem.displayNotes,
                owner_.interactionState_.noteSelection.selectedIndices, renderItem);
        }
    }
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
            float x = static_cast<float>(sourceTimeToX(timePos));

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

        const float x = static_cast<float>(sourceTimeToX(f0tl.timeAtFrame(frame)));
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
        float x = static_cast<float>(sourceTimeToX(anchor.time));
        float y = freqToY(anchor.freq);

        g.setColour(anchorColour);
        g.fillEllipse(x - 2.0f, y - 2.0f, 4.0f, 4.0f);

        if (i > 0) {
            const auto& prev = interactionState_.drawing.pendingAnchors[i - 1];
            float prevX = static_cast<float>(sourceTimeToX(prev.time));
            float prevY = freqToY(prev.freq);
            g.setColour(anchorColour.withAlpha(0.7f));
            g.drawLine(prevX, prevY, x, y, 2.0f);
        }
    }

    if (!interactionState_.drawing.pendingAnchors.empty()) {
        const auto& last = interactionState_.drawing.pendingAnchors.back();
        float lastX = static_cast<float>(sourceTimeToX(last.time));
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

    int x1 = sourceTimeToX(startTime);
    int x2 = sourceTimeToX(endTime);
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

void PianoRollComponent::paint(juce::Graphics& g) {
    // Chrome shell: component-space background, shadow, rounded clip, theme
    g.fillAll(UIColors::rollBackground);

    const auto bounds = getLocalBounds().toFloat();
    UIColors::drawShadow(g, bounds);

    juce::Path chromePath;
    chromePath.addRoundedRectangle(bounds, UIColors::cornerRadius);

    juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(chromePath);

    // Theme-specific background fill inside rounded clip
    switch (UIColors::currentThemeId()) {
        case ThemeId::DarkBlueGrey:
            UIColors::fillSoothe2SpectrumBackground(g, bounds, UIColors::cornerRadius);
            break;
        case ThemeId::Aurora:
            UIColors::fillAuroraTimelineBackground(g, bounds, UIColors::cornerRadius);
            break;
        case ThemeId::BlueBreeze:
            UIColors::fillMistedTimelineField(g, bounds, UIColors::cornerRadius);
            break;
        case ThemeId::Overdose:
            UiAssets::drawAssetStretch(g, UiAssetId::PanelEditorMain, bounds);
            break;
        default:
            g.setColour(UIColors::rollBackground);
            g.fillPath(chromePath);
            break;
    }

    // Pattern tiles: ruler/grid/lanes (already clipped to chrome shell)
    drawPreparedPatternTiles(g);
    // Content tiles: waveform/notes/F0/anchors (already clipped to chrome shell)
    drawPreparedContentTiles(g);
}

void PianoRollComponent::paintOverChildren(juce::Graphics& g)
{
    auto ctx = makePresentationRenderContext();
    renderer_->drawTimeGridHandles(g, ctx);
    if (shouldShowPianoKeys()) {
        renderer_->drawPianoKeys(g, ctx);
    }
}

bool PianoRollComponent::shouldShowPianoKeys() const noexcept
{
    return currentTool_ != ToolId::TimeTool;
}

void PianoRollComponent::setInferenceActive(bool active)
{
    inferenceActive_ = active;
    waveformBuildTickCounter_ = 0;
}

bool PianoRollComponent::applyNoteParameterToSelectedNotes(float retuneSpeed, float vibratoDepth, float vibratoRate) {
    auto notes = getEditedContentNotesCopy();
    auto originalNotes = notes;  // Save for before-patch in note-only undo path
    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return false;

    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;
    bool anySelected = false;

    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(notes.size()));
    for (int noteIndex : interactionState_.noteSelection.selectedIndices) {
        auto& n = notes[static_cast<size_t>(noteIndex)];
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
                retuneSpeed, vibratoDepth, vibratoRate);
            auto snap = clonedCurve->getSnapshot();

            const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(
                editRange.startFrame,
                editRange.endFrameExclusive,
                f0tl.endFrameExclusive());

            // Extract notes overlapping the expanded affected range (not just dirty range)
            const double affectedStartSec = f0tl.timeAtFrame(affectedRange.startFrame);
            const double affectedEndSec = f0tl.timeAtFrame(affectedRange.endFrameExclusive);
            auto overlapsRange = [affectedStartSec, affectedEndSec](const Note& n) {
                return n.endTime > affectedStartSec && n.startTime < affectedEndSec;
            };
            std::vector<Note> notesInRange;
            for (const auto& n : notes) {
                if (overlapsRange(n)) notesInRange.push_back(n);
            }

            // Extract segments overlapping the affected range (range-scoped, not full)
            auto allSegments = snap->getCorrectionSegments();
            std::vector<PitchCorrectionSegment> segmentsInRange;
            for (const auto& seg : allSegments) {
                if (seg.startFrame < affectedRange.endFrameExclusive && seg.endFrame > affectedRange.startFrame)
                    segmentsInRange.push_back(seg);
            }

            if (!commitEditedContentNotesAndSegments(notesInRange, segmentsInRange, affectedRange)) {
                return false;
            }

            listeners_.call([affectedRange](Listener& l) { l.pitchCurveEdited(affectedRange.startFrame, affectedRange.endFrameExclusive - 1); });
    repaint();
    return true;
        }
    }

    // Fallback: notes changed but no valid F0 timeline mapping or no current curve.
    // Pure note edit �?seconds-based PianoRollNotePatchAction for undo.
    if (anySelected && dirtyEndTime > dirtyStartTime) {
        ContentNoteRangePatch afterPatch;
        afterPatch.affectedRange.startSeconds = dirtyStartTime;
        afterPatch.affectedRange.endSeconds = dirtyEndTime;

        auto overlapsRange = [dirtyStartTime, dirtyEndTime](const Note& n) {
            return n.endTime > dirtyStartTime && n.startTime < dirtyEndTime;
        };

        for (const auto& n : notes) {
            if (overlapsRange(n)) afterPatch.afterNotesInRange.push_back(n);
        }

        if (!contentCommands_->commitNotePatch(editedContentKey_, afterPatch)) {
            return false;
        }

        refreshEditedContentNotes();

        // Build before-patch from original (unmodified) notes for undo.
        ContentNoteRangePatch beforePatch;
        beforePatch.affectedRange = afterPatch.affectedRange;
        for (const auto& n : originalNotes) {
            if (overlapsRange(n)) beforePatch.afterNotesInRange.push_back(n);
        }

        auto action = std::make_unique<PianoRollNotePatchAction>(
            contentCommands_,
            editedContentKey_,
            pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : TRANS("编辑"),
            std::move(beforePatch),
            std::move(afterPatch));

        if (processor_ != nullptr)
            processor_->getUndoManager().addAction(std::move(action));

        pendingUndoDescription_ = {};
        undoSnapshotCaptured_ = false;
            repaint();
        return true;
    }

    return false;
}

bool PianoRollComponent::applyParameterToFrameRange(float retuneSpeed, float vibratoDepth, float vibratoRate, int startFrame, int endFrameExclusive) {
    if (!currentCurve_ || endFrameExclusive <= startFrame) return false;
    if (!currentCurve_->hasCorrectionInRange(startFrame, endFrameExclusive)) return false;

    enqueueNoteBasedCorrectionAsync(getEditedContentNotesCopy(),
                                    startFrame, endFrameExclusive,
                                    retuneSpeed, vibratoDepth, vibratoRate);

    const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(startFrame,
                                                                          endFrameExclusive,
                                                                          currentF0Timeline().endFrameExclusive());
    const int notifyEndFrame = std::max(affectedRange.startFrame, affectedRange.endFrameExclusive - 1);
    listeners_.call([affectedRange, notifyEndFrame](Listener& l) { l.pitchCurveEdited(affectedRange.startFrame, notifyEndFrame); });
    repaint();
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
    const auto notes = getEditedContentNotesCopy();
    double minStart = std::numeric_limits<double>::max();
    double maxEnd = -1.0;
    for (int noteIndex : interactionState_.noteSelection.selectedIndices) {
        if (noteIndex < 0 || noteIndex >= static_cast<int>(notes.size())) {
            continue;
        }
        const auto& note = notes[static_cast<size_t>(noteIndex)];
        minStart = std::min(minStart, note.startTime);
        maxEnd = std::max(maxEnd, note.endTime);
    }
    return maxEnd > minStart && getFrameRangeForTimeSpan(minStart, maxEnd, startFrame, endFrameExclusive);
}

void PianoRollComponent::syncF0SelectionToSelectedNotes()
{
    int startFrame = 0;
    int endFrameExclusive = 0;
    if (getSelectedNotesFrameRange(startFrame, endFrameExclusive)) {
        interactionState_.selection.setF0Range(startFrame, endFrameExclusive);
        return;
    }

    interactionState_.selection.clearF0Selection();
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
    pendingUndoDescription_ = TRANS("Edit retune speed");
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
    const auto notes = getEditedContentNotesCopy();
    if (interactionState_.noteSelection.selectedIndices.size() != 1) {
        return false;
    }

    const int noteIndex = interactionState_.noteSelection.selectedIndices.front();
    if (noteIndex < 0 || noteIndex >= static_cast<int>(notes.size())) {
        return false;
    }

    const auto* selectedNote = &notes[static_cast<size_t>(noteIndex)];
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
    const auto& allSegments = snapshot->getCorrectionSegments();

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return -1;
    const float tolerancePixels = 15.0f;

    int bestIdx = -1;
    float bestDist = tolerancePixels;

    for (int i = 0; i < static_cast<int>(allSegments.size()); ++i) {
        const auto& seg = allSegments[i];
        if (seg.source != PitchCorrectionSegment::Source::LineAnchor) continue;
        if (seg.f0Data.empty()) continue;

        const double startTime = f0tl.timeAtFrame(seg.startFrame);
        const double endTime   = f0tl.timeAtFrame(seg.endFrame);

        const int startX = sourceTimeToX(startTime);
        const int endX   = sourceTimeToX(endTime);

        if (x < startX - tolerancePixels || x > endX + tolerancePixels) continue;

        const double clickSource = xToSourceTime(x);
        const double relT = juce::jlimit(0.0, 1.0,
            (clickSource - startTime) / (endTime - startTime));
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
    // Note Split 鎺у埗闊抽珮鍒嗘闃堝€硷紙cents�?
    segmentationPolicy_.transitionThresholdCents = juce::jlimit(
        OpenTune::PitchControlConfig::kMinNoteSplitCents,
        OpenTune::PitchControlConfig::kMaxNoteSplitCents,
        value);

    // Note Split 浠呮洿鏂板垎娈电瓥鐣ュ弬鏁帮紝涓嶈Е鍙?AUTO 閲嶆柊鐢熸垚�?
    // AUTO 鎿嶄綔鐢辩敤鎴蜂富鍔ㄨЕ鍙戯紝浣跨敤褰撳墠绛栫暐鎵ц鍒嗘�?
    repaint();
}

void PianoRollComponent::resized() {

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
    timeUnitToggleButton_.toFront(false);
    scrollModeToggleButton_.toFront(false);

    previewOverlay_.setBounds(getLocalBounds());
    fixedPlayhead_.setBounds(getLocalBounds());
    updatePlayheadVisibility();
    prepareVisiblePatternTiles();
    prepareVisibleContentTiles();
    repaint();
}

void PianoRollComponent::applyEditedContentCurve(std::shared_ptr<PitchCurve> curve)
{
    currentCurve_ = std::move(curve);

    interactionState_.selection.hasSelectionArea = false;
    interactionState_.selection.selectionStartTime = 0.0;
    interactionState_.selection.selectionEndTime = 0.0;
    interactionState_.selection.selectionStartMidi = 0.0f;
    interactionState_.selection.selectionEndMidi = 0.0f;
}

void PianoRollComponent::applyEditedContentAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                                       int sampleRate)
{
    audioBuffer_ = std::move(buffer);
    audioBufferSampleRate_ = sampleRate > 0 ? static_cast<double>(sampleRate)
                                            : static_cast<double>(PianoRollComponent::kAudioSampleRate);
}

double PianoRollComponent::getContentDurationSeconds() const
{
    return activeContentProjection().contentDurationSeconds;
}

const PianoRollComponent::TimelineContentPlacement* PianoRollComponent::findEditedPlacement() const noexcept
{
    if (!editedContentKey_.isValid())
        return nullptr;
    for (const auto& placement : timelineContentPlacements_)
    {
        if (placement.contentKey == editedContentKey_ && placement.projection.isValid())
            return &placement;
    }
    return nullptr;
}

bool PianoRollComponent::hasTimelineContentPlacement() const noexcept
{
    for (const auto& placement : timelineContentPlacements_) {
        if (placement.isValid()) {
            return true;
        }
    }
    return false;
}

ContentTimelineProjection PianoRollComponent::activeContentProjection() const noexcept
{
    if (const auto* placement = findEditedPlacement())
        return placement->projection;
    return {};
}

double PianoRollComponent::sourceTimeToTimelineTime(double sourceSeconds) const
{
    const auto projection = activeContentProjection();
    jassert(projection.isValid());
    const auto snap = readEditedSnapshot();
    jassert(snap != nullptr && snap->timeGrid != nullptr);
    const double outputSeconds = snap->timeGrid->tauForward(sourceSeconds);
    return projection.projectContentTimeToTimeline(outputSeconds);
}

int PianoRollComponent::sourceTimeToX(double sourceSeconds) const
{
    return makeViewMapper().timeToX(sourceTimeToTimelineTime(sourceSeconds));
}

double PianoRollComponent::xToSourceTime(int x) const
{
    const auto projection = activeContentProjection();
    jassert(projection.isValid());

    const auto snap = readEditedSnapshot();
    jassert(snap != nullptr && snap->timeGrid != nullptr);

    const double timeline = makeViewMapper().xToTime(x);
    const double output   = projection.projectTimelineTimeToContent(timeline);
    return snap->timeGrid->tauInverse(output);
}

SourceEditRange PianoRollComponent::sourceEditRange() const
{
    const auto snap = readEditedSnapshot();
    jassert(snap != nullptr && snap->timeGrid != nullptr);
    return SourceEditRange::fromTimeGrid(*snap->timeGrid, 0.0);
}

bool PianoRollComponent::applyTimelineContentPlacements(std::vector<TimelineContentPlacement> placements,
                                                               bool explicitContract)
{
    placements.erase(std::remove_if(placements.begin(),
                                    placements.end(),
                                    [](const auto& placement) { return !placement.isValid(); }),
                     placements.end());

    const bool changed = placements.size() != timelineContentPlacements_.size()
        || !std::equal(placements.begin(), placements.end(), timelineContentPlacements_.begin(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.contentKey == rhs.contentKey
                    && std::abs(lhs.projection.timelineStartSeconds - rhs.projection.timelineStartSeconds) <= 1.0e-9
                    && std::abs(lhs.projection.timelineDurationSeconds - rhs.projection.timelineDurationSeconds) <= 1.0e-9
                    && std::abs(lhs.projection.contentDurationSeconds - rhs.projection.contentDurationSeconds) <= 1.0e-9;
            });

    explicitTimelineContentPlacements_ = explicitContract;

    if (!changed) {
        return false;
    }

    timelineContentPlacements_ = std::move(placements);

    std::set<ContentKey> aliveContents;
    for (const auto& placement : timelineContentPlacements_)
        aliveContents.insert(placement.contentKey);
    waveformMipmapCache_.prune(aliveContents);

    userScrollHold_ = false;
    updateScrollBars();
    prepareVisibleContentTiles();
    repaint();
    return true;
}

void PianoRollComponent::deriveSingleTimelineContentPlacement()
{
    if (explicitTimelineContentPlacements_) {
        return;
    }

    std::vector<TimelineContentPlacement> placements;
    if (editedContentKey_.isValid() && pendingSingleContentProjection_.isValid()) {
        placements.push_back({ editedContentKey_, pendingSingleContentProjection_ });
    }

    applyTimelineContentPlacements(std::move(placements), false);
}

void PianoRollComponent::setContentProjection(const ContentTimelineProjection& projection)
{
    const bool changed = std::abs(pendingSingleContentProjection_.timelineStartSeconds - projection.timelineStartSeconds) > 1.0e-9
        || std::abs(pendingSingleContentProjection_.timelineDurationSeconds - projection.timelineDurationSeconds) > 1.0e-9
        || std::abs(pendingSingleContentProjection_.contentDurationSeconds - projection.contentDurationSeconds) > 1.0e-9;

    if (!changed) {
        return;
    }

    pendingSingleContentProjection_ = projection;
    explicitTimelineContentPlacements_ = false;
    deriveSingleTimelineContentPlacement();
    userScrollHold_ = false;
    prepareVisibleContentTiles();
    repaint();
}

void PianoRollComponent::setTimelineContentPlacements(std::vector<TimelineContentPlacement> placements)
{
    if (applyTimelineContentPlacements(std::move(placements), true)) {
        pendingSingleContentProjection_ = activeContentProjection();
    }
}

void PianoRollComponent::setEditedContent(ContentKey contentKey,
                                           std::shared_ptr<PitchCurve> curve,
                                           std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                           int sampleRate)
{
    const double normalizedSampleRate = sampleRate > 0 ? static_cast<double>(sampleRate)
                                                        : static_cast<double>(PianoRollComponent::kAudioSampleRate);
    const bool contentChanged = editedContentKey_ != contentKey;
    const bool curveChanged = currentCurve_ != curve;
    const bool bufferChanged = audioBuffer_ != buffer || audioBufferSampleRate_ != normalizedSampleRate;

    if (!contentChanged && !curveChanged && !bufferChanged) {
        return;
    }

    if (contentChanged) {
        editedContentKey_ = contentKey;
        clearNoteDraft();
        autoTuneInFlight_.store(false, std::memory_order_release);
        pendingUndoDescription_ = {};
        beforeUndoNotes_.clear();
        beforeUndoSegments_.clear();
        undoSnapshotCaptured_ = false;
    }

    // notes �?pitchCurve 閫氳�?commitNotesAndPitchCurve 鍚屽啓鍒?store�?
    // 璇讳晶涔熷繀椤诲悓璇伙細curveChanged 鏃跺繀�?refresh notes锛屽惁鍒?undo/redo �?
    // 鍑虹�?curve 鍥為€€�?notes 瑙嗚娈嬬暀鐨勪笉瀵圭О锛坈achedNotes_ 婊炲悗锛夈€?
    if (contentChanged || curveChanged) {
        refreshEditedContentNotes();
    }

    if (contentChanged || curveChanged) {
        editedContentEpoch_.fetch_add(1, std::memory_order_release);
        applyEditedContentCurve(std::move(curve));
    }

    if (contentChanged || bufferChanged) {
        applyEditedContentAudioBuffer(std::move(buffer), sampleRate);
    }

    if (contentChanged || bufferChanged) {
        deriveSingleTimelineContentPlacement();
    }

    userScrollHold_ = false;
    updateScrollBars();
    prepareVisibleContentTiles();
    repaint();
}

void PianoRollComponent::onTimeGridRevisionChanged()
{
    timeGridEpoch_.fetch_add(1, std::memory_order_relaxed);
    prepareVisibleContentTiles();
    repaint();
}

void PianoRollComponent::onNotesRevisionChanged()
{
    refreshEditedContentNotes();
    notesEpoch_.fetch_add(1, std::memory_order_relaxed);
    prepareVisibleContentTiles();
    repaint();
}

void PianoRollComponent::onPitchRevisionChanged()
{
    f0LODCache_.clear();
    pitchEpoch_.fetch_add(1, std::memory_order_relaxed);
    prepareVisibleContentTiles();
    repaint();
}

void PianoRollComponent::requestContentRedraw() {
    prepareVisibleContentTiles();
    repaint();
}

double PianoRollComponent::readPlayheadTime() const
{
    if (auto source = positionSource_.lock()) {
        return source->load(std::memory_order_relaxed);
    }
    return 0.0;
}

juce::Rectangle<int> PianoRollComponent::getTimelineViewportBounds() const
{
    constexpr int panelInset = 12;
    const int viewportWidth = juce::jmax(0, getWidth() - panelInset - verticalScrollBar_.getWidth());
    const int viewportHeight = juce::jmax(0, getHeight() - panelInset - horizontalScrollBar_.getHeight());
    return { 0, 0, viewportWidth, viewportHeight };
}

TimelineViewportRequest PianoRollComponent::makeViewportRequest(
    TimelineViewportRequest::Kind kind,
    double targetTime,
    double anchorViewportX,
    double pps) const
{
    TimelineViewportRequest req;
    req.kind = kind;
    req.viewKind = TimelineViewportRequest::ViewKind::PianoRoll;
    req.targetTime = targetTime;
    req.anchorViewportX = anchorViewportX;
    req.viewportWidth = getTimelineContentViewportWidth();
    req.pixelsPerSecond = pps;
    return req;
}

PianoRollRenderer::RenderContext PianoRollComponent::makePresentationRenderContext() const
{
    return buildRenderContext();
}

void PianoRollComponent::onHeartbeatTick()
{
    if (!isShowing()) {
        return;
    }

    consumeCompletedCorrectionResults();

    const bool playingNow = isPlaying_.load(std::memory_order_relaxed);
    if (showWaveform_) {
        bool progressed = false;
        if (inferenceActive_) {
            waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 8;
            if (waveformBuildTickCounter_ == 0)
                progressed = waveformMipmapCache_.buildIncremental(0.15);
        } else if (playingNow) {
            waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 6;
            if (waveformBuildTickCounter_ == 0)
                progressed = waveformMipmapCache_.buildIncremental(0.25);
        } else {
            waveformBuildTickCounter_ = 0;
            progressed = waveformMipmapCache_.buildIncremental(0.75);
        }

        if (progressed) {
            if (playingNow) {
                waveformVisualRefreshPending_ = true;
            } else {
                waveformVisualRefreshPending_ = false;
            repaint();
            }
        }
    } else {
        waveformBuildTickCounter_ = 0;
        waveformVisualRefreshPending_ = false;
    }

    if (!playingNow && waveformVisualRefreshPending_) {
        waveformVisualRefreshPending_ = false;
        repaint();
    }
}

void PianoRollComponent::onScrollVBlankCallback(double timestampSec)
{
    (void)timestampSec;

    if (!isShowing())
        return;

    if (!isPlaying_.load(std::memory_order_relaxed))
        return;

    // Playing: camera follows transport. hostTime is already absolute timeline
    // seconds; pass it directly to the policy (no content/timeline projection).
    // Playhead is fixed at content-area centre (viewport centre - pianoKeyWidth).
    const double playheadTime = readPlayheadTime();
    const int visibleWidth = getTimelineContentViewportWidth();
    if (visibleWidth <= 0)
        return;

    if (scrollMode_ == ScrollMode::Continuous || scrollMode_ == ScrollMode::Page) {
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Click,
            playheadTime,
            static_cast<double>(getTimelineViewportBounds().getCentreX() - pianoKeyWidth_),
            camera_.pixelsPerSecond);
        commitViewportRequest(req, juce::sendNotification);
    }
}

void PianoRollComponent::commitViewportRequest(TimelineViewportRequest req, juce::NotificationType notify) {
    applyResolvedCamera(TimelineViewportPolicy::resolve(req), notify);
}

void PianoRollComponent::applyResolvedCamera(TimelineViewportCamera next, juce::NotificationType notify) {
    if (next == camera_) {
        updatePlayheadVisibility();
        return;
    }

    camera_ = next;

    prepareVisiblePatternTiles();
    prepareVisibleContentTiles();
    updateScrollBars();
    updatePlayheadVisibility();
    repaint();

    if (notify == juce::sendNotification) {
        listeners_.call([this](Listener& l) { l.timelineViewportChanged(camera_); });
    }
}

void PianoRollComponent::focusActiveContentForRegionSwitch(
    const std::vector<SilentGap>& silentGaps,
    juce::NotificationType notify)
{
    // Respect user manual interaction
    if (userHasManuallyZoomed_ || userScrollHold_)
        return;

    const auto projection = activeContentProjection();
    if (!projection.isValid())
        return;

    const double duration = projection.timelineDurationSeconds;
    const int visibleWidth = getTimelineContentViewportWidth();
    if (visibleWidth <= 0 || duration <= 0.0)
        return;

    constexpr double defaultPps = TimelineViewportCamera::kDefaultPixelsPerSecond;

    // Fit entire content — policy clamps pps to valid range
    const double fitPps = static_cast<double>(visibleWidth) / duration;
    const auto req = makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        projection.timelineStartSeconds,
        0.0,
        (fitPps > 0.0) ? fitPps : defaultPps);
    commitViewportRequest(req, notify);
}

void PianoRollComponent::setCurrentTool(ToolId tool) {
    if (tool == ToolId::TimeTool && !experimentalFeaturesEnabled_) {
        tool = ToolId::Select;
    }

    if (tool == ToolId::TimeTool
        && currentTool_ != ToolId::TimeTool
        && processor_ != nullptr
        && editedContentKey_.isValid()) {
        // This is a processor-specific operation, not content
        processor_->ensureTimeToolAnchorSeed(editedContentKey_);
    }

    const bool toolChanged = currentTool_ != tool;
    bool clearedAnchorPreview = false;
    if (interactionState_.drawing.isPlacingAnchors && tool != ToolId::LineAnchor) {
        interactionState_.drawing.isPlacingAnchors = false;
        interactionState_.drawing.pendingAnchors.clear();
        clearedAnchorPreview = true;
    }

    // 鈿★�?vocal-time-stretch �?.4 (Phase F) �?Time tool is mutually exclusive
    // with the Note family of tools.  Switching INTO TimeTool drops any
    // inflight note-side state so the user's next mouseDown is interpreted
    // strictly as a TimeGrid handle action; switching OUT clears Time-tool
    // selection so a stale handle highlight doesn't persist into Note tools.
    if (toolChanged) {
        if (tool == ToolId::TimeTool) {
            if (pressedPianoKey_ >= 0) {
                if (pianoKeyAudition_ != nullptr)
                    pianoKeyAudition_->noteOff(pressedPianoKey_);
                pressedPianoKey_ = -1;
            }
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
            // �?.4: Time tool uses normal cursor + per-handle hover hand cursor
            // applied by handleTimeToolMouseMove (via ctx.setMouseCursor).
            setMouseCursor(juce::MouseCursor::NormalCursor);
            break;
    }

    // 閫氱煡鐩戝惉鑰呭伐鍏峰凡鍒囨崲锛堝弬鏁伴潰鏉块渶瑕佸悓姝ユ寜閽珮浜級
    if (toolChanged) {
        listeners_.call([tool](Listener& l) { l.currentToolChanged(tool); });
        }

    if (toolChanged || clearedAnchorPreview) {
        repaint(getLocalBounds());
    }
}

void PianoRollComponent::setExperimentalFeaturesEnabled(bool enabled)
{
    if (experimentalFeaturesEnabled_ == enabled) {
        return;
    }

    experimentalFeaturesEnabled_ = enabled;
    if (!enabled && currentTool_ == ToolId::TimeTool) {
        setCurrentTool(ToolId::Select);
        return;
    }

    repaint(getLocalBounds());
}

void PianoRollComponent::setShowWaveform(bool shouldShow) {
    if (showWaveform_ == shouldShow) return;
    showWaveform_ = shouldShow;
    prepareVisibleContentTiles();
    repaint();
}

void PianoRollComponent::setShowLanes(bool shouldShow) {
    if (showLanes_ == shouldShow) return;
    showLanes_ = shouldShow;
    prepareVisiblePatternTiles();
    repaint();
}

void PianoRollComponent::setNoteNameMode(NoteNameMode noteNameMode) {
    if (noteNameMode_ == noteNameMode) return;
    noteNameMode_ = noteNameMode;
    prepareVisibleContentTiles();
    repaint();
}

void PianoRollComponent::setShowUnvoicedFrames(bool shouldShow) {
    if (showUnvoicedFrames_ == shouldShow) return;
    showUnvoicedFrames_ = shouldShow;
    prepareVisibleContentTiles();
    repaint();
}

void PianoRollComponent::setBpm(double bpm) {
    bpm_ = juce::jlimit(60.0, 240.0, bpm);
    prepareVisiblePatternTiles();
    repaint();
}

void PianoRollComponent::setTimeSignature(int numerator, int denominator) {
    if (numerator <= 0 || denominator <= 0) return;
    timeSigNum_ = numerator;
    timeSigDenom_ = denominator;
    prepareVisiblePatternTiles();
    repaint();
}

void PianoRollComponent::setTimeUnit(TimeUnit unit) {
    if (timeUnit_ == unit) return;
    timeUnit_ = unit;
    prepareVisiblePatternTiles();
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

void PianoRollComponent::mouseDoubleClick(const juce::MouseEvent& e) {
    // 鈿★�?vocal-time-stretch �?.4 �?Time tool double-click forwarded to handler.
    // Other tools currently have no double-click semantics, so the handler
    // ignores them by switching on currentTool_.
    toolHandler_->mouseDoubleClick(e);
}

void PianoRollComponent::mouseDown(const juce::MouseEvent& e) {
    if (isAutoTuneProcessing()) {
        return;
    }

    // Ctrl+drag panning �?only on non-interactive area, so existing
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
            dragStartVerticalScrollOffset_ = verticalScrollOffset_;
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            return;
        }
    }

    // Piano key audition: click in piano key area triggers note preview.
    if (shouldShowPianoKeys() && e.x < pianoKeyWidth_) {
        int midiNote = static_cast<int>(std::ceil(yToMidi(static_cast<float>(e.y))));
        midiNote = juce::jlimit(0, 127, midiNote);
        pressedPianoKey_ = midiNote;
        if (pianoKeyAudition_ != nullptr)
            pianoKeyAudition_->noteOn(midiNote);
        repaint();
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
        const double pps = camera_.pixelsPerSecond;
        const double newVisibleStart = camera_.visibleStartSeconds - deltaX / pps;
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            newVisibleStart,
            0.0,
            pps);
        commitViewportRequest(req, juce::dontSendNotification);
        float newScrollY = dragStartVerticalScrollOffset_ - (float)deltaY;
        float maxScroll = getTotalHeight() - getHeight();
        verticalScrollOffset_ = juce::jlimit(0.0f, std::max(0.0f, maxScroll), newScrollY);
        refreshVerticalViewportGeometry();
        return;
    }

    // Piano key glissando: dragging across keys changes the note
    if (shouldShowPianoKeys() && pressedPianoKey_ >= 0) {
        int midiNote = static_cast<int>(std::ceil(yToMidi(static_cast<float>(e.y))));
        midiNote = juce::jlimit(0, 127, midiNote);
        if (midiNote != pressedPianoKey_) {
            if (pianoKeyAudition_ != nullptr) {
                pianoKeyAudition_->noteOff(pressedPianoKey_);
                pianoKeyAudition_->noteOn(midiNote);
            }
            pressedPianoKey_ = midiNote;
            repaint();
        }
        return;
    }

    toolHandler_->mouseDrag(e);
    // Note: individual tool handlers call invalidateVisual() with proper dirty
    // rects when needed. The preview overlay handles transient visuals (draw-note
    // preview, selection box) without triggering render-model rebuild.
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

    if (shouldShowPianoKeys() && pressedPianoKey_ >= 0) {
        if (pianoKeyAudition_ != nullptr)
            pianoKeyAudition_->noteOff(pressedPianoKey_);
        pressedPianoKey_ = -1;
        repaint();
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
    const double pps = camera_.pixelsPerSecond;
    const double newVisibleStart = camera_.visibleStartSeconds - pixelDelta / pps;
    userScrollHold_ = true;
    const auto req = makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        newVisibleStart,
        0.0,
        pps);
    commitViewportRequest(req, juce::sendNotification);
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
    const double oldPps = camera_.pixelsPerSecond;
    const double newPps = oldPps * zoomFactor;

    const int mouseX = e.x - pianoKeyWidth_;
    const double mouseTime = camera_.visibleStartSeconds + mouseX / oldPps;

    userHasManuallyZoomed_ = true;
    const auto req = makeViewportRequest(
        TimelineViewportRequest::Kind::Zoom,
        mouseTime,
        static_cast<double>(mouseX),
        newPps);
    commitViewportRequest(req, juce::sendNotification);
}

void PianoRollComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    float deltaX = wheel.deltaX;
    float deltaY = wheel.deltaY;

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
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Undo, key)) {
        listeners_.call([](Listener& l) { l.undoRequested(); });
        return true;
    }
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Redo, key)) {
        listeners_.call([](Listener& l) { l.redoRequested(); });
        return true;
    }
    return toolHandler_->keyPressed(key);
}

PianoRollRenderer::ContentRenderItem PianoRollComponent::buildContentRenderItem(
    const TimelineContentPlacement& placement) const
{
    PianoRollRenderer::ContentRenderItem item;
    item.contentKey = placement.contentKey;
    item.projection = placement.projection;
    item.active = placement.contentKey == editedContentKey_;

    std::shared_ptr<PitchCurve> curve;
    if (item.active) {
        curve = currentCurve_;
        item.audioBuffer = audioBuffer_;
        item.displayNotes = getDisplayedNotes();
    } else {
        if (auto snap = readSnapshotFor(placement.contentKey)) {
            curve = snap->pitchCurve;
            item.audioBuffer = snap->audioBuffer;
            item.displayNotes = snap->notes;
        }
    }

    if (curve != nullptr) {
        item.pitchSnapshot = curve->getSnapshot();
        if (item.pitchSnapshot != nullptr && item.pitchSnapshot->size() > 0)
            item.f0Timeline = { item.pitchSnapshot->getHopSize(),
                                item.pitchSnapshot->getSampleRate(),
                                static_cast<int>(item.pitchSnapshot->size()) };
    }

    return item;
}

std::shared_ptr<const F0VisualLOD> PianoRollComponent::getOrBuildF0LOD(
    ContentKey key,
    std::shared_ptr<const PitchCurveSnapshot> pitchSnapshot) const
{
    if (!pitchSnapshot || pitchSnapshot->size() == 0)
        return nullptr;

    F0LODCacheKey cacheKey;
    cacheKey.contentKey = key;
    cacheKey.pitchRenderGeneration = pitchSnapshot->getRenderGeneration();

    // Cache hit
    auto it = f0LODCache_.find(cacheKey);
    if (it != f0LODCache_.end())
        return it->second;

    // Cache miss: build
    auto f0LOD = std::make_shared<F0VisualLOD>();
    const auto& originalF0 = pitchSnapshot->getOriginalF0();

    // Only build correctedF0 if correction layer exists (lazy)
    std::vector<float> correctedF0;
    if (pitchSnapshot->hasCorrectionLayer()) {
        correctedF0.resize(originalF0.size(), 0.0f);
        pitchSnapshot->renderCorrectionLayerF0Range(
            0, static_cast<int>(originalF0.size()),
            [&](int frame, const float* data, int length) {
                for (int i = 0; i < length; ++i) {
                    const int f = frame + i;
                    if (f >= 0 && f < static_cast<int>(correctedF0.size()))
                        correctedF0[static_cast<size_t>(f)] = data[i];
                }
            });
    }

    f0LOD->build(originalF0, correctedF0,
                pitchSnapshot->getHopSize(),
                pitchSnapshot->getSampleRate());

    f0LODCache_[cacheKey] = f0LOD;
    return f0LOD;
}

void PianoRollComponent::visibilityChanged()
{
    // When component becomes visible, automatically grab keyboard focus
    // This ensures user can use shortcuts (e.g., Ctrl+A to select all) without
    // manual click.
    if (isShowing() && isVisible())
    {
        // Use callAfterDelay to ensure focus grab after message loop processing
        // is complete. This is necessary because component may not be able to
        // receive focus immediately when it just became visible.
        juce::Component::SafePointer<PianoRollComponent> safeThis(this);
        juce::Timer::callAfterDelay(10, [safeThis]() {
            if (safeThis != nullptr && safeThis->isShowing())
            {
                safeThis->grabKeyboardFocus();
            }
        });
        updatePlayheadVisibility();
    }
}

void PianoRollComponent::setReferenceOverlay(std::optional<PianoRollRenderer::ReferenceOverlay> overlay)
{
    referenceOverlay_ = std::move(overlay);
    previewOverlay_.repaint();
}

PianoRollRenderer::RenderContext PianoRollComponent::buildRenderContext(int renderWidthPx,
                                                                        int renderPianoKeyWidth) const
{
    PianoRollRenderer::RenderContext ctx;

    ctx.width = renderWidthPx;
    ctx.height = getTimelineViewportBounds().getBottom();
    ctx.pianoKeyWidth = renderPianoKeyWidth;
    ctx.rulerHeight = rulerHeight_;
    ctx.pixelsPerSecond = camera_.pixelsPerSecond;
    ctx.pixelsPerSemitone = pixelsPerSemitone_;
    ctx.minMidi = minMidi_;
    ctx.maxMidi = maxMidi_;
    ctx.bpm = bpm_;
    ctx.scaleRootNote = scaleRootNote_;
    ctx.scaleType = scaleType_;
    ctx.noteNameMode = noteNameMode_;
    ctx.showLanes = showLanes_;
    ctx.showUnvoicedFrames = showUnvoicedFrames_;
    ctx.showOriginalF0 = showOriginalF0_;
    ctx.showCorrectedF0 = showCorrectedF0_;
    ctx.timeUnit = (timeUnit_ == TimeUnit::Bars)
        ? PianoRollTimeUnit::Bars
        : PianoRollTimeUnit::Seconds;
    ctx.activeProjection = activeContentProjection();
    ctx.coords = makeViewMapper();

    ctx.contents.reserve(timelineContentPlacements_.size());
    for (const auto& placement : timelineContentPlacements_)
        if (placement.isValid())
            ctx.contents.push_back(buildContentRenderItem(placement));

    // Transient fields
    ctx.pressedPianoKey = pressedPianoKey_;
    ctx.hasF0Selection = interactionState_.selection.hasF0Selection;
    ctx.f0SelectionStartFrame = interactionState_.selection.selectedF0StartFrame;
    ctx.f0SelectionEndFrameExclusive = interactionState_.selection.selectedF0EndFrameExclusive;

    if (interactionState_.timeTool.isDraggingHandle
        && interactionState_.timeTool.dragWorkingSnapshot != nullptr) {
        ctx.timeGridSnapshot = interactionState_.timeTool.dragWorkingSnapshot;
    } else if (auto snap = readEditedSnapshot()) {
        ctx.timeGridSnapshot = snap->timeGrid;
    }

    ctx.timeGridHoveredHandleId  = interactionState_.timeTool.hoveredHandleId;
    ctx.timeGridSelectedHandleId = interactionState_.timeTool.selectedHandleId;
    ctx.additionalSelectedHandleIds = interactionState_.timeTool.additionalSelectedIds;
    ctx.selectedLineAnchorSegmentIds = interactionState_.selectedLineAnchorSegmentIds;
    ctx.currentTool = currentTool_;
    ctx.referenceOverlay = referenceOverlay_;

    return ctx;
}

int PianoRollComponent::getTimelineContentViewportWidth() const
{
    return juce::jmax(0, getTimelineViewportBounds().getWidth() - pianoKeyWidth_);
}

int PianoRollComponent::getTimelineContentViewportHeight() const
{
    return juce::jmax(0, getTimelineViewportBounds().getHeight() - rulerHeight_);
}

int PianoRollComponent::getMaxHorizontalScroll() const
{
    const int visibleWidth = getTimelineContentViewportWidth();
    const int totalContentWidth = static_cast<int>(horizontalScrollBar_.getRangeLimit().getEnd());
    return juce::jmax(0, totalContentWidth - visibleWidth);
}

void PianoRollComponent::refreshVerticalViewportGeometry()
{
    updateScrollBars();
    prepareVisiblePatternTiles();
    prepareVisibleContentTiles();
    repaint();
}

void PianoRollComponent::setScale(int rootNote, int scaleType)
{
    const int clampedRoot = juce::jlimit(0, 11, rootNote);
    const int clampedType = juce::jlimit(1, 8, scaleType);
    if (scaleRootNote_ == clampedRoot && scaleType_ == clampedType)
        return;
    scaleRootNote_ = clampedRoot;
    scaleType_ = clampedType;
    prepareVisiblePatternTiles();
    repaint();
}

void PianoRollComponent::fitToScreen() {
    // 濡傛灉鐢ㄦ埛宸叉墜鍔ㄨ皟鏁磋繃缂╂斁锛屼笉鑷姩瑕嗙�?
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
    const auto activeProjection = activeContentProjection();
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
        double pixelsPerSecond = static_cast<double>(viewWidth) / duration;
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            hasProjectedClipTimeline ? activeProjection.timelineStartSeconds - duration * 0.1 : 0.0,
            0.0,
            pixelsPerSecond);
        commitViewportRequest(req, juce::sendNotification);
    } else {
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            0.0,
            0.0,
            TimelineViewportCamera::kDefaultPixelsPerSecond);
        commitViewportRequest(req, juce::sendNotification);
    }
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
    // 缁熶竴璇箟锛氶鐜団啍MIDI 浠モ€滃崐闊充腑蹇冪嚎鈥濅负閿氱偣锛堜笉鏄敭杈圭晫锛夈�?
    return 12.0f * std::log2(frequency / 440.0f) + 69.0f - 0.5f;
}

float PianoRollComponent::midiToFreq(float midiNote) const {
    // �?freqToMidi 淇濇寔涓ユ牸浜掗€嗙殑涓績绾块敋鐐圭害瀹氥�?
    return 440.0f * std::pow(2.0f, (midiNote + 0.5f - 69.0f) / 12.0f);
}

float PianoRollComponent::yToFreq(float y) const {
    return midiToFreq(yToMidi(y));
}

float PianoRollComponent::freqToY(float freq) const {
    return midiToY(freqToMidi(freq));
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

juce::String PianoRollComponent::AutoTuneApplyResult::message() const
{
    switch (status) {
        case AutoTuneApplyStatus::Applied:
            return juce::String("AUTO has been queued.");
        case AutoTuneApplyStatus::NoCurve:
            return juce::String("AUTO needs an active pitch curve. Run audio analysis first.");
        case AutoTuneApplyStatus::NoProcessor:
            return juce::String("AUTO cannot run because the processor is not attached.");
        case AutoTuneApplyStatus::NoContent:
            return juce::String("AUTO needs an active editable clip.");
        case AutoTuneApplyStatus::MissingContentSnapshot:
            return juce::String("AUTO cannot read the editable content snapshot.");
        case AutoTuneApplyStatus::OriginalF0NotReady:
            return juce::String("AUTO needs OriginalF0 to be ready for this clip.");
        case AutoTuneApplyStatus::AlreadyInFlight:
            return juce::String("AUTO is already processing this clip.");
        case AutoTuneApplyStatus::MissingCurveSnapshot:
            return juce::String("AUTO cannot read the current pitch-curve snapshot.");
        case AutoTuneApplyStatus::EmptyOriginalF0:
            return juce::String("AUTO needs non-empty OriginalF0 data.");
        case AutoTuneApplyStatus::EmptyTimeline:
            return juce::String("AUTO cannot map this clip to an F0 timeline.");
        case AutoTuneApplyStatus::NoTargetSelection:
            return juce::String("AUTO needs a selected note, F0 range, or selection area.");
        case AutoTuneApplyStatus::EmptyTargetRange:
            return juce::String("AUTO target range is empty.");
    }

    return juce::String("AUTO could not be applied.");
}

PianoRollComponent::AutoTuneApplyResult PianoRollComponent::applyAutoTuneToSelection()
{
    AppLogger::log("AutoTune: applyAutoTuneToSelection entry"
        " contentKey.objectId=" + juce::String(static_cast<juce::int64>(editedContentKey_.objectId))
        + " curve=" + juce::String(currentCurve_ != nullptr ? 1 : 0)
        + " processor=" + juce::String(processor_ != nullptr ? 1 : 0));

    if (!currentCurve_) {
        return { AutoTuneApplyStatus::NoCurve };
    }

    if (!processor_) {
        return { AutoTuneApplyStatus::NoProcessor };
    }

    if (!editedContentKey_.isValid()) {
        return { AutoTuneApplyStatus::NoContent };
    }

    auto snap = readEditedSnapshot();
    if (snap == nullptr) {
        return { AutoTuneApplyStatus::MissingContentSnapshot };
    }

    const auto originalF0State = snap->originalF0State;
    if (originalF0State != OriginalF0State::Ready) {
        return { AutoTuneApplyStatus::OriginalF0NotReady };
    }

    if (autoTuneInFlight_.exchange(true, std::memory_order_acq_rel)) {
        return { AutoTuneApplyStatus::AlreadyInFlight };
    }

    repaint(); repaint();

    auto failAfterStart = [this](AutoTuneApplyStatus status) {
        autoTuneInFlight_.store(false, std::memory_order_release);
    repaint();  // scale高亮是chrome，只repaint
        return AutoTuneApplyResult{ status };
    };

    auto snapshot = currentCurve_->getSnapshot();
    if (!snapshot) {
        AppLogger::log("AutoTune: apply failed reason=null_snapshot");
        return failAfterStart(AutoTuneApplyStatus::MissingCurveSnapshot);
    }

    AppLogger::log("AutoTune: snapshot frames=" + juce::String(static_cast<int>(snapshot->size()))
        + " hop=" + juce::String(snapshot->getHopSize())
        + " sampleRate=" + juce::String(snapshot->getSampleRate())
        + " duration=" + juce::String(getContentDurationSeconds()));

    const auto& originalF0 = snapshot->getOriginalF0();
    if (originalF0.empty()) {
        return failAfterStart(AutoTuneApplyStatus::EmptyOriginalF0);
    }
    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) {
        return failAfterStart(AutoTuneApplyStatus::EmptyTimeline);
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
        return failAfterStart(AutoTuneApplyStatus::NoTargetSelection);
    }

    const auto targetRange = f0tl.rangeForFrames(targetDecision.range.startFrame,
                                                targetDecision.range.endFrameExclusive);
    if (targetRange.isEmpty()) {
        return failAfterStart(AutoTuneApplyStatus::EmptyTargetRange);
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

    request->autoOriginalF0Full = originalF0;
    request->autoHopSize = currentCurve_ ? currentCurve_->getHopSize() : 512;
    request->autoF0SampleRate = currentCurve_ ? currentCurve_->getSampleRate() : 16000.0;
    request->autoStartFrame = startFrame;
    request->autoEndFrame = endFrame;
    request->autoGenParams = genParams;

    request->contentEpochSnapshot = editedContentEpoch_.load(std::memory_order_acquire);
    request->contentKeySnapshot = editedContentKey_;

    captureBeforeUndoSnapshot();
    pendingUndoDescription_ = TRANS("自动调音");

    correctionWorker_->enqueue(request);

    AppLogger::log("AutoTune: enqueued contentKey.objectId=" + juce::String(static_cast<juce::int64>(editedContentKey_.objectId))
        + " startFrame=" + juce::String(startFrame) + " endFrame=" + juce::String(endFrame));

    return { AutoTuneApplyStatus::Applied };
}

void PianoRollComponent::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) {
    if (scrollBar == &horizontalScrollBar_) {
        const double pps = camera_.pixelsPerSecond;
        const double newVisibleStart = newRangeStart / pps;
        userScrollHold_ = true;
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            newVisibleStart,
            0.0,
            pps);
        commitViewportRequest(req, juce::sendNotification);
    } else if (scrollBar == &verticalScrollBar_) {
        verticalScrollOffset_ = static_cast<float>(newRangeStart);
        refreshVerticalViewportGeometry();
    }
}

std::vector<Note> PianoRollComponent::getEditedContentNotesCopy() const {
    return cachedNotes_;
}

bool PianoRollComponent::isAutoTuneProcessing() const
{
    return autoTuneInFlight_.load(std::memory_order_acquire);
}

void PianoRollComponent::updateScrollBars() {
    int visibleWidth = getWidth() - pianoKeyWidth_ - UIColors::scrollBarThickness;
    visibleWidth = juce::jmax(1, visibleWidth);
    const double pps = camera_.pixelsPerSecond;

    const double visibleDuration = visibleWidth / pps;
    const double scrollbarEndSeconds = std::max(
        computeContentTimelineEndSeconds() + visibleDuration,
        camera_.visibleStartSeconds + visibleDuration);

    const auto range = TimelineViewportPolicy::computeViewportRange(
        0.0,
        scrollbarEndSeconds,
        camera_,
        visibleWidth,
        readPlayheadTime());

    horizontalScrollBar_.setRangeLimits(
        range.absoluteStartPx(),
        range.absoluteEndPx(),
        juce::dontSendNotification);
    horizontalScrollBar_.setCurrentRange(
        range.visibleStartPx(),
        range.visibleWidthPx(),
        juce::dontSendNotification);

    // Vertical
    float totalHeight = getTotalHeight();
    int visibleHeight = getHeight() - rulerHeight_ - UIColors::scrollBarThickness;
    visibleHeight = juce::jmax(1, visibleHeight);

    verticalScrollBar_.setRangeLimits(0.0, totalHeight, juce::dontSendNotification);
    verticalScrollBar_.setCurrentRange(verticalScrollOffset_, visibleHeight, juce::dontSendNotification);
}

// ============================================================================
// v12 New: Camera-based viewport functions
// ============================================================================

ViewMapper PianoRollComponent::makeViewMapper() const noexcept {
    return ViewMapper{
        camera_.visibleStartSeconds,
        camera_.pixelsPerSecond,
        pianoKeyWidth_,
        getTimelineContentViewportWidth(),
        getTimelineContentViewportHeight(),
        pixelsPerSemitone_,
        verticalScrollOffset_,
        maxMidi_
    };
}

double PianoRollComponent::computeContentTimelineEndSeconds() const noexcept {
    // Absolute timeline: always start from 0, no surface/domain semantics
    double maxEndSeconds = 0.0;

    for (const auto& placement : timelineContentPlacements_) {
        if (placement.isValid()) {
            maxEndSeconds = std::max(maxEndSeconds, placement.projection.timelineEndSeconds());
        }
    }

    // 无 placement 时使用 audio 或 notes 的实际 duration
    if (maxEndSeconds <= 0.0) {
        double duration = 0.0;
        if (audioBuffer_ && audioBuffer_->getNumSamples() > 0) {
            duration = static_cast<double>(audioBuffer_->getNumSamples()) / audioBufferSampleRate_;
        } else {
            const auto& notes = getCommittedNotes();
            for (const auto& note : notes) {
                if (note.endTime > duration)
                    duration = note.endTime;
            }
        }
        if (duration > 0.0)
            maxEndSeconds = duration;
    }

    return maxEndSeconds;
}

void PianoRollComponent::updatePlayheadVisibility()
{
    const auto viewportBounds = getTimelineViewportBounds();

    // Cont + playing + following → anchor at viewport center
    // Paused/manual/Page/seek → anchor at time-derived x
    const bool contFollowing = isPlaying_.load(std::memory_order_relaxed)
                            && scrollMode_ == ScrollMode::Continuous
                            && !userScrollHold_;

    const int anchorX = contFollowing
        ? viewportBounds.getCentreX()
        : makeViewMapper().timeToX(readPlayheadTime());

    const int pianoKeyW = shouldShowPianoKeys() ? pianoKeyWidth_ : 0;
    fixedPlayhead_.setAnchorBounds(anchorX, getHeight());
    fixedPlayhead_.setVisible(anchorX >= pianoKeyW && anchorX <= viewportBounds.getRight());
}

void PianoRollComponent::handleAsyncUpdate() {}

void PianoRollComponent::prepareVisiblePatternTiles()
{
    preparedPatternTiles_.clear();
    
    const auto viewport = getTimelineViewportBounds();
    const double pps = camera_.pixelsPerSecond;
    const double tileDurationSec = static_cast<double>(TimelinePatternCache::kPatternTileWidthPx) / pps;
    const double viewStart = camera_.visibleStartSeconds;
    const double viewEnd = viewStart + viewport.getWidth() / pps;
    
    // Floor to tile boundary
    double firstTileStart = std::floor(viewStart / tileDurationSec) * tileDurationSec;
    if (firstTileStart < 0.0) firstTileStart = 0.0;
    
    for (double tileStart = firstTileStart; tileStart < viewEnd; tileStart += tileDurationSec)
    {
        PatternTileKey key;
        key.viewKind = "pianoroll";
        key.startSeconds = tileStart;
        key.endSeconds = tileStart + tileDurationSec;
        key.pixelsPerSecond = pps;
        key.timeUnit = (timeUnit_ == TimeUnit::Bars) ? 1 : 0;
        key.tempo = static_cast<int>(bpm_);
        key.timeSigNumerator = timeSigNum_;
        key.timeSigDenominator = timeSigDenom_;
        key.themeId = static_cast<int>(UIColors::currentThemeId());
        key.verticalGeometry = encodePianoRollVerticalGeometry(
            pixelsPerSemitone_, 0, rulerHeight_,
            verticalScrollOffset_, getTimelineViewportBounds().getHeight());
        key.laneStyle = encodeLaneStyle(showLanes_, scaleRootNote_, scaleType_);
        
        const juce::Image& tile = patternCache_.getPatternTile(key, TimelineLayerComposer::buildPatternTile);
        preparedPatternTiles_.push_back({key, &tile});
    }
}

uint64_t PianoRollComponent::revisionForContentSlot(ContentSlot slot) const noexcept
{
    uint64_t r = editedContentEpoch_.load(std::memory_order_relaxed);

    if (slot == ContentSlot::Waveform)
    {
        // Mipmap build progress: re-generate tile when incremental build changes pixels
        if (const auto* mipmap = waveformMipmapCache_.get(editedContentKey_))
            r ^= static_cast<uint64_t>(mipmap->isComplete() ? 1 : 0) + static_cast<uint64_t>(
                std::llround(mipmap->getBuildProgress() * 10000.0)) * 31 + 0x9e3779b9 + (r << 6) + (r >> 2);
    }

    if (slot == ContentSlot::Notes)
    {
        r ^= notesEpoch_.load(std::memory_order_relaxed) * 31 + 0x9e3779b9 + (r << 6) + (r >> 2);
        r ^= static_cast<uint64_t>(noteNameMode_) + 0x9e3779b9 + (r << 6) + (r >> 2);
    }

    if (slot == ContentSlot::F0)
    {
        r ^= pitchEpoch_.load(std::memory_order_relaxed) * 31 + 0x9e3779b9 + (r << 6) + (r >> 2);
        r ^= static_cast<uint64_t>(showOriginalF0_) + 0x9e3779b9 + (r << 6) + (r >> 2);
        r ^= static_cast<uint64_t>(showCorrectedF0_) + 0x9e3779b9 + (r << 6) + (r >> 2);
        r ^= static_cast<uint64_t>(showUnvoicedFrames_) + 0x9e3779b9 + (r << 6) + (r >> 2);
    }

    // TimeGrid affects X mapping for all content
    r ^= timeGridEpoch_.load(std::memory_order_relaxed) * 31 + 0x9e3779b9 + (r << 6) + (r >> 2);

    return r;
}

std::vector<ContentSlot> PianoRollComponent::visibleContentSlots() const noexcept
{
    std::vector<ContentSlot> slots;
    if (showWaveform_)  slots.push_back(ContentSlot::Waveform);
    slots.push_back(ContentSlot::Notes);
    if (showOriginalF0_ || showCorrectedF0_ || showUnvoicedFrames_) slots.push_back(ContentSlot::F0);
    slots.push_back(ContentSlot::TimeAnchors);
    return slots;
}

void PianoRollComponent::prepareVisibleContentTiles()
{
    preparedContentTiles_.clear();

    const auto projection = activeContentProjection();
    if (!projection.isValid())
        return;

    const auto viewport = getTimelineViewportBounds();
    const double pps = camera_.pixelsPerSecond;
    const double tileDurationSec = static_cast<double>(TimelinePatternCache::kPatternTileWidthPx) / pps;
    const double viewStart = camera_.visibleStartSeconds;
    const double viewEnd = viewStart + viewport.getWidth() / pps;

    double firstTileStart = std::floor(viewStart / tileDurationSec) * tileDurationSec;
    if (firstTileStart < 0.0) firstTileStart = 0.0;

    const auto slots = visibleContentSlots();
    const int tileContentH = getTimelineContentViewportHeight();

    for (double tileStart = firstTileStart; tileStart < viewEnd; tileStart += tileDurationSec)
    {
        for (auto slot : slots)
        {
            ContentTileKey key;
            key.viewKind = "pianoroll";
            key.slot = slot;
            key.contentKey = editedContentKey_;
            key.startSeconds = tileStart;
            key.endSeconds = tileStart + tileDurationSec;
            key.pixelsPerSecond = pps;
            key.verticalGeometry = encodePianoRollVerticalGeometry(
                pixelsPerSemitone_, pianoKeyWidth_, rulerHeight_,
                verticalScrollOffset_, getTimelineContentViewportHeight());
            key.revision = revisionForContentSlot(slot);

            std::shared_ptr<const TimeGridSnapshot> timeAnchorsSnapshot;
            if (auto snap = readEditedSnapshot())
                timeAnchorsSnapshot = snap->timeGrid;

            const auto& tile = contentCache_.getOrBuildTile(key, tileContentH,
                [this, projection, tileStart, pps, slot, timeAnchorsSnapshot](juce::Graphics& g, const ContentTileKey& k, juce::Rectangle<int> bounds) {
                    PianoRollRenderer::RenderContext ctx;
                    ctx.width = bounds.getWidth();
                    ctx.height = bounds.getHeight();
                    ctx.pianoKeyWidth = 0;
                    ctx.rulerHeight = 0;
                    ctx.pixelsPerSecond = k.pixelsPerSecond;
                    ctx.pixelsPerSemitone = pixelsPerSemitone_;
                    ctx.minMidi = minMidi_;
                    ctx.maxMidi = maxMidi_;
                    ctx.bpm = bpm_;
                    ctx.scaleRootNote = scaleRootNote_;
                    ctx.scaleType = scaleType_;
                    ctx.noteNameMode = noteNameMode_;
                    ctx.showLanes = false;
                    ctx.showUnvoicedFrames = showUnvoicedFrames_;
                    ctx.showOriginalF0 = showOriginalF0_;
                    ctx.showCorrectedF0 = showCorrectedF0_;
                    ctx.timeUnit = (timeUnit_ == TimeUnit::Bars) ? PianoRollTimeUnit::Bars : PianoRollTimeUnit::Seconds;
                    ctx.activeProjection = projection;

                    ViewMapper tileCoords;
                    tileCoords.visibleStartSeconds = k.startSeconds;
                    tileCoords.pixelsPerSecond = k.pixelsPerSecond;
                    tileCoords.contentStartX = 0;
                    tileCoords.contentWidth = bounds.getWidth();
                    tileCoords.contentHeight = bounds.getHeight();
                    tileCoords.pixelsPerSemitone = pixelsPerSemitone_;
                    tileCoords.maxMidi = maxMidi_;
                    tileCoords.verticalScrollOffset = verticalScrollOffset_;
                    ctx.coords = tileCoords;
                    ctx.timeGridSnapshot = timeAnchorsSnapshot;

                    PianoRollRenderer::ContentRenderItem item;
                    item.contentKey = editedContentKey_;
                    item.projection = projection;
                    item.active = true;
                    item.audioBuffer = audioBuffer_;
                    item.displayNotes = getDisplayedNotes();

                    if (currentCurve_) {
                        item.pitchSnapshot = currentCurve_->getSnapshot();
                        if (item.pitchSnapshot && item.pitchSnapshot->size() > 0) {
                            item.f0Timeline = { item.pitchSnapshot->getHopSize(),
                                               item.pitchSnapshot->getSampleRate(),
                                               static_cast<int>(item.pitchSnapshot->size()) };
                            item.f0LOD = getOrBuildF0LOD(editedContentKey_, item.pitchSnapshot);
                        }
                    }

                    if (audioBuffer_) {
                        const auto* mipmap = waveformMipmapCache_.get(editedContentKey_);
                        if (mipmap && mipmap->hasSource()) {
                            int bestLevel = mipmap->selectBestLevelIndex(pps);
                            item.waveformSnapshot = mipmap->snapshotLevel(bestLevel);
                        }
                    }

                    switch (slot)
                    {
                        case ContentSlot::Waveform:
                            if (showWaveform_ && item.waveformSnapshot.peaks.size() > 0)
                                renderer_->drawWaveform(g, ctx, item);
                            break;
                        case ContentSlot::Notes:
                            renderer_->drawNotes(g, ctx, item);
                            break;
                        case ContentSlot::F0:
                            if (showUnvoicedFrames_)
                                renderer_->drawUnvoicedFrameBands(g, ctx, item);
                            renderer_->drawF0Curve(g, ctx, item);
                            break;
                        case ContentSlot::TimeAnchors:
                            renderer_->drawTimeGridAnchors(g, ctx);
                            break;
                        default:
                            break;
                    }
                });

            preparedContentTiles_.push_back({key, &tile});
        }
    }
}

void PianoRollComponent::drawPreparedPatternTiles(juce::Graphics& g)
{
    RenderParams params;
    const auto viewport = getTimelineViewportBounds();
    params.viewportBoundsX = pianoKeyWidth_;
    params.visibleStartSeconds = camera_.visibleStartSeconds;
    params.pixelsPerSecond = camera_.pixelsPerSecond;
    params.viewportWidth = viewport.getWidth();
    params.viewportHeight = viewport.getHeight();

    for (const auto& pt : preparedPatternTiles_) {
        if (!pt.image || !pt.image->isValid()) continue;
        TimelineLayerComposer::drawPatternTile(g, *pt.image, pt.key.startSeconds, params);
    }
}

void PianoRollComponent::drawPreparedContentTiles(juce::Graphics& g)
{
    RenderParams params;
    const auto viewport = getTimelineViewportBounds();
    params.viewportBoundsX = pianoKeyWidth_;
    params.visibleStartSeconds = camera_.visibleStartSeconds;
    params.pixelsPerSecond = camera_.pixelsPerSecond;
    params.viewportWidth = viewport.getWidth();
    params.viewportHeight = viewport.getHeight();
    params.contentOffsetY = rulerHeight_;

    for (const auto& pt : preparedContentTiles_) {
        if (!pt.image || !pt.image->isValid()) continue;
        TimelineLayerComposer::drawContentTile(g, *pt.image, pt.key.startSeconds, params);
    }
}

} // namespace OpenTune
