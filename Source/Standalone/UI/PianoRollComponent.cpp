#include "PianoRollComponent.h"
#include "../../Utils/LocalizationManager.h"
#include "../Utils/AppLogger.h"
#include "../../Utils/PianoRollEditAction.h"
#include "../../Utils/TimeGridEditAction.h"   // 鈿★笍 vocal-time-stretch 搂8.7
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
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
#include "../../Render/RenderChunkPlanner.h"
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

uint64_t hashCombineContentKey(uint64_t seed, ContentKey key) noexcept
{
    seed = hashCombine(seed, static_cast<uint64_t>(key.domainKind));
    seed = hashCombine(seed, key.objectId);
    return hashCombine(seed, key.sourceWindowDiscriminator);
}

constexpr double kPianoRollPinnedViewportRatio = 0.5;
constexpr double kPianoRollRenderBandOverscanScreens = 1.0;

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

    addAndMakeVisible(rulerSurface_);
    addAndMakeVisible(contentSurface_);
    addAndMakeVisible(previewOverlay_);
    addAndMakeVisible(playheadOverlay_);
    timeUnitToggleButton_.toFront(false);
    scrollModeToggleButton_.toFront(false);
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
    toolCtx.projectTimelineTimeToContent = [this](double timelineSeconds) {
        return projectTimelineTimeToContent(timelineSeconds);
    };
    toolCtx.projectContentTimeToTimeline = [this](double contentSeconds) {
        return projectContentTimeToTimeline(contentSeconds);
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
    toolCtx.invalidateContentVisual = [this]() {
        invalidateVisual(static_cast<uint32_t>(PianoRollVisualInvalidationReason::Content),
                         PianoRollVisualInvalidationPriority::Interactive);
    };
    toolCtx.repaintPreviewOverlay = [this]() { previewOverlay_.repaint(); };
    toolCtx.setMouseCursor = [this](const juce::MouseCursor& c) { setMouseCursor(c); };
    toolCtx.grabKeyboardFocus = [this]() { grabKeyboardFocus(); };
    toolCtx.getAudioEditingScheme = [this]() { return audioEditingScheme_; };
    toolCtx.notifyPlayheadChange = [this](double time) {
        listeners_.call([time](Listener& l) { l.playheadPositionChangeRequested(time); });
        userScrollHold_ = false;
        pendingSeekTime_ = time;
        const bool isPlaying = isPlaying_.load(std::memory_order_relaxed);
        playheadOverlay_.setPlayheadSeconds(time);
        updatePlayheadPresentationPolicy();
            // 鎾斁涓?seek锛氳缃?pending锛孷Blank 鐢?pending 鍊煎眳涓洿鍒?host 纭
        if (scrollMode_ == ScrollMode::Continuous || isPlaying) {
            // 绔嬪嵆灞呬腑鍒版柊浣嶇疆锛屼笉闇€瑕?smooth offset
            const int visibleWidth = getTimelineContentViewportWidth();
            if (visibleWidth > 0) {
                const int pinnedViewportX = getContinuousPinnedPlayheadViewportX();
                const int playheadContentX = static_cast<int>(std::llround(getPlayheadAbsolutePixelX(time)));
                const int centeredScroll = std::max(0, playheadContentX - (pinnedViewportX - pianoKeyWidth_));
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
    // 鈿★笍 vocal-time-stretch 搂8.7 鈥?Time tool / TimeGrid wiring
    // ============================================================
    toolCtx.getTimeGridSnapshot = [this]() -> std::shared_ptr<const TimeGridSnapshot> {
        auto snap = readEditedSnapshot();
        return snap ? snap->timeGrid : nullptr;
    };
    toolCtx.commitTimeGrid = [this](std::shared_ptr<const TimeGridSnapshot> newSnap,
                                     std::shared_ptr<const TimeGridSnapshot> oldSnap,
                                     int64_t affectedSrcStart,
                                     int64_t affectedSrcEnd,
                                     juce::String description) -> bool {
        if (processor_ == nullptr || !editedContentKey_.isValid()) return false;
        if (newSnap == nullptr) return false;

        auto action = std::make_unique<TimeGridEditAction>(
            contentCommands_,
            editedContentKey_,
            description.isNotEmpty() ? description : juce::String("缂栬緫鏃堕棿缃戞牸"),
            std::move(oldSnap),
            newSnap,
            affectedSrcStart,
            affectedSrcEnd);
        // First publish the new snapshot to the content (the action's redo()
        // will replay this); then push the action so undo() reverts.
        const bool published = contentCommands_->setTimeGrid(
            editedContentKey_, newSnap, affectedSrcStart, affectedSrcEnd);
        if (!published) return false;
        processor_->getUndoManager().addAction(std::move(action));
        return true;
    };
    toolCtx.notifyTimeGridChanged = [this]() {
        // 搂8.6 鈥?wider repaint via VisualInvalidation TimeGrid reason.
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
    pendingUndoDescription_ = TRANS("鑷姩璋冮煶");
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
            completed.vibratoRate,
            completed.audioSampleRate)) {
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
    if (!commitEditedContentCorrectedSegments(copyCorrectedSegments(completed.curve),
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
    refreshEditedContentNotes();
}

void PianoRollComponent::setContentCommands(std::shared_ptr<ContentEditCommands> commands)
{
    contentCommands_ = std::move(commands);
}


void PianoRollComponent::refreshEditedContentNotes()
{
    cachedNotes_.clear();
    cachedNotesRevision_ = 0;
    if (processor_ == nullptr || !editedContentKey_.isValid()) {
        return;
    }

    if (auto snap = readEditedSnapshot()) {
        cachedNotes_ = snap->notes;
        cachedNotesRevision_ = snap->notesRevision;
    }
    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
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

    // Pure note edits do not own a corrected-F0 range, so the undo snapshot covers the content.
    const auto success = commitEditedContentNotes(interactionState_.noteDraft.workingNotes,
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

bool PianoRollComponent::commitEditedContentNotes(const std::vector<Note>& notes,
                                                          F0FrameRange affectedRange)
{
    if (processor_ == nullptr || !editedContentKey_.isValid()) {
        return false;
    }

    if (!undoSnapshotCaptured_)
        captureBeforeUndoSnapshot();

    if (!contentCommands_->setNotes(editedContentKey_, notes)) {
        return false;
    }

    refreshEditedContentNotes();
    recordUndoAction(pendingUndoDescription_, affectedRange);
    return true;
}

bool PianoRollComponent::commitEditedContentNotesAndSegments(const std::vector<Note>& notes,
                                                             const std::vector<CorrectedSegment>& segments,
                                                             F0FrameRange affectedRange)
{
    if (processor_ == nullptr || !editedContentKey_.isValid()) {
        return false;
    }

    if (!undoSnapshotCaptured_)
        captureBeforeUndoSnapshot();

    if (!contentCommands_->commitNotesAndSegments(editedContentKey_, notes, segments)) {
        return false;
    }

    refreshEditedContentNotes();

    auto committedSnap = readEditedSnapshot();
    if (committedSnap != nullptr && committedSnap->pitchCurve != nullptr) {
        setEditedContent(editedContentKey_, committedSnap->pitchCurve, audioBuffer_, static_cast<int>(audioBufferSampleRate_));
    }

    recordUndoAction(pendingUndoDescription_, affectedRange);
    return true;
}

bool PianoRollComponent::commitEditedContentCorrectedSegments(const std::vector<CorrectedSegment>& segments,
                                                                        F0FrameRange affectedRange)
{
    if (processor_ == nullptr || !editedContentKey_.isValid()) {
        return false;
    }

    if (!undoSnapshotCaptured_)
        captureBeforeUndoSnapshot();

    if (!contentCommands_->setCorrectedSegments(editedContentKey_, segments)) {
        return false;
    }

    auto committedSnap = readEditedSnapshot();
    if (committedSnap != nullptr && committedSnap->pitchCurve != nullptr) {
        setEditedContent(editedContentKey_, committedSnap->pitchCurve, audioBuffer_, static_cast<int>(audioBufferSampleRate_));
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
    if (processor_ == nullptr || !editedContentKey_.isValid() || !undoSnapshotCaptured_)
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

    // F0FrameRange.endFrameExclusive 鏄紑鍖洪棿鍙崇锛汸ianoRollEditAction 鐨?
    // affectedEndFrame 鏄棴鍖洪棿鍙崇锛堝吋瀹规棦鏈?getter 璇箟锛夛紝鎹㈢畻 -1銆?
    const int affectedStart = std::max(0, affectedRange.startFrame);
    const int affectedEnd = std::max(affectedStart,
                                      affectedRange.endFrameExclusive > 0
                                          ? affectedRange.endFrameExclusive - 1
                                          : 0);

    auto action = std::make_unique<PianoRollEditAction>(
        contentCommands_,
        editedContentKey_,
        description.isNotEmpty() ? description : TRANS("缂栬緫"),
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
    const auto& notes = getCommittedNotes();
    const auto f0tl = currentF0Timeline();
    if (notes.empty() || f0tl.isEmpty()) {
        interactionState_.noteSelection.clear();
        interactionState_.selection.hasSelectionArea = false;
        interactionState_.selection.isSelectingArea = false;
        interactionState_.selection.clearF0Selection();
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content),
                         PianoRollVisualInvalidationPriority::Interactive);
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
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content),
                     PianoRollVisualInvalidationPriority::Interactive);

    return anyOverlap;
}

juce::Rectangle<int> PianoRollComponent::getNoteBounds(const Note& note) const
{
    const float adjustedPitch = note.getAdjustedPitch();
    if (adjustedPitch <= 0.0f) {
        return {};
    }

    const auto projection = activeContentProjection();
    if (projection.isValid()
        && (note.endTime <= 0.0
            || note.startTime >= projection.contentDurationSeconds)) {
        return {};
    }

    const int x1 = timeToX(projectContentTimeToTimeline(note.startTime));
    const int x2 = timeToX(projectContentTimeToTimeline(note.endTime));
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

    const int x1 = timeToX(projectContentTimeToTimeline(startTime));
    const int x2 = timeToX(projectContentTimeToTimeline(endTime));
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

    const int x1 = timeToX(projectContentTimeToTimeline(std::min(interactionState_.drawing.dirtyStartTime,
                                                                 interactionState_.drawing.dirtyEndTime)));
    const int x2 = timeToX(projectContentTimeToTimeline(std::max(interactionState_.drawing.dirtyStartTime,
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
        includePoint(static_cast<float>(timeToX(projectContentTimeToTimeline(anchor.time))), freqToY(anchor.freq));
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

        const float x = static_cast<float>(timeToX(projectContentTimeToTimeline(f0tl.timeAtFrame(frame))));
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

    // dirtyStartFrame/dirtyEndFrame 鏄墍鏈?manual ops 鐨?dirty 甯у苟闆嗭紙鍚鐐癸級銆?
    const F0FrameRange affectedRange{dirtyStartFrame,
                                      dirtyEndFrame >= dirtyStartFrame ? dirtyEndFrame + 1 : dirtyStartFrame};
    if (!commitEditedContentCorrectedSegments(copyCorrectedSegments(editedCurve), affectedRange)) {
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
    request->contentEpochSnapshot = editedContentEpoch_.load(std::memory_order_acquire);
    request->contentKeySnapshot = editedContentKey_;
    if (!undoSnapshotCaptured_)
        captureBeforeUndoSnapshot();

    correctionWorker_->enqueue(request);
}

// ============================================================================
// PianoRollPreviewOverlay 鈥?paint transient interaction previews
// ============================================================================

void PianoRollPreviewOverlay::paint(juce::Graphics& g)
{
    if (!owner_.renderModelCache_.isValid()) return;

    const auto& ctx = owner_.renderModelCache_.getRenderContext();
    const auto themeId = UIColors::currentThemeId();

    if (!ctx.isTimeView()) {
        bool hasActiveMat = false;
        for (const auto& item : ctx.contents) {
            if (!item.active) continue;
            hasActiveMat = true;
            break;
        }

        if (hasActiveMat || owner_.currentCurve_ != nullptr) {
            owner_.drawNoteDragCurvePreview(g);
            owner_.drawHandDrawPreview(g);
            owner_.drawLineAnchorPreview(g);
        }
    }

    // Draw note preview rectangle during DrawNote drag (option B: no noteDraft active)
    if (owner_.interactionState_.drawing.isDrawingNote
        && owner_.currentTool_ == ToolId::DrawNote) {
        double startTime = std::min(owner_.interactionState_.drawing.drawingNoteStartTime,
                                    owner_.interactionState_.drawing.drawingNoteEndTime);
        double endTime = std::max(owner_.interactionState_.drawing.drawingNoteStartTime,
                                  owner_.interactionState_.drawing.drawingNoteEndTime);
        float pitch = owner_.interactionState_.drawing.drawingNotePitch;

        if (pitch > 0.0f && endTime > startTime) {
            int x1 = owner_.timeToX(owner_.projectContentTimeToTimeline(startTime));
            int x2 = owner_.timeToX(owner_.projectContentTimeToTimeline(endTime));
            float midiNote = 69.0f + 12.0f * std::log2(pitch / 440.0f);
            float y = owner_.midiToY(midiNote);
            float noteHeight = owner_.pixelsPerSemitone_;

            juce::Rectangle<float> noteRect(static_cast<float>(std::min(x1, x2)),
                                            y,
                                            static_cast<float>(std::abs(x2 - x1)),
                                            noteHeight);

            // Semi-transparent preview note
            g.setColour(UIColors::noteBlockSelected.withAlpha(0.5f));
            g.fillRoundedRectangle(noteRect, 3.0f);
            g.setColour(UIColors::noteBlockSelected.withAlpha(0.8f));
            g.drawRoundedRectangle(noteRect, 3.0f, 1.5f);
        }
    }

    owner_.drawSelectionBox(g, themeId);
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
            float x = static_cast<float>(timeToX(projectContentTimeToTimeline(timePos)));

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

        const float x = static_cast<float>(timeToX(projectContentTimeToTimeline(f0tl.timeAtFrame(frame))));
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
        float x = static_cast<float>(timeToX(projectContentTimeToTimeline(anchor.time)));
        float y = freqToY(anchor.freq);

        g.setColour(anchorColour);
        g.fillEllipse(x - 2.0f, y - 2.0f, 4.0f, 4.0f);

        if (i > 0) {
            const auto& prev = interactionState_.drawing.pendingAnchors[i - 1];
            float prevX = static_cast<float>(timeToX(projectContentTimeToTimeline(prev.time)));
            float prevY = freqToY(prev.freq);
            g.setColour(anchorColour.withAlpha(0.7f));
            g.drawLine(prevX, prevY, x, y, 2.0f);
        }
    }

    if (!interactionState_.drawing.pendingAnchors.empty()) {
        const auto& last = interactionState_.drawing.pendingAnchors.back();
        float lastX = static_cast<float>(timeToX(projectContentTimeToTimeline(last.time)));
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

    int x1 = timeToX(projectContentTimeToTimeline(startTime));
    int x2 = timeToX(projectContentTimeToTimeline(endTime));
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
    // Fill full component to cover ruler area above the 12px-inset rounded clip.
    // The opaque gradient fill below fully covers this base color where applicable.
    g.fillAll(UIColors::rollBackground);

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
    // paint() ONLY consumes the prepared render model 鈥?no data preparation.
    // Cache refresh happens from state-change paths before repaint is requested.
    if (!renderModelCache_.isValid()) {
        // Safety net: cache not ready, draw background only. State-change callers
        // should refresh the prepared model before requesting repaint.
        paintBackgroundOnly(g);
        return;
    }
    // Fill full component to cover ruler area above the 12px-inset rounded clip.
    // The opaque gradient fill below fully covers this base color where applicable.
    g.fillAll(UIColors::rollBackground);

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

void PianoRollComponent::paintOverChildren(juce::Graphics& g)
{
    if (!renderModelCache_.isValid()) {
        return;
    }

    // 搂8.5 鈥?TimeGrid overlay uses live viewport coordinates (via
    // makePresentationRenderContext), not the cached render-band context.
    // The render-band timeToX carries a snapshot of scrollOffsetPx at
    // model-build time; when the user scrolls within the overscan band
    // without a model rebuild, the surface content moves via setBounds +
    // setImageOffsetX but the overlay must follow via current scroll.
    auto ctx = makePresentationRenderContext();
    const auto themeId = UIColors::currentThemeId();
    juce::ignoreUnused(themeId);

    // 鈿★笍 搂8.5 鈥?paint TimeGrid handles ABOVE chunk boundaries / waveform but
    // BELOW the piano keys (which sit on the left edge).  In Time view we
    // also force-render endpoint handles (even on identity grid) so the user
    // sees ClipStart / ClipEnd as anchor references.
    renderer_->drawTimeGridHandles(g, ctx);

    // 搂8.5 (Phase J) 鈥?Pitch view shows piano keys; Time view replaces the
    // left band with a dim spacer so the timeline aligns visually.
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
                retuneSpeed, vibratoDepth, vibratoRate, 44100.0);
            auto snap = clonedCurve->getSnapshot();

            const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(
                editRange.startFrame,
                editRange.endFrameExclusive,
                f0tl.endFrameExclusive());
            if (!commitEditedContentNotesAndSegments(notes, snap->getCorrectedSegments(), affectedRange)) {
                return false;
            }

            listeners_.call([affectedRange](Listener& l) { l.pitchCurveEdited(affectedRange.startFrame, affectedRange.endFrameExclusive - 1); });
            invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
            return true;
        } else {
            if (!commitEditedContentNotes(notes, currentFullF0Range())) {
                return false;
            }
        }
    } else {
        if (!commitEditedContentNotes(notes, currentFullF0Range())) {
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

    enqueueNoteBasedCorrectionAsync(getEditedContentNotesCopy(),
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
    pendingUndoDescription_ = TRANS("淇敼棰ら煶娣卞害");
    return applyVibratoParameterToSelection(VibratoParam::Depth, depth);
}

bool PianoRollComponent::applyVibratoRateToSelection(float rate) {
    pendingUndoDescription_ = TRANS("淇敼棰ら煶閫熺巼");
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
    const auto& allSegments = snapshot->getCorrectedSegments();

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return -1;
    const float tolerancePixels = 15.0f;
    const double clickTime = projectTimelineTimeToContent(xToTime(x));

    int bestIdx = -1;
    float bestDist = tolerancePixels;

    for (int i = 0; i < static_cast<int>(allSegments.size()); ++i) {
        const auto& seg = allSegments[i];
        if (seg.source != CorrectedSegment::Source::LineAnchor) continue;
        if (seg.f0Data.empty()) continue;

        const double startTime = f0tl.timeAtFrame(seg.startFrame);
        const double endTime = f0tl.timeAtFrame(seg.endFrame);

        const int startX = timeToX(projectContentTimeToTimeline(startTime));
        const int endX = timeToX(projectContentTimeToTimeline(endTime));

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
    // Note Split 鎺у埗闊抽珮鍒嗘闃堝€硷紙cents锛?
    segmentationPolicy_.transitionThresholdCents = juce::jlimit(
        OpenTune::PitchControlConfig::kMinNoteSplitCents,
        OpenTune::PitchControlConfig::kMaxNoteSplitCents,
        value);

    // Note Split 浠呮洿鏂板垎娈电瓥鐣ュ弬鏁帮紝涓嶈Е鍙?AUTO 閲嶆柊鐢熸垚銆?
    // AUTO 鎿嶄綔鐢辩敤鎴蜂富鍔ㄨЕ鍙戯紝浣跨敤褰撳墠绛栫暐鎵ц鍒嗘銆?
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
    timeUnitToggleButton_.toFront(false);
    scrollModeToggleButton_.toFront(false);

    updateRulerSurfaceBounds();
    updateContentSurfaceBounds();
    ensureRenderBandCoversCurrentViewport(true);
    previewOverlay_.setBounds(getLocalBounds());
    playheadOverlay_.setBounds(getLocalBounds());
    updatePlayheadPresentationPolicy();
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

    timeConverter_.setZoom(zoomLevel_);
    timeConverter_.setScrollOffset(scrollOffset_);
}

double PianoRollComponent::getContentDurationSeconds() const
{
    return activeContentProjection().contentDurationSeconds;
}

const PianoRollComponent::TimelineContentPlacement* PianoRollComponent::findActiveTimelineContentPlacement() const noexcept
{
    const auto activeIt = std::find_if(timelineContentPlacements_.begin(),
                                       timelineContentPlacements_.end(),
                                       [this](const auto& placement) {
                                           return placement.contentKey == editedContentKey_
                                               && placement.isValid();
                                       });
    if (activeIt != timelineContentPlacements_.end()) {
        return &(*activeIt);
    }

    const auto firstValidIt = std::find_if(timelineContentPlacements_.begin(),
                                           timelineContentPlacements_.end(),
                                           [](const auto& placement) { return placement.isValid(); });
    return firstValidIt != timelineContentPlacements_.end() ? &(*firstValidIt) : nullptr;
}

ContentTimelineProjection PianoRollComponent::activeContentProjection() const noexcept
{
    const auto* activePlacement = findActiveTimelineContentPlacement();
    if (activePlacement != nullptr) {
        return activePlacement->projection;
    }
    return !explicitTimelineContentPlacements_
        ? pendingSingleContentProjection_
        : ContentTimelineProjection{};
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

    if (!timelineViewDomain_.isValid()) {
        playheadOverlay_.setTimelineStartSeconds(timelineViewOriginSeconds());
    }
    userScrollHold_ = false;
    updatePlayheadPresentationPolicy();
    updateScrollBars();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                     PianoRollVisualInvalidationPriority::Interactive);
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
    if (!timelineViewDomain_.isValid()) {
        playheadOverlay_.setTimelineStartSeconds(timelineViewOriginSeconds());
    }
    prepareVisibleRenderModel();
    userScrollHold_ = false;
    updatePlayheadPresentationPolicy();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                     PianoRollVisualInvalidationPriority::Interactive);
}

void PianoRollComponent::setTimelineContentPlacements(std::vector<TimelineContentPlacement> placements)
{
    if (applyTimelineContentPlacements(std::move(placements), true)) {
        pendingSingleContentProjection_ = activeContentProjection();
        updatePlayheadPresentationPolicy();
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
    updatePlayheadPresentationPolicy();
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
    updatePlayheadPresentationPolicy();
    updateScrollBars();
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport),
                     PianoRollVisualInvalidationPriority::Interactive);
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

    const bool hasAudio = (buffer != nullptr);

    if (contentChanged) {
        editedContentKey_ = contentKey;
        clearNoteDraft();
        autoTuneInFlight_.store(false, std::memory_order_release);
        pendingUndoDescription_ = {};
        beforeUndoNotes_.clear();
        beforeUndoSegments_.clear();
        undoSnapshotCaptured_ = false;
    }

    // notes 涓?pitchCurve 閫氳繃 commitNotesAndPitchCurve 鍚屽啓鍒?store锛?
    // 璇讳晶涔熷繀椤诲悓璇伙細curveChanged 鏃跺繀椤?refresh notes锛屽惁鍒?undo/redo 浼?
    // 鍑虹幇 curve 鍥為€€浣?notes 瑙嗚娈嬬暀鐨勪笉瀵圭О锛坈achedNotes_ 婊炲悗锛夈€?
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

    if (hasAudio && (contentChanged || bufferChanged)) {
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
    if (pendingVisualInvalidation_.hasWork()) {
        const uint32_t modelReasons =
            toInvalidationMask(PianoRollVisualInvalidationReason::Viewport)
            | toInvalidationMask(PianoRollVisualInvalidationReason::Content)
            | toInvalidationMask(PianoRollVisualInvalidationReason::Interaction)
            | toInvalidationMask(PianoRollVisualInvalidationReason::Decoration)
            | toInvalidationMask(PianoRollVisualInvalidationReason::TimeGrid);
        if ((pendingVisualInvalidation_.reasonsMask & modelReasons) != 0) {
            prepareVisibleRenderModel();
        }
    }

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
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content),
                     PianoRollVisualInvalidationPriority::Normal);
}

void PianoRollComponent::setScrollOffset(int offset) {
    const int newOffset = juce::jmax(0, offset);
    if (newOffset == scrollOffset_) return;

    const int oldOffset = scrollOffset_;
    const auto viewportState = makeTimelineViewportState();

    scrollOffset_ = newOffset;
    timeConverter_.setScrollOffset(scrollOffset_);
    playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));
    horizontalScrollBar_.setCurrentRangeStart(scrollOffset_, juce::dontSendNotification);
    updatePlayheadPresentationPolicy();
    updateRulerSurfaceBounds();
    updateContentSurfaceBounds();

    const bool requiresFullRedrawForDelta = viewportState.requiresFullRedrawForDelta(oldOffset, newOffset);
    const bool rebuiltRenderBand = ensureRenderBandCoversCurrentViewport(false);
    const bool requiresFullRepaint = requiresFullRedrawForDelta || rebuiltRenderBand;

    if (requiresFullRepaint) {
        FrameScheduler::instance().requestContentInvalidation(*this,
                                                              getLocalBounds(),
                                                              FrameScheduler::Priority::Interactive);
        return;
    }

    rulerSurface_.repaint();
    contentSurface_.repaint();
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
    const auto projection = activeContentProjection();
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
                invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
            }
        }
    } else {
        waveformBuildTickCounter_ = 0;
        waveformVisualRefreshPending_ = false;
    }

    if (!playingNow && waveformVisualRefreshPending_) {
        waveformVisualRefreshPending_ = false;
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
    }

    flushPendingVisualInvalidation();
}

void PianoRollComponent::onScrollVBlankCallback(double timestampSec)
{
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
        // is intentionally skipped 鈥?when paused, the user controls the view.
        const double stoppedPresentationTime = pendingSeekTime_ >= 0.0 ? pendingSeekTime_ : hostTime;
        resetPresentationClock(stoppedPresentationTime);
        updatePlayheadPresentationPolicy();
        playheadOverlay_.setPlayheadSeconds(stoppedPresentationTime);
        return;
    }

    // 濡傛灉鏈?pending seek锛屾鏌?host 鏄惁宸茬‘璁わ紙position 鎺ヨ繎 pending 鍊硷級
    lastObservedRawPlayheadTime_ = rawHostTime;
    double playheadTime;
    if (pendingSeekTime_ >= 0.0) {
        if (std::abs(hostTime - pendingSeekTime_) < 0.05) {
            // Host 宸茬‘璁?seek
            pendingSeekTime_ = -1.0;
            playheadTime = hostTime;
        } else {
            // Host 灏氭湭纭锛屼娇鐢?pending 鍊硷紙妯℃嫙鎾斁澶翠粠 seek 鐐瑰墠杩涳級
            playheadTime = pendingSeekTime_;
        }
    } else {
        playheadTime = hostTime;
    }

    updatePresentationClock(playheadTime, timestampSec);
    const double displayPlayheadTime = getDisplayPlayheadTime(timestampSec);

    const int visibleWidth = getTimelineContentViewportWidth();
    if (visibleWidth <= 0) {
        updatePlayheadPresentationPolicy();
        playheadOverlay_.setPlayheadSeconds(displayPlayheadTime);
        return;
    }

    if (scrollMode_ == ScrollMode::Continuous) {
        if (!userScrollHold_) {
            const int pinnedViewportX = getContinuousPinnedPlayheadViewportX();
            const int playheadContentX = static_cast<int>(std::llround(getPlayheadAbsolutePixelX(displayPlayheadTime)));
            const int desiredScroll = playheadContentX - (pinnedViewportX - pianoKeyWidth_);
            const int maxScroll = getMaxHorizontalScroll();

            if (desiredScroll >= 0 && desiredScroll <= maxScroll) {
                // Scrollable range: pin playhead, scroll content
                if (desiredScroll != scrollOffset_)
                    setScrollOffset(desiredScroll);
            } else {
                // Boundary: unpin playhead, let it move freely across viewport
                playheadOverlay_.clearPinnedViewportX();
                playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));
                const int clampedScroll = juce::jlimit(0, maxScroll, desiredScroll);
                if (clampedScroll != scrollOffset_)
                    setScrollOffset(clampedScroll);
            }
        }
    }
    else if (scrollMode_ == ScrollMode::Page) {
        const int absX = static_cast<int>(std::llround(getPlayheadAbsolutePixelX(displayPlayheadTime)));
        const int pageIndex = visibleWidth > 0 ? juce::jmax(0, absX / visibleWidth) : 0;
        const int newScroll = pageIndex * visibleWidth;
        if (newScroll != scrollOffset_)
            setScrollOffset(newScroll);
    }

    updatePlayheadPresentationPolicy();
    playheadOverlay_.setPlayheadSeconds(displayPlayheadTime);
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

    // 鈿★笍 vocal-time-stretch 搂8.4 (Phase F) 鈥?Time tool is mutually exclusive
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
            // 搂8.4: Time tool uses normal cursor + per-handle hover hand cursor
            // applied by handleTimeToolMouseMove (via ctx.setMouseCursor).
            setMouseCursor(juce::MouseCursor::NormalCursor);
            break;
    }

    // 閫氱煡鐩戝惉鑰呭伐鍏峰凡鍒囨崲锛堝弬鏁伴潰鏉块渶瑕佸悓姝ユ寜閽珮浜級
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

    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Interaction),
                     getLocalBounds(),
                     PianoRollVisualInvalidationPriority::Interactive);
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
    toolHandler_->mouseMove(e);
}

void PianoRollComponent::mouseDoubleClick(const juce::MouseEvent& e) {
    // 鈿★笍 vocal-time-stretch 搂8.4 鈥?Time tool double-click forwarded to handler.
    // Other tools currently have no double-click semantics, so the handler
    // ignores them by switching on currentTool_.
    toolHandler_->mouseDoubleClick(e);
}

void PianoRollComponent::mouseDown(const juce::MouseEvent& e) {
    if (isAutoTuneProcessing()) {
        return;
    }

    // Ctrl+drag panning 鈥?only on non-interactive area, so existing
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

    // Piano key audition: click in piano key area triggers note preview.
    if (shouldShowPianoKeys() && e.x < pianoKeyWidth_) {
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
    if (shouldShowPianoKeys() && pressedPianoKey_ >= 0) {
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
    const TimelineContentPlacement& placement,
    double visibleTimeStart,
    double visibleTimeEnd,
    int viewportStartX,
    int viewportEndX,
    int renderScrollOffsetPx,
    int renderPianoKeyWidth) const
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
        item.selectedNoteIndices = interactionState_.noteSelection.selectedIndices;
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

        if (item.pitchSnapshot != nullptr && !item.f0Timeline.isEmpty()) {
            const double visibleContentStart = juce::jlimit(0.0,
                                                                    placement.projection.contentDurationSeconds,
                                                                    visibleTimeStart - placement.projection.timelineStartSeconds);
            const double visibleContentEnd = juce::jlimit(visibleContentStart,
                                                                  placement.projection.contentDurationSeconds,
                                                                  visibleTimeEnd - placement.projection.timelineStartSeconds);
            const auto visibleFrames = item.f0Timeline.rangeForTimesWithMargin(visibleContentStart,
                                                                               visibleContentEnd,
                                                                               10);
            const int startFrame = juce::jlimit(0,
                                                static_cast<int>(item.pitchSnapshot->size()),
                                                visibleFrames.startFrame);
            const int endFrame = juce::jlimit(startFrame,
                                              static_cast<int>(item.pitchSnapshot->size()),
                                              visibleFrames.endFrameExclusive);

            PianoRollRenderer::F0VisualBuildOptions visualOptions;
            visualOptions.startFrame = startFrame;
            visualOptions.endFrameExclusive = endFrame;
            visualOptions.viewportStartX = viewportStartX;
            visualOptions.viewportEndX = viewportEndX;

            const auto& originalF0 = item.pitchSnapshot->getOriginalF0();
            const auto& originalEnergy = item.pitchSnapshot->getOriginalEnergy();
            item.originalF0VisualSegments = PianoRollRenderer::buildF0VisualSegments(
                originalF0,
                &originalEnergy,
                nullptr,
                visualOptions,
                [&item, this, renderScrollOffsetPx, renderPianoKeyWidth](int frame) -> float {
                    return static_cast<float>(timeToXForRenderScroll(
                        item.projection.timelineStartSeconds + item.f0Timeline.timeAtFrame(frame),
                        renderScrollOffsetPx,
                        renderPianoKeyWidth));
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
                    [&item, this, renderScrollOffsetPx, renderPianoKeyWidth](int frame) -> float {
                        return static_cast<float>(timeToXForRenderScroll(
                            item.projection.timelineStartSeconds + item.f0Timeline.timeAtFrame(frame),
                            renderScrollOffsetPx,
                            renderPianoKeyWidth));
                    },
                    [this](int, float frequency) -> float {
                        return midiToY(freqToMidi(frequency));
                    });
            }
        }
    }

    if (item.audioBuffer != nullptr) {
        auto& mipmap = waveformMipmapCache_.getOrCreate(placement.contentKey);
        if (mipmap.isSourceChanged(item.audioBuffer))
            mipmap.setAudioSource(item.audioBuffer);
        item.waveformMipmap = &mipmap;
    }

    if (showChunkBoundaries_ && processor_) {
        auto snap = processor_->getContentSnapshot(placement.contentKey);
        if (snap && snap->audioBuffer) {
            const int totalSamples = snap->audioBuffer->getNumSamples();
            constexpr int defaultHop = 512;
            auto boundaries = RenderChunkPlanner::buildChunkBoundariesFromSilentGaps(
                totalSamples, snap->silentGaps, defaultHop);
            item.chunkBoundaries.reserve(boundaries.size());
            for (const auto sample : boundaries) {
                item.chunkBoundaries.push_back(
                    TimeCoordinate::samplesToSeconds(sample, TimeCoordinate::kRenderSampleRate));
            }
        }
    }

    return item;
}

void PianoRollComponent::visibilityChanged()
{
    // 褰撶粍浠跺彉涓哄彲瑙佹椂锛岃嚜鍔ㄨ幏鍙栭敭鐩樼劍鐐?
    // 杩欑‘淇濈敤鎴锋棤闇€鎵嬪姩鐐瑰嚮鍗冲彲浣跨敤蹇嵎閿紙濡?Ctrl+A 鍏ㄩ€夛級
    if (isShowing() && isVisible())
    {
        // 浣跨敤 callAfterDelay 纭繚鍦ㄦ秷鎭惊鐜鐞嗗畬鎴愬悗鑾峰彇鐒︾偣
        // 杩欐槸蹇呰鐨勶紝鍥犱负缁勪欢鍒氬垰鏄剧ず鏃跺彲鑳借繕涓嶈兘绔嬪嵆鎺ユ敹鐒︾偣
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
    ++visualPrefsRevision_;
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content),
                     PianoRollVisualInvalidationPriority::Interactive);
}

PianoRollRenderer::RenderContext PianoRollComponent::buildRenderContext(double visibleTimeStart,
                                                                        double visibleTimeEnd,
                                                                        int viewportStartX,
                                                                        int viewportEndX,
                                                                        int renderScrollOffsetPx,
                                                                        int renderWidthPx,
                                                                        int renderPianoKeyWidth) const
{
    PianoRollRenderer::RenderContext ctx;
    ctx.width = renderWidthPx;
    ctx.height = getTimelineViewportBounds().getBottom();
    ctx.pianoKeyWidth = renderPianoKeyWidth;
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

    ctx.contents.reserve(timelineContentPlacements_.size());
    for (const auto& placement : timelineContentPlacements_)
        if (placement.isValid())
            ctx.contents.push_back(buildContentRenderItem(placement,
                                                                         visibleTimeStart,
                                                                         visibleTimeEnd,
                                                                         viewportStartX,
                                                                         viewportEndX,
                                                                         renderScrollOffsetPx,
                                                                         renderPianoKeyWidth));

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
    ctx.xToTime = [this, renderScrollOffsetPx, renderPianoKeyWidth](int x) {
        return xToTimeForRenderScroll(x, renderScrollOffsetPx, renderPianoKeyWidth);
    };
    ctx.timeToX = [this, renderScrollOffsetPx, renderPianoKeyWidth](double seconds) {
        return timeToXForRenderScroll(seconds, renderScrollOffsetPx, renderPianoKeyWidth);
    };

    ctx.hasF0Selection = interactionState_.selection.hasF0Selection;
    ctx.f0SelectionStartFrame = interactionState_.selection.selectedF0StartFrame;
    ctx.f0SelectionEndFrameExclusive = interactionState_.selection.selectedF0EndFrameExclusive;

    // 鈿★笍 vocal-time-stretch 搂8.7 鈥?inject TimeGrid snapshot for 搂8.5 renderer.
    // During an active drag, prefer the working snapshot for live preview;
    // otherwise pull from the processor's published TimeGrid.
    if (interactionState_.timeTool.isDraggingHandle
        && interactionState_.timeTool.dragWorkingSnapshot != nullptr) {
        ctx.timeGridSnapshot = interactionState_.timeTool.dragWorkingSnapshot;
    } else if (auto snap = readEditedSnapshot()) {
        ctx.timeGridSnapshot = snap->timeGrid;
    }
    ctx.timeGridHoveredHandleId  = interactionState_.timeTool.hoveredHandleId;
    ctx.timeGridSelectedHandleId = interactionState_.timeTool.selectedHandleId;
    // 搂8.5 (Phase J) 鈥?currentTool drives view-mode in renderer.
    ctx.currentTool = currentTool_;

    // 搂8.5 (Phase I): TimeGrid handles 浣跨敤 content-local
    // output_seconds锛岄渶瑕佹姇褰卞埌 timeline time 鍐嶉€氳繃 timeToX 杞睆骞曞潗鏍囥€?
    ctx.contentTimeToTimeline = [this](double contentSeconds) {
        return projectContentTimeToTimeline(contentSeconds);
    };

    ctx.referenceOverlay = referenceOverlay_;

    return ctx;
}

void PianoRollComponent::prepareVisibleRenderModel() const {
    refreshPreparedRenderModel(false);
}

void PianoRollComponent::refreshPreparedRenderModel(bool forceRebuild) const
{
    const auto timelineViewportBounds = getTimelineViewportBounds();
    const int viewportStartX = pianoKeyWidth_;
    const int viewportEndX = timelineViewportBounds.getRight();
    rebuildPreparedRenderModelForViewport(viewportStartX, viewportEndX, forceRebuild);
}

TimelineViewportState PianoRollComponent::makeTimelineViewportState() const
{
    const auto viewportBounds = getTimelineViewportBounds();
    TimelineViewportState viewportState;
    viewportState.zoomLevel = zoomLevel_;
    viewportState.scrollOffsetPx = scrollOffset_;
    viewportState.viewportWidthPx = juce::jmax(0, viewportBounds.getWidth() - pianoKeyWidth_);
    viewportState.viewportHeightPx = viewportBounds.getHeight();
    viewportState.contentStartX = pianoKeyWidth_;
    return viewportState;
}

int PianoRollComponent::getTimelineContentViewportWidth() const
{
    return juce::jmax(0, getTimelineViewportBounds().getWidth() - pianoKeyWidth_);
}

int PianoRollComponent::getContinuousPinnedPlayheadViewportX() const
{
    return pianoKeyWidth_ + (getTimelineContentViewportWidth() / 2);
}

int PianoRollComponent::getMaxHorizontalScroll() const
{
    const int visibleWidth = getTimelineContentViewportWidth();
    const int totalContentWidth = static_cast<int>(horizontalScrollBar_.getRangeLimit().getEnd());
    return juce::jmax(0, totalContentWidth - visibleWidth);
}

double PianoRollComponent::getDisplayPlayheadTime(double timestampSec) const
{
    if (!presentationClockPrimed_)
        return lastAuthoritativePlayheadTime_;

    const double elapsed = juce::jmax(0.0, timestampSec - presentationClockAnchorTimestampSec_);
    return juce::jmax(0.0, presentationClockAnchorTime_ + elapsed);
}

void PianoRollComponent::updatePresentationClock(double authoritativeTime, double timestampSec)
{
    if (!presentationClockPrimed_ || !isPlaying_.load(std::memory_order_relaxed)) {
        resetPresentationClock(authoritativeTime);
        presentationClockAnchorTimestampSec_ = timestampSec;
        presentationClockLastObservationTimestampSec_ = timestampSec;
        return;
    }

    const double predictedNow = getDisplayPlayheadTime(timestampSec);
    const double predictionError = authoritativeTime - predictedNow;
    const double authoritativeDelta = authoritativeTime - lastAuthoritativePlayheadTime_;
    const double observationGap = juce::jmax(0.0, timestampSec - presentationClockLastObservationTimestampSec_);

    const bool discontinuity = authoritativeDelta < -0.001
        || authoritativeDelta > observationGap + 0.050
        || std::abs(predictionError) > 0.050;

    if (discontinuity) {
        presentationClockAnchorTime_ = authoritativeTime;
        presentationClockAnchorTimestampSec_ = timestampSec;
    }

    lastAuthoritativePlayheadTime_ = authoritativeTime;
    presentationClockLastObservationTimestampSec_ = timestampSec;
}

void PianoRollComponent::resetPresentationClock(double authoritativeTime)
{
    presentationClockPrimed_ = true;
    lastAuthoritativePlayheadTime_ = authoritativeTime;
    presentationClockAnchorTime_ = authoritativeTime;
    presentationClockAnchorTimestampSec_ = juce::Time::getMillisecondCounterHiRes() * 0.001;
    presentationClockLastObservationTimestampSec_ = presentationClockAnchorTimestampSec_;
}

PianoRollRenderer::RenderContext PianoRollComponent::makePresentationRenderContext() const
{
    auto ctx = renderModelCache_.getRenderContext();
    // Use the component's canonical timeToX/xToTime which apply
    // toVisibleTimelineSeconds() 鈥?essential when the active placement
    // has a non-zero timeline origin.
    ctx.timeToX = [this](double seconds) { return timeToX(seconds); };
    ctx.xToTime = [this](int x) { return xToTime(x); };
    const auto viewportBounds = getTimelineViewportBounds();
    ctx.width = viewportBounds.getWidth();
    ctx.height = viewportBounds.getHeight();
    ctx.pianoKeyWidth = pianoKeyWidth_;
    ctx.rulerHeight = rulerHeight_;
    ctx.pixelsPerSecond = getTimelinePixelsPerSecond();
    return ctx;
}

void PianoRollComponent::updatePlayheadPresentationPolicy()
{
    const bool wantPin = scrollMode_ == ScrollMode::Continuous
        && isPlaying_.load(std::memory_order_relaxed)
        && !userScrollHold_;

    if (!wantPin) {
        playheadOverlay_.clearPinnedViewportX();
        return;
    }

    const int pinnedViewportX = getContinuousPinnedPlayheadViewportX();
    const int maxScroll = getMaxHorizontalScroll();

    if (maxScroll <= 0) {
        // Content fits within viewport 鈥?no scrolling possible, let playhead move freely
        playheadOverlay_.clearPinnedViewportX();
        playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));
        return;
    }

    const double playheadTime = lastAuthoritativePlayheadTime_;
    const int playheadContentX = static_cast<int>(std::llround(getPlayheadAbsolutePixelX(playheadTime)));
    const int desiredScroll = playheadContentX - (pinnedViewportX - pianoKeyWidth_);

    if (desiredScroll >= 0 && desiredScroll <= maxScroll) {
        playheadOverlay_.setPinnedViewportX(static_cast<double>(pinnedViewportX));
    } else {
        playheadOverlay_.clearPinnedViewportX();
        playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));
    }
}

bool PianoRollComponent::preparedRenderBandCoversViewport(int viewportStartX, int viewportEndX) const
{
    if (!renderModelCache_.isValid()) {
        return false;
    }

    const int64_t viewportStartMs = secondsToMs(xToTime(viewportStartX));
    const int64_t viewportEndMs = secondsToMs(xToTime(viewportEndX));
    return viewportStartMs >= preparedBandStartMs_
        && viewportEndMs <= preparedBandEndMs_;
}

bool PianoRollComponent::renderBandNeedsRebuild(int contentViewportWidth, int viewportHeight) const
{
    if (!renderBand_.valid) {
        return true;
    }

    const int targetWidth = juce::jmax(contentViewportWidth,
                                       contentViewportWidth * 3);
    return renderBand_.widthPx != targetWidth
        || renderBand_.heightPx != viewportHeight;
}

bool PianoRollComponent::ensureRenderBandCoversCurrentViewport(bool forceRebuild) const
{
    const auto timelineViewportBounds = getTimelineViewportBounds();
    const int contentViewportWidth = juce::jmax(0, timelineViewportBounds.getWidth() - pianoKeyWidth_);
    const int viewportHeight = timelineViewportBounds.getHeight();
    if (contentViewportWidth <= 0 || viewportHeight <= 0) {
        renderBand_.valid = false;
        rulerSurface_.clearSurfaceImage();
        contentSurface_.clearSurfaceImage();
        return false;
    }

    const int viewportStartX = pianoKeyWidth_;
    const int viewportEndX = timelineViewportBounds.getRight();
    const bool needsGeometryRebuild = renderBandNeedsRebuild(contentViewportWidth, viewportHeight);
    const bool coversViewport = preparedRenderBandCoversViewport(viewportStartX, viewportEndX);

    if (forceRebuild || needsGeometryRebuild || !coversViewport) {
        const int overscanPx = juce::jmax(contentViewportWidth,
                                          static_cast<int>(std::lround(contentViewportWidth * kPianoRollRenderBandOverscanScreens)));
        renderBand_.startContentX = juce::jmax(0, scrollOffset_ - overscanPx);
        renderBand_.widthPx = juce::jmax(contentViewportWidth, contentViewportWidth + overscanPx * 2);
        renderBand_.heightPx = viewportHeight;
        renderBand_.valid = true;
    }

    const int bandScrollOffsetPx = juce::jmax(0, renderBand_.startContentX);
    const int bandViewportStartX = pianoKeyWidth_;
    const int bandViewportEndX = pianoKeyWidth_ + renderBand_.widthPx;
    const double visibleTimeStart = xToTimeForRenderScroll(bandViewportStartX, bandScrollOffsetPx, pianoKeyWidth_);
    const double visibleTimeEnd = xToTimeForRenderScroll(bandViewportEndX, bandScrollOffsetPx, pianoKeyWidth_);

    uint64_t placementRevision = 1469598103934665603ull;
    for (const auto& placement : timelineContentPlacements_) {
        placementRevision = hashCombine(placementRevision, placement.contentKey.objectId);
        placementRevision = hashCombine(placementRevision, static_cast<uint64_t>(secondsToMs(placement.projection.timelineStartSeconds)));
        placementRevision = hashCombine(placementRevision, static_cast<uint64_t>(secondsToMs(placement.projection.timelineDurationSeconds)));
        placementRevision = hashCombine(placementRevision, static_cast<uint64_t>(secondsToMs(placement.projection.contentDurationSeconds)));
    }

    PianoRollRenderModelCache::Key cacheKey;
    cacheKey.contentKey = editedContentKey_;
    cacheKey.pitchEpoch = editedContentEpoch_.load(std::memory_order_relaxed);
    cacheKey.notesEpoch = cachedNotesRevision_;
    cacheKey.visualPrefsRevision = visualPrefsRevision_;
    cacheKey.timeGridRevision = [this]() -> uint64_t {
        auto snap = readEditedSnapshot();
        return snap ? snap->timeGridRevision : 0;
    }();
    cacheKey.visibleStartBandMs = secondsToMs(visibleTimeStart);
    cacheKey.visibleEndBandMs = secondsToMs(visibleTimeEnd);
    const auto projection = activeContentProjection();
    cacheKey.projectionStartMs = secondsToMs(projection.timelineStartSeconds);
    cacheKey.projectionDurationMs = secondsToMs(projection.timelineDurationSeconds);
    cacheKey.placementProjectionRevision = placementRevision;
    cacheKey.zoomBucket = static_cast<int>(zoomLevel_ * 100.0 + 0.5);
    cacheKey.verticalZoomBucket = quantizeGeometryPx(pixelsPerSemitone_);
    cacheKey.verticalScrollBucket = quantizeGeometryPx(verticalScrollOffset_);
    cacheKey.viewportSizeRevision = viewportSizeRevision_;
    cacheKey.interactionEpoch = interactionRevision_;

    bool rebuiltModel = false;
    if (forceRebuild || !renderModelCache_.isValid() || renderModelCache_.getCurrentKey() != cacheKey) {
        auto ctx = buildRenderContext(visibleTimeStart,
                                      visibleTimeEnd,
                                      bandViewportStartX,
                                      bandViewportEndX,
                                      bandScrollOffsetPx,
                                      bandViewportEndX,
                                      pianoKeyWidth_);
        renderModelCache_.rebuild(cacheKey, std::move(ctx));
        preparedBandStartMs_ = cacheKey.visibleStartBandMs;
        preparedBandEndMs_ = cacheKey.visibleEndBandMs;
        FrameScheduler::instance().recordRenderModelRebuild(FrameScheduler::TimelineReason::ContentModelInvalid);
        rebuiltModel = true;
    }

    if (rebuiltModel || forceRebuild || needsGeometryRebuild || !coversViewport) {
        rebuildRulerSurface();
        rebuildContentSurface();
    }

    updateRulerSurfaceBounds();
    updateContentSurfaceBounds();
    return rebuiltModel || needsGeometryRebuild || !coversViewport;
}

void PianoRollComponent::rebuildRulerSurface() const
{
    if (!renderBand_.valid || !renderModelCache_.isValid()) {
        rulerSurface_.clearSurfaceImage();
        return;
    }

    rulerSurfaceImage_ = juce::Image(juce::Image::ARGB,
                                     juce::jmax(1, renderBand_.widthPx),
                                     juce::jmax(1, rulerHeight_),
                                     true);

    juce::Graphics g(rulerSurfaceImage_);
    auto ctx = renderModelCache_.getRenderContext();
    ctx.width = renderBand_.widthPx + pianoKeyWidth_;
    ctx.height = rulerHeight_;
    renderer_->drawTimeRuler(g, ctx);
    rulerSurface_.setSurfaceImage(rulerSurfaceImage_);
}

void PianoRollComponent::rebuildContentSurface() const
{
    if (!renderBand_.valid || !renderModelCache_.isValid()) {
        contentSurface_.clearSurfaceImage();
        return;
    }

    const int contentHeight = juce::jmax(1, renderBand_.heightPx - rulerHeight_);
    contentSurfaceImage_ = juce::Image(juce::Image::ARGB,
                                       juce::jmax(1, renderBand_.widthPx),
                                       contentHeight,
                                       true);

    juce::Graphics g(contentSurfaceImage_);
    const auto& ctx = renderModelCache_.getRenderContext();

    {
        const juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(juce::Rectangle<int>(ctx.pianoKeyWidth,
                                                0,
                                                juce::jmax(0, ctx.width - ctx.pianoKeyWidth),
                                                contentHeight));
        // Translate component-space coordinates into content-local (ruler excluded).
        g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(-rulerHeight_)));

        renderer_->drawGridLines(g, ctx);
        for (const auto& item : ctx.contents)
            renderer_->drawUnvoicedFrameBands(g, ctx, item);

        if (ctx.referenceOverlay.has_value() && ctx.referenceOverlay->enabled) {
            renderer_->drawGhostNotes(g, ctx, *ctx.referenceOverlay);
            renderer_->drawGhostAnchors(g, ctx, *ctx.referenceOverlay);
        }

        if (showWaveform_) {
            for (const auto& item : ctx.contents)
                renderer_->drawWaveform(g, ctx, item);
        }

        if (!ctx.isTimeView()) {
            renderer_->drawLanes(g, ctx);

            for (const auto& item : ctx.contents)
                renderer_->drawNotes(g, ctx, item);

            for (const auto& item : ctx.contents) {
                if (item.pitchSnapshot == nullptr)
                    continue;

                if (showOriginalF0_ && !item.originalF0VisualSegments.empty())
                    renderer_->drawPreparedF0Curve(g, item.originalF0VisualSegments, UIColors::originalF0, 0.78f, true, ctx);

                if (showCorrectedF0_ && !item.correctedF0VisualSegments.empty())
                    renderer_->drawPreparedF0Curve(g, item.correctedF0VisualSegments, UIColors::correctedF0, 1.0f, false, ctx);
            }
        }

        for (const auto& item : ctx.contents)
            renderer_->drawChunkBoundaries(g, ctx, item);
    }

    contentSurface_.setSurfaceImage(contentSurfaceImage_);
}

void PianoRollComponent::updateRulerSurfaceBounds() const
{
    const auto timelineViewportBounds = getTimelineViewportBounds();
    rulerSurfaceBounds_ = {
        timelineViewportBounds.getX() + pianoKeyWidth_,
        timelineViewportBounds.getY(),
        juce::jmax(0, timelineViewportBounds.getWidth() - pianoKeyWidth_),
        rulerHeight_
    };
    rulerSurface_.setBounds(rulerSurfaceBounds_);
    rulerSurface_.setImageOffsetX(renderBand_.valid ? renderBand_.startContentX - scrollOffset_ - pianoKeyWidth_ : 0);
}

void PianoRollComponent::updateContentSurfaceBounds() const
{
    const auto timelineViewportBounds = getTimelineViewportBounds();
    contentSurfaceBounds_ = timelineViewportBounds.withTrimmedTop(rulerHeight_).withTrimmedLeft(pianoKeyWidth_);
    contentSurface_.setBounds(contentSurfaceBounds_);
    contentSurface_.setImageOffsetX(renderBand_.valid ? renderBand_.startContentX - scrollOffset_ - pianoKeyWidth_ : 0);
}

void PianoRollComponent::rebuildPreparedRenderModelForViewport(int viewportStartX,
                                                               int viewportEndX,
                                                               bool forceRebuild) const
{
    juce::ignoreUnused(viewportStartX, viewportEndX);
    ensureRenderBandCoversCurrentViewport(forceRebuild);
}

void PianoRollComponent::refreshVerticalViewportGeometry(PianoRollVisualInvalidationPriority priority)
{
    updateScrollBars();
    refreshPreparedRenderModel(true);
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport), priority);
}

void PianoRollComponent::setScale(int rootNote, int scaleType)
{
    const int clampedRoot = juce::jlimit(0, 11, rootNote);
    const int clampedType = juce::jlimit(1, 8, scaleType);
    if (scaleRootNote_ == clampedRoot && scaleType_ == clampedType)
        return;
    scaleRootNote_ = clampedRoot;
    scaleType_ = clampedType;
    ++visualPrefsRevision_;
    prepareVisibleRenderModel();
    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Content));
}

void PianoRollComponent::fitToScreen() {
    // 濡傛灉鐢ㄦ埛宸叉墜鍔ㄨ皟鏁磋繃缂╂斁锛屼笉鑷姩瑕嗙洊
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
    // 缁熶竴璇箟锛氶鐜団啍MIDI 浠モ€滃崐闊充腑蹇冪嚎鈥濅负閿氱偣锛堜笉鏄敭杈圭晫锛夈€?
    return 12.0f * std::log2(frequency / 440.0f) + 69.0f - 0.5f;
}

float PianoRollComponent::midiToFreq(float midiNote) const {
    // 涓?freqToMidi 淇濇寔涓ユ牸浜掗€嗙殑涓績绾块敋鐐圭害瀹氥€?
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

    const auto projection = activeContentProjection();
    return projection.isValid() ? projection.timelineStartSeconds : 0.0;
}

double PianoRollComponent::timelineViewEndSeconds() const noexcept
{
    if (timelineViewDomain_.isValid()) {
        return timelineViewDomain_.endSeconds;
    }

    const auto projection = activeContentProjection();
    return projection.isValid() ? projection.timelineEndSeconds() : 0.0;
}

bool PianoRollComponent::hasExplicitTimelineViewDomain() const noexcept
{
    return timelineViewDomain_.isValid();
}

double PianoRollComponent::projectTimelineTimeToContent(double timelineSeconds) const {
    const auto projection = activeContentProjection();
    return projection.isValid()
        ? projection.projectTimelineTimeToContent(timelineSeconds)
        : timelineSeconds;
}

double PianoRollComponent::projectContentTimeToTimeline(double contentSeconds) const {
    const auto projection = activeContentProjection();
    return projection.isValid()
        ? projection.projectContentTimeToTimeline(contentSeconds)
        : contentSeconds;
}

double PianoRollComponent::getTimelinePixelsPerSecond() const {
    return timeConverter_.getPixelsPerSecond();
}

double PianoRollComponent::getPlayheadAbsolutePixelX(double playheadTimeSeconds) const {
    return juce::jmax(0.0, toVisibleTimelineSeconds(playheadTimeSeconds)) * getTimelinePixelsPerSecond();
}

int PianoRollComponent::timeToXForRenderScroll(double seconds,
                                               int renderScrollOffsetPx,
                                               int renderPianoKeyWidth) const {
    return static_cast<int>(std::llround(toVisibleTimelineSeconds(seconds) * getTimelinePixelsPerSecond()))
        - renderScrollOffsetPx
        + renderPianoKeyWidth;
}

double PianoRollComponent::xToTimeForRenderScroll(int x,
                                                  int renderScrollOffsetPx,
                                                  int renderPianoKeyWidth) const {
    return toAbsoluteTimelineSeconds(
        (static_cast<double>(x - renderPianoKeyWidth + renderScrollOffsetPx)) / getTimelinePixelsPerSecond());
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

    invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Decoration));

    auto failAfterStart = [this](AutoTuneApplyStatus status) {
        autoTuneInFlight_.store(false, std::memory_order_release);
        invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Decoration));
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
    request->audioSampleRate = static_cast<double>(PianoRollComponent::kAudioSampleRate);

    request->autoOriginalF0Full = originalF0;
    request->autoHopSize = currentCurve_ ? currentCurve_->getHopSize() : 512;
    request->autoF0SampleRate = currentCurve_ ? currentCurve_->getSampleRate() : 16000.0;
    request->autoStartFrame = startFrame;
    request->autoEndFrame = endFrame;
    request->autoGenParams = genParams;

    request->contentEpochSnapshot = editedContentEpoch_.load(std::memory_order_acquire);
    request->contentKeySnapshot = editedContentKey_;

    captureBeforeUndoSnapshot();
    pendingUndoDescription_ = TRANS("鑷姩璋冮煶");

    correctionWorker_->enqueue(request);

    AppLogger::log("AutoTune: enqueued contentKey.objectId=" + juce::String(static_cast<juce::int64>(editedContentKey_.objectId))
        + " startFrame=" + juce::String(startFrame) + " endFrame=" + juce::String(endFrame));

    return { AutoTuneApplyStatus::Applied };
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

std::vector<Note> PianoRollComponent::getEditedContentNotesCopy() const {
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
    for (const auto& placement : timelineContentPlacements_) {
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
