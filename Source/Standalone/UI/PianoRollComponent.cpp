#include "PianoRollComponent.h"
#include "../../PluginProcessor.h"
#include "../../Utils/LocalizationManager.h"
#include "../Utils/AppLogger.h"
#include "../../Utils/PianoRollEditAction.h"
#include "../../Utils/PianoRollNotePatchAction.h"
#include "../../Utils/TimeGridEditAction.h"   // 閳库槄?vocal-time-stretch ?.7
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include "../DSP/ChromaKeyDetector.h"
#include "../Utils/LegacyNoteGenerator.h"
#include "../Utils/SimdPerceptualPitchEstimator.h"
#include "../Utils/ZoomSensitivityConfig.h"
#include "UiAssets.h"
#include "UiText.h"
#include "ToolbarIcons.h"
#include "../../Utils/AudioEditingScheme.h"
#include "Utils/PianoKeyAudition.h"
#include "TimelineViewportPolicy.h"
#include "TimelineLayerComposer.h"
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

    scrollVBlankAttachment_ = std::make_unique<juce::VBlankAttachment>(
        this, [this](double timestampSec) { onScrollVBlankCallback(timestampSec); });

    scrollModeToggleButton_.toFront(false);
    timeUnitToggleButton_.toFront(false);
}



void PianoRollComponent::initializeRenderer() {
    renderer_ = std::make_unique<PianoRollRenderer>();
}

PianoRollToolHandler::Context PianoRollComponent::buildToolHandlerContext() {
    PianoRollToolHandler::Context toolCtx;
    toolCtx.getState = [this]() -> InteractionState& { return interactionState_; };

    toolCtx.getViewMapper = [this]() -> ViewMapper { return makeViewMapper(); };
    toolCtx.contentOriginY = rulerHeight_;

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

    toolCtx.invalidateLiveNotes = [this](const std::vector<Note>& before, const std::vector<Note>& after) {
        invalidateLiveNotes(before, after);
    };
    toolCtx.invalidateSelectionFeedback = [this]() {
        invalidateSelectionFeedback();
    };
    toolCtx.invalidateInteractionPreview = [this](const juce::Rectangle<int>& bounds) {
        invalidateInteractionPreview(bounds);
    };
    toolCtx.setMouseCursor = [this](const juce::MouseCursor& c) { setMouseCursor(c); };
    toolCtx.grabKeyboardFocus = [this]() { grabKeyboardFocus(); };
    toolCtx.getAudioEditingScheme = [this]() { return audioEditingScheme_; };
    toolCtx.notifyPlayheadChange = [this](double time) {
        const auto seekRevision = playHeadState_.hostPositionRevision.load(std::memory_order_acquire);
        bool requestDispatched = false;
        listeners_.call([&](Listener& l) {
            if (l.playheadPositionChangeRequested(time))
                requestDispatched = true;
        });
        userScrollHold_ = false;
        if (requestDispatched) {
            seekSentRevision_ = seekRevision;
            pendingSeekTime_ = time;
        } else {
            pendingSeekTime_ = -1.0;
            seekSentRevision_ = 0;
        }
    };
    toolCtx.notifyPitchCurveEdited = [this](int s, int e) {
        listeners_.call([s, e](Listener& l) { l.pitchCurveEdited(s, e); });
    };

    toolCtx.applyManualCorrection = [this](std::vector<PianoRollToolHandler::ManualCorrectionOp> ops, int s, int e, bool render) {
        return applyManualCorrectionPatch(ops, s, e, render);
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
    // 閳库槄锟?vocal-time-stretch 锟?.7 锟?Time tool / TimeGrid wiring
    // ============================================================
    toolCtx.getActiveContentTimeGrid = [this]() -> std::shared_ptr<const TimeGridSnapshot> {
        auto placement = findEditedPlacement();
        if (!placement) return nullptr;
        auto snap = readSnapshotFor(placement->contentKey);
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
        const bool published = contentCommands_->setTimeGrid(editedContentKey_, newSnap);
        if (!published) return false;
        processor_->getUndoManager().addAction(std::move(action));

        {
            auto snap = readEditedSnapshot();
            if (snap) lastKnownTimeGridRevision_ = snap->timeGridRevision;
        }
        requestContentRedraw();
        return true;
    };
    toolCtx.repaintTimeGridHandles = [this]() {
        overlay_->repaint();
    };

    return toolCtx;
}

void PianoRollComponent::initializeToolHandler() {
    toolHandler_ = std::make_unique<PianoRollToolHandler>(buildToolHandlerContext());
}

PianoRollComponent::PianoRollComponent(const PlayHeadState& playHeadState)
    : playHeadState_(playHeadState) {
    initializeUIComponents();
    initializeRenderer();
    initializeToolHandler();
    overlay_ = std::make_unique<PianoRollOverlayComponent>(*this);
    addAndMakeVisible(*overlay_);
    overlay_->setInterceptsMouseClicks(false, false);
}

PianoRollComponent::~PianoRollComponent() {
    scrollVBlankAttachment_.reset();
    horizontalScrollBar_.removeListener(this);
    verticalScrollBar_.removeListener(this);
}

bool PianoRollComponent::applyCorrectionToEntireClip(float retuneSpeed, float vibratoDepth, float vibratoRate)
{
    if (!currentCurve_) {
        return false;
    }

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) {
        return false;
    }

    auto notes = getCommittedNotes();
    auto editedCurve = currentCurve_->clone();
    editedCurve->applyCorrectionToRange(notes, 0, f0tl.endFrameExclusive(),
                                        retuneSpeed, vibratoDepth, vibratoRate);

    const auto snap = editedCurve->getSnapshot();
    const auto allSegments = snap->getCorrectionSegments();
    const F0FrameRange affectedRange{0, f0tl.endFrameExclusive()};

    captureBeforeUndoSnapshot();
    pendingUndoDescription_ = TRANS("自动调音");

    if (!commitEditedContentNotesAndSegments(notes, allSegments, affectedRange)) {
        return false;
    }
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
        clearNoteDraft();
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
    // When startTime matches 锟?same note, compare content.
    // When startTime differs 锟?deletion or insertion.
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
            // Same position 锟?compare content for modification
            if (!notesContentEqual(baseline[i], working[j])) {
                dirtyStartTime = std::min(dirtyStartTime, baseline[i].startTime);
                dirtyEndTime = std::max({dirtyEndTime, baseline[i].endTime, working[j].endTime});
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

    // No actual changes 锟?skip commit, not a failure
    if (dirtyEndTime <= dirtyStartTime) {
        clearNoteDraft();
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

    const auto committedSnap = contentCommands_->commitNotePatch(editedContentKey_, patch);
    if (!committedSnap) {
        return false;
    }

    cachedNotes_ = committedSnap->notes;
    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
    syncF0SelectionToSelectedNotes();

    // Build the before-patch from baseline notes in the same seconds range.
    // Note-only undo uses seconds-based PianoRollNotePatchAction 锟?no frame
    // conversion, no segment involvement, same coordinate system as commitNotePatch().
    ContentNoteRangePatch beforePatch;
    beforePatch.affectedRange = patch.affectedRange;
    for (const auto& n : baseline) {
        if (overlapsRange(n)) beforePatch.afterNotesInRange.push_back(n);
    }

    auto action = std::make_unique<PianoRollNotePatchAction>(
        contentCommands_,
        editedContentKey_,
        pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : TRANS("缂栬緫"),
        std::move(beforePatch),
        std::move(patch));

    if (processor_ != nullptr)
        processor_->getUndoManager().addAction(std::move(action));

    pendingUndoDescription_ = {};
    undoSnapshotCaptured_ = false;
    clearNoteDraft();
    lastKnownNotesRevision_ = committedSnap->notesRevision;
    requestContentRedraw();
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
    // commitEditedContentNotesAndSegments 锟?commitContentNotesAndSegments
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

    // Capture range-scoped before data directly 锟?no full snapshot.
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
        pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : TRANS("缂栬緫"),
        std::move(beforeNotes),
        std::move(afterNotes),
        std::move(beforeSegments),
        std::move(afterSegments),
        ContentEditRangeFrames{affectedRange.startFrame, affectedRange.endFrameExclusive});

    processor_->getUndoManager().addAction(std::move(action));
    pendingUndoDescription_ = {};
    undoSnapshotCaptured_ = false;
    lastKnownNotesRevision_ = committedSnap->notesRevision;
    lastKnownPitchRevision_ = committedSnap->pitchRevision;
    {
        requestContentRedraw();
    }
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
        description.isNotEmpty() ? description : TRANS("缂栬緫"),
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
        overlay_->repaint();
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
    overlay_->repaint();

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
    const float midi = makeViewMapper().freqToMidi(adjustedPitch);
    const float y = makeViewMapper().midiToY(midi) - (pixelsPerSemitone_ * 0.5f);
    const int top = static_cast<int>(std::floor(y));
    const int height = std::max(1, static_cast<int>(std::ceil(pixelsPerSemitone_)));
    return juce::Rectangle<int>(x1, top, width, height)
        .expanded(4)
        .translated(0, rulerHeight_)
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
    const int y1 = static_cast<int>(std::floor(makeViewMapper().midiToY(maxMidi)));
    const int y2 = static_cast<int>(std::ceil(makeViewMapper().midiToY(minMidi)));
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
        includePoint(static_cast<float>(sourceTimeToX(anchor.time)),
                     makeViewMapper().freqToY(anchor.freq) + static_cast<float>(rulerHeight_));
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
        const float y = makeViewMapper().freqToY(f0);
        const auto pointBounds = juce::Rectangle<float>(x - 2.0f, y - 2.0f, 4.0f, 4.0f);
        bounds = hasBounds ? bounds.getUnion(pointBounds) : pointBounds;
        hasBounds = true;
    }

    return hasBounds ? bounds.getSmallestIntegerContainer().expanded(4).getIntersection(getTimelineViewportBounds())
                     : juce::Rectangle<int>();
}

void PianoRollComponent::invalidateLiveNotes(const std::vector<Note>& beforeNotes, const std::vector<Note>& afterNotes)
{
    auto beforeBounds = getNotesBounds(beforeNotes);
    auto afterBounds = getNotesBounds(afterNotes);
    auto dirty = beforeBounds.getUnion(afterBounds);
    if (!dirty.isEmpty()) {
        if (zoomPreviewActive_) {
            // 缩放事务期间冻结 Image，仅标记脏，由 endZoomPreview 最终重建
            contentDirty_ = true;
        } else {
            rasterizeContent(dirty);
            repaint(dirty.getX(), dirty.getY(), dirty.getWidth(), dirty.getHeight());
        }
        overlay_->repaint(dirty.getX(), dirty.getY(), dirty.getWidth(), dirty.getHeight());
    }
}

void PianoRollComponent::invalidateSelectionFeedback()
{
    overlay_->repaint();
}

void PianoRollComponent::invalidateInteractionPreview(const juce::Rectangle<int>& bounds)
{
    overlay_->repaint(bounds);
}

bool PianoRollComponent::applyManualCorrectionPatch(const std::vector<PianoRollToolHandler::ManualCorrectionOp>& ops,
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

    // dirtyStartFrame/dirtyEndFrame 閺勵垱澧嶉張?manual ops 锟?dirty 鐢冭嫙闂嗗棴绱欓崥顐ゎ伂閻愮櫢绱氶妴?
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

    return true;
}

// ============================================================================
// drawPlayheadOverlay 鈥?鐩存帴缁樺埗鎾斁澶达紙鍙栦唬 FixedPlayheadComponent 瀛愮粍浠讹級
// ============================================================================

void PianoRollComponent::drawPlayheadOverlay(juce::Graphics& g)
{
    const auto viewportBounds = getTimelineViewportBounds();
    const auto mapper = makeViewMapper();

    const int contentViewportLeft = mapper.contentStartX;
    const int timeDerivedX = mapper.timeToX(playheadTimeForPaint_);
    const int contentViewportRight = viewportBounds.getRight();
    const int viewportCentreX = (contentViewportLeft + contentViewportRight) / 2;

    const bool playing = playHeadState_.isPlaying.load(std::memory_order_relaxed);
    const bool continuousMode = scrollMode_ == ScrollMode::Continuous && !userScrollHold_;

    const auto pres = TimelineViewportPolicy::computePlayheadPresentation(
        timeDerivedX, viewportCentreX, contentViewportRight, contentViewportLeft,
        playing, continuousMode);

    if (!pres.visible)
        return;

    const float anchorX = static_cast<float>(pres.anchorX);
    const float height = static_cast<float>(getHeight());
    juce::Graphics::ScopedSaveState playheadClip(g);
    g.reduceClipRegion(timeAxisRect());

    g.setColour(playheadColour_);
    g.drawLine(anchorX, 0.0f, anchorX, height, 2.0f);

    static const juce::Path kPlayheadTriangle = [] {
        juce::Path p;
        p.addTriangle(-6.0f, 0.0f, 6.0f, 0.0f, 0.0f, 6.0f);
        return p;
    }();
    g.fillPath(kPlayheadTriangle, juce::AffineTransform::translation(anchorX, 0.0f));
}

// ============================================================================
// drawTransientOverlay 鈥?paint transient interaction previews
// ============================================================================

void PianoRollComponent::drawTransientOverlay(juce::Graphics& g)
{
    juce::Graphics::ScopedSaveState overlaySave(g);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    if (currentTool_ != ToolId::TimeTool) {
        if (currentCurve_ != nullptr) {
            drawNoteDragCurvePreview(g);
            drawHandDrawPreview(g);
            drawLineAnchorPreview(g);
        }
    }

    // 鈿★笍 Cursor preview (璞佸厤璺緞): DrawNote tool 鐨勭粯鍒朵腑 note preview
    // 杩欐槸浜や簰 cursor preview锛屼笉锟?committed/draft note body
    // The committed/draft note body is drawn by the direct content pass.
    if (interactionState_.drawing.isDrawingNote
        && currentTool_ == ToolId::DrawNote) {
        juce::Graphics::ScopedSaveState previewSave(g);
        double startTime = std::min(interactionState_.drawing.drawingNoteStartTime,
                                    interactionState_.drawing.drawingNoteEndTime);
        double endTime = std::max(interactionState_.drawing.drawingNoteStartTime,
                                  interactionState_.drawing.drawingNoteEndTime);
        float pitch = interactionState_.drawing.drawingNotePitch;

        if (pitch > 0.0f && endTime > startTime) {
            int x1 = sourceTimeToX(startTime);
            int x2 = sourceTimeToX(endTime);
            const auto mapper = makeViewMapper();
            const float y = mapper.freqToY(pitch) - (pixelsPerSemitone_ * 0.5f);
            float noteHeight = pixelsPerSemitone_;

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

    drawSelectionBox(g, UIColors::currentThemeId());
}

void PianoRollComponent::drawTimeGridHandles(juce::Graphics& g)
{
    if (!isTimeView()) return;
    if (interactionState_.timeTool.selectedHandleId == 0
        && interactionState_.timeTool.hoveredHandleId == 0
        && interactionState_.timeTool.additionalSelectedIds.empty()) return;

    juce::Graphics::ScopedSaveState ss(g);
    const auto contentBounds = juce::Rectangle<int>(
        pianoKeyWidth_, rulerHeight_,
        getTimelineContentViewportWidth(), getTimelineContentViewportHeight());
    g.reduceClipRegion(contentBounds);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    PianoRollRenderer::RenderContext ctx;
    ctx.width = getTimelineViewportBounds().getWidth();
    ctx.height = getTimelineContentViewportHeight();
    ctx.pianoKeyWidth = pianoKeyWidth_;
    ctx.rulerHeight = 0;
    ctx.pixelsPerSecond = camera_.pixelsPerSecond;
    ctx.pixelsPerSemitone = pixelsPerSemitone_;
    ctx.minMidi = minMidi_;
    ctx.maxMidi = maxMidi_;
    ctx.bpm = bpm_;
    ctx.coords = makeViewMapper();
    ctx.timeGridHoveredHandleId = interactionState_.timeTool.hoveredHandleId;
    ctx.timeGridSelectedHandleId = interactionState_.timeTool.selectedHandleId;
    ctx.additionalSelectedHandleIds = interactionState_.timeTool.additionalSelectedIds;
    ctx.currentTool = currentTool_;
    ctx.contents = buildContentRenderItems();
    // 拖拽中的 working TimeGrid snapshot 仅应用于 Overlay 把手绘制
    if (interactionState_.timeTool.isDraggingHandle
        && interactionState_.timeTool.dragWorkingSnapshot != nullptr) {
        for (auto& item : ctx.contents) {
            if (item.active)
                item.timeGrid = interactionState_.timeTool.dragWorkingSnapshot;
        }
    }

    for (const auto& item : ctx.contents) {
        if (item.active)
            renderer_->drawTimeGridHandles(g, ctx, item);
    }
}

void PianoRollComponent::drawSelectedNoteHighlights(juce::Graphics& g)
{
    if (interactionState_.noteSelection.selectedIndices.empty()) return;

    juce::Graphics::ScopedSaveState ss(g);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    PianoRollRenderer::RenderContext ctx;
    ctx.width = getTimelineViewportBounds().getWidth();
    ctx.height = getTimelineContentViewportHeight();
    ctx.pianoKeyWidth = pianoKeyWidth_;
    ctx.rulerHeight = 0;
    ctx.pixelsPerSecond = camera_.pixelsPerSecond;
    ctx.pixelsPerSemitone = pixelsPerSemitone_;
    ctx.minMidi = minMidi_;
    ctx.maxMidi = maxMidi_;
    ctx.coords = makeViewMapper();
    ctx.contents = buildContentRenderItems();

    for (const auto& item : ctx.contents) {
        if (!item.active) continue;
        renderer_->drawSelectedNoteHighlights(
            g, ctx, *item.displayNotes,
            interactionState_.noteSelection.selectedIndices, item);
    }
}

void PianoRollComponent::drawPianoKeysPressed(juce::Graphics& g)
{
    if (pressedPianoKey_ < 0 || !shouldShowPianoKeys()) return;

    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(0, rulerHeight_, pianoKeyWidth_, getTimelineContentViewportHeight());
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    const float vOrigin = std::floor(verticalScrollOffset_);
    const float vFrac = verticalScrollOffset_ - vOrigin;
    g.addTransform(juce::AffineTransform::translation(0.0f, -vFrac));

    const float noteY = (maxMidi_ - pressedPianoKey_) * pixelsPerSemitone_ - vOrigin;
    const float noteH = pixelsPerSemitone_;
    g.setColour(UIColors::noteBlockSelected.withAlpha(0.35f));
    g.fillRect(0.0f, noteY, static_cast<float>(pianoKeyWidth_), noteH);
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
            float y = makeViewMapper().freqToY(f0);
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
        const float y = makeViewMapper().freqToY(f0);
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
        float y = makeViewMapper().freqToY(anchor.freq);

        g.setColour(anchorColour);
        g.fillEllipse(x - 2.0f, y - 2.0f, 4.0f, 4.0f);

        if (i > 0) {
            const auto& prev = interactionState_.drawing.pendingAnchors[i - 1];
            float prevX = static_cast<float>(sourceTimeToX(prev.time));
            float prevY = makeViewMapper().freqToY(prev.freq);
            g.setColour(anchorColour.withAlpha(0.7f));
            g.drawLine(prevX, prevY, x, y, 2.0f);
        }
    }

    if (!interactionState_.drawing.pendingAnchors.empty()) {
        const auto& last = interactionState_.drawing.pendingAnchors.back();
        float lastX = static_cast<float>(sourceTimeToX(last.time));
        float lastY = makeViewMapper().freqToY(last.freq);
        g.setColour(anchorColour.withAlpha(0.4f));
        g.drawLine(lastX, lastY,
                   interactionState_.drawing.currentMousePos.x,
                   interactionState_.drawing.currentMousePos.y - static_cast<float>(rulerHeight_),
                   1.5f);
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
    float y1 = makeViewMapper().midiToY(maxMidi);
    float y2 = makeViewMapper().midiToY(minMidi);

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
void PianoRollComponent::paint(juce::Graphics& g)
{
    const double paintStartMs = juce::Time::getMillisecondCounterHiRes();
    const int vpW = getTimelineViewportBounds().getWidth();
    const int vpH = getTimelineViewportBounds().getHeight();
    if (vpW <= 0 || vpH <= 0) return;

    if (zoomPreviewActive_) {
        RasterView liveView{camera_, pixelsPerSemitone_, verticalScrollOffset_};
        drawStaticLayer(g, liveView, getLocalBounds());
        drawContentLayer(g, liveView, getLocalBounds());
    } else {
        // 正常模式：仅呈现既有两张 Image
        if (staticSurface_.isValid()) {
            g.drawImageAt(staticSurface_, 0, 0, false);
        }
        if (contentSurface_.isValid()) {
            g.drawImageAt(contentSurface_, 0, 0, false);
        }
    }

    const double paintEndMs = juce::Time::getMillisecondCounterHiRes();
    recordRenderProbe(RenderProbePoint::RootPaint, paintEndMs - paintStartMs);
    if (lastVBlankMs_ > 0.0)
        recordRenderProbe(RenderProbePoint::VBlankToRootPaint, paintStartMs - lastVBlankMs_);
}

void PianoRollComponent::rasterizeDirtySurfaces()
{
    if (zoomPreviewActive_) return;  // 缩放预览期间保留 dirty，不栅格

    const int vpW = getTimelineViewportBounds().getWidth();
    const int vpH = getTimelineViewportBounds().getHeight();
    if (vpW <= 0 || vpH <= 0) return;

    const int fullW = getWidth();
    const int fullH = getHeight();

    // staticSurface_ 覆盖完整组件 chrome 区域
    if (!staticSurface_.isValid() || staticSurface_.getWidth() != fullW || staticSurface_.getHeight() != fullH) {
        staticSurface_ = juce::Image(juce::Image::ARGB, fullW, fullH, true);
        staticDirty_ = true;
    }
    // contentSurface_ 保持时间轴视口区
    if (!contentSurface_.isValid() || contentSurface_.getWidth() != vpW || contentSurface_.getHeight() != vpH) {
        contentSurface_ = juce::Image(juce::Image::ARGB, vpW, vpH, true);
        contentDirty_ = true;
    }

    // 全量双表面重建时一次性同步 rasterView_ 为 live 状态
    if (staticDirty_ && contentDirty_) {
        rasterView_.camera = camera_;
        rasterView_.pixelsPerSemitone = pixelsPerSemitone_;
        rasterView_.verticalScrollOffset = verticalScrollOffset_;
    }

    // 仅栅格脏表面
    if (staticDirty_) rasterizeStatic();
    if (contentDirty_) rasterizeContent();
}

void PianoRollComponent::recordRenderProbe(RenderProbePoint point, double elapsedMs)
{
    RasterProbe* probe = nullptr;
    switch (point) {
        case RenderProbePoint::StaticRaster:  probe = &staticRasterProbe_;  break;
        case RenderProbePoint::ContentRaster: probe = &contentRasterProbe_; break;
        case RenderProbePoint::OverlayPresent: probe = &overlayPresentProbe_; break;
        case RenderProbePoint::RootPaint: probe = &rootPaintProbe_; break;
        case RenderProbePoint::VBlankToRootPaint: probe = &vblankToRootPaintProbe_; break;
    }
    probe->count++;
    probe->totalMs += elapsedMs;

    const double now = juce::Time::getMillisecondCounterHiRes();
    if (probeReportWindowStart_ == 0.0) probeReportWindowStart_ = now;
    if (now - probeReportWindowStart_ >= 2000.0) {
        auto avg = [](const RasterProbe& p) { return p.count > 0 ? p.totalMs / p.count : 0.0; };
        AppLogger::log(juce::String("[PR-Perf] static-raster:") + juce::String(avg(staticRasterProbe_), 1) + "ms x" + juce::String(staticRasterProbe_.count)
            + " content-raster:" + juce::String(avg(contentRasterProbe_), 1) + "ms x" + juce::String(contentRasterProbe_.count)
            + " overlay-present:" + juce::String(avg(overlayPresentProbe_), 1) + "ms x" + juce::String(overlayPresentProbe_.count)
            + " root-paint:" + juce::String(avg(rootPaintProbe_), 1) + "ms x" + juce::String(rootPaintProbe_.count)
            + " vblank-to-root-paint:" + juce::String(avg(vblankToRootPaintProbe_), 1) + "ms x" + juce::String(vblankToRootPaintProbe_.count));
        staticRasterProbe_ = {};
        contentRasterProbe_ = {};
        overlayPresentProbe_ = {};
        rootPaintProbe_ = {};
        vblankToRootPaintProbe_ = {};
        probeReportWindowStart_ = now;
    }
}

void PianoRollComponent::drawStaticLayer(juce::Graphics& g, const RasterView& rv, juce::Rectangle<int> bounds)
{
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(bounds);

    const int imgW = getWidth();
    const int imgH = getHeight();

    // 主题背景 + chrome
    g.setColour(UIColors::rollBackground);
    g.fillRect(bounds);
    {
        juce::Path chromePath;
        chromePath.addRoundedRectangle(juce::Rectangle<float>(0, 0, static_cast<float>(imgW), static_cast<float>(imgH)), UIColors::cornerRadius);
        juce::Graphics::ScopedSaveState css(g);
        g.reduceClipRegion(chromePath);
        switch (UIColors::currentThemeId()) {
            case ThemeId::DarkBlueGrey: UIColors::fillSoothe2SpectrumBackground(g, juce::Rectangle<float>(0, 0, static_cast<float>(imgW), static_cast<float>(imgH)), UIColors::cornerRadius); break;
            case ThemeId::Aurora: g.setColour(UIColors::rollBackground); g.fillPath(chromePath); break;
            case ThemeId::BlueBreeze: UIColors::fillMistedTimelineField(g, juce::Rectangle<float>(0,0,static_cast<float>(imgW),static_cast<float>(imgH)), UIColors::cornerRadius); break;
            case ThemeId::Overdose: UiAssets::drawAssetStretch(g, UiAssetId::PanelEditorMain, juce::Rectangle<float>(0,0,static_cast<float>(imgW),static_cast<float>(imgH))); break;
            default: g.setColour(UIColors::rollBackground); g.fillPath(chromePath); break;
        }
    }
    UIColors::drawShadow(g, juce::Rectangle<float>(0, 0, static_cast<float>(imgW), static_cast<float>(imgH)));
    g.setColour(juce::Colours::white);

    // 标尺
    {
        juce::Graphics::ScopedSaveState rs(g);
        const double pps = rv.camera.pixelsPerSecond;
        const double visibleStart = rv.camera.visibleStartSeconds;
        const double visibleEnd = visibleStart + getTimelineContentViewportWidth() / pps;
        const int cw = getTimelineContentViewportWidth();
        RenderParams rp;
        rp.visibleStartSeconds = visibleStart; rp.visibleEndSeconds = visibleEnd;
        rp.pixelsPerSecond = pps;
        rp.timeUnit = (timeUnit_ == TimeUnit::Bars) ? 1 : 0;
        rp.tempo = bpm_;
        rp.themeId = static_cast<int>(UIColors::currentThemeId());
        rp.pixelsPerSemitone = 0.0f; rp.worldTopY = 0;
        rp.rulerHeight = rulerHeight_; rp.laneStyle = 0;
        rp.viewportWidth = cw; rp.viewportHeight = rulerHeight_;
        rp.viewKind = "pianoroll";
        g.addTransform(juce::AffineTransform::translation(static_cast<float>(pianoKeyWidth_), 0.0f));
        g.reduceClipRegion(0, 0, cw, rulerHeight_);
        TimelineLayerComposer::drawTimeRuler(g, rp);
    }

    // Lane strips + grid + piano keys
    {
        juce::Graphics::ScopedSaveState ls(g);
        const double pps = rv.camera.pixelsPerSecond;
        const double visibleStart = rv.camera.visibleStartSeconds;
        const double visibleEnd = visibleStart + getTimelineContentViewportWidth() / pps;
        const int cw = getTimelineContentViewportWidth();
        const int ch = getTimelineContentViewportHeight();
        const float vOrigin = std::floor(rv.verticalScrollOffset);
        const float vFrac = rv.verticalScrollOffset - vOrigin;

        RenderParams lp;
        lp.visibleStartSeconds = visibleStart; lp.visibleEndSeconds = visibleEnd;
        lp.pixelsPerSecond = pps; lp.timeUnit = (timeUnit_ == TimeUnit::Bars) ? 1 : 0;
        lp.tempo = bpm_;
        lp.themeId = static_cast<int>(UIColors::currentThemeId());
        lp.pixelsPerSemitone = rv.pixelsPerSemitone; lp.worldTopY = vOrigin;
        lp.rulerHeight = 0; lp.laneStyle = encodeLaneStyle(showLanes_, scaleRootNote_, scaleType_);
        lp.viewportWidth = cw; lp.viewportHeight = ch; lp.viewKind = "pianoroll";

        if (showLanes_) {
            juce::Graphics::ScopedSaveState lss(g);
            g.addTransform(juce::AffineTransform::translation(static_cast<float>(pianoKeyWidth_), static_cast<float>(rulerHeight_) - vFrac));
            TimelineLayerComposer::drawLaneStripRepeats(g, lp);
        }
        {
            juce::Graphics::ScopedSaveState gs(g);
            g.addTransform(juce::AffineTransform::translation(static_cast<float>(pianoKeyWidth_), static_cast<float>(rulerHeight_)));
            TimelineLayerComposer::drawGridLines(g, lp);
        }

        // 琴键
        if (shouldShowPianoKeys()) {
            const int vpW = getTimelineViewportBounds().getWidth();
            auto ctxForKeys = [&]() {
                PianoRollRenderer::RenderContext rctx;
                rctx.width = vpW;
                rctx.height = ch;
                rctx.pianoKeyWidth = pianoKeyWidth_;
                rctx.rulerHeight = 0;
                rctx.pixelsPerSecond = pps;
                rctx.pixelsPerSemitone = rv.pixelsPerSemitone;
                rctx.minMidi = minMidi_;
                rctx.maxMidi = maxMidi_;
                rctx.scaleRootNote = scaleRootNote_;
                rctx.scaleType = scaleType_;
                rctx.noteNameMode = noteNameMode_;
                rctx.coords = makeViewMapperForRasterView(rv);
                rctx.rasterBounds = bounds;
                return rctx;
            };
            juce::Graphics::ScopedSaveState pks(g);
            g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));
            g.reduceClipRegion(0, 0, pianoKeyWidth_, ch);
            renderer_->drawPianoKeys(g, ctxForKeys());
        }
    }
}

void PianoRollComponent::rasterizeStatic(std::optional<juce::Rectangle<int>> dirtyRect)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    const int imgW = staticSurface_.getWidth();
    const int imgH = staticSurface_.getHeight();
    if (!staticSurface_.isValid() || imgW <= 0 || imgH <= 0) return;

    const juce::Rectangle<int> rasterBounds = dirtyRect.value_or(staticSurface_.getBounds());

    juce::Graphics g(staticSurface_);
    staticSurface_.clear(rasterBounds);

    drawStaticLayer(g, rasterView_, rasterBounds);

    staticDirty_ = false;

    recordRenderProbe(RenderProbePoint::StaticRaster, juce::Time::getMillisecondCounterHiRes() - t0);
}

void PianoRollComponent::drawContentLayer(juce::Graphics& g, const RasterView& rv, juce::Rectangle<int> rasterBounds)
{
    const int w = getTimelineViewportBounds().getWidth();
    const int cw = getTimelineContentViewportWidth();
    const int ch = getTimelineContentViewportHeight();
    const auto timelineZone = juce::Rectangle<int>(pianoKeyWidth_, rulerHeight_, cw, ch);
    const auto clipArea = rasterBounds.getIntersection(timelineZone);
    if (clipArea.isEmpty()) return;

    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(clipArea);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    PianoRollRenderer::RenderContext renderCtx;
    renderCtx.width = w;
    renderCtx.height = ch;
    renderCtx.pianoKeyWidth = pianoKeyWidth_;
    renderCtx.rulerHeight = 0;
    renderCtx.pixelsPerSecond = rv.camera.pixelsPerSecond;
    renderCtx.pixelsPerSemitone = rv.pixelsPerSemitone;
    renderCtx.minMidi = minMidi_;
    renderCtx.maxMidi = maxMidi_;
    renderCtx.bpm = bpm_;
    renderCtx.scaleRootNote = scaleRootNote_;
    renderCtx.scaleType = scaleType_;
    renderCtx.noteNameMode = noteNameMode_;
    renderCtx.showLanes = showLanes_;
    renderCtx.showUnvoicedFrames = showUnvoicedFrames_;
    renderCtx.showOriginalF0 = showOriginalF0_;
    renderCtx.showCorrectedF0 = showCorrectedF0_;
    renderCtx.timeUnit = (timeUnit_ == TimeUnit::Bars) ? PianoRollTimeUnit::Bars : PianoRollTimeUnit::Seconds;
    renderCtx.coords = makeViewMapperForRasterView(rv);
    renderCtx.referenceOverlay = referenceOverlay_;
    renderCtx.rasterBounds = rasterBounds;

    // Copy content items to render context
    renderCtx.contents = buildContentRenderItems();

    const double pps = rv.camera.pixelsPerSecond;


    for (const auto& item : renderCtx.contents) {
        if (showWaveform_ && waveformMipmapCache_.isComplete() && item.audioBuffer != nullptr) {
            const auto* mipmap = waveformMipmapCache_.get(item.contentKey);
            if (mipmap != nullptr && mipmap->hasSource()) {
                const int bestLevel = mipmap->selectBestLevelIndex(pps);
                const auto& level = mipmap->getLevel(bestLevel);
                if (!level.peaks.empty())
                    renderer_->drawWaveform(g, renderCtx, item, level, bestLevel);
            }
        }
    }

    for (const auto& item : renderCtx.contents)
        renderer_->drawUnvoicedFrameBands(g, renderCtx, item);

    for (const auto& item : renderCtx.contents)
        renderer_->drawNotes(g, renderCtx, item);

    for (const auto& item : renderCtx.contents)
        renderer_->drawF0Curve(g, renderCtx, item);

    for (const auto& item : renderCtx.contents)
        renderer_->drawTimeGridAnchors(g, renderCtx, item);

    // Ghost content
    if (referenceOverlay_.has_value() && referenceOverlay_->enabled) {
        renderer_->drawGhostNotes(g, renderCtx, *referenceOverlay_);
        renderer_->drawGhostAnchors(g, renderCtx, *referenceOverlay_);
    }
}

void PianoRollComponent::rasterizeContent(std::optional<juce::Rectangle<int>> dirtyRect)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    const int w = getTimelineViewportBounds().getWidth();
    const int h = getTimelineViewportBounds().getHeight();
    if (!contentSurface_.isValid() || w <= 0 || h <= 0) return;

    juce::Graphics g(contentSurface_);

    const bool fullRaster = !dirtyRect.has_value();
    const auto rasterBounds = dirtyRect.value_or(juce::Rectangle<int>(0, 0, w, h));

    if (!dirtyRect.has_value()) {
        // JUCE fillAll(transparentBlack) 跳过透明填充 → 用 Image::clear 强制清空
        contentSurface_.clear(contentSurface_.getBounds());
    } else {
        // 局部条带清空
        contentSurface_.clear(*dirtyRect);
    }

    // 只在时间轴区域绘制内容
    drawContentLayer(g, rasterView_, rasterBounds);

    if (fullRaster)
        contentDirty_ = false;

    recordRenderProbe(RenderProbePoint::ContentRaster, juce::Time::getMillisecondCounterHiRes() - t0);
}

void PianoRollComponent::applyRasterCamera(const TimelineViewportCamera& newCamera)
{
    // 表面无效或脏 → 全量重建
    if (!staticSurface_.isValid() || staticDirty_) {
        staticDirty_ = true;
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        repaint();
        return;
    }

    // PPS 变化 → 全量重建
    if (newCamera.pixelsPerSecond != rasterView_.camera.pixelsPerSecond) {
        staticDirty_ = true;
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        repaint();
        return;
    }

    // dPixels 由连续 rasterView_.camera 投影，不含任何量化时间
    const int dPixels = static_cast<int>(std::llround(
        (newCamera.visibleStartSeconds - rasterView_.camera.visibleStartSeconds) * rasterView_.camera.pixelsPerSecond));

    // 大幅跳转（超过视口宽度）→ 全量重建
    if (std::abs(dPixels) >= getTimelineContentViewportWidth()) {
        staticDirty_ = true;
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        repaint();
        return;
    }

    // 无整数像素差 → 不移动、不栅格、不写 rasterView_.camera
    if (dPixels == 0) {
        return;
    }

    // ── 同 PPS 横向滚动：moveImageSection + 补绘条带 ──
    const int viewportW = getTimelineViewportBounds().getWidth();
    const int viewportH = getTimelineViewportBounds().getHeight();
    const int timelineLeft = pianoKeyWidth_;
    const int timelineW = viewportW - timelineLeft;

    const int srcX = timelineLeft + (dPixels > 0 ? dPixels : 0);
    const int dstX = timelineLeft + (dPixels > 0 ? 0 : -dPixels);
    const int moveW = timelineW - std::abs(dPixels);

    if (moveW > 0) {
        staticSurface_.moveImageSection(dstX, 0, srcX, 0, moveW, viewportH);
        contentSurface_.moveImageSection(dstX, 0, srcX, 0, moveW, viewportH);
    }

    // 直接写 rasterView_.camera 为新的连续语义相机
    rasterView_.camera = newCamera;

    // 补绘露出条带
    int stripX, stripW;
    if (dPixels > 0) {
        stripX = timelineLeft + timelineW - dPixels;
        stripW = dPixels;
    } else {
        stripX = timelineLeft;
        stripW = -dPixels;
    }
    stripX = juce::jmax(timelineLeft, stripX);
    stripW = juce::jmin(stripW, timelineW);

    if (stripW > 0) {
        juce::Rectangle<int> strip(stripX, 0, stripW, viewportH);
        rasterizeStatic(strip);
        rasterizeContent(strip);
    }

    repaint(timeAxisRect());
}

std::vector<PianoRollRenderer::ContentRenderItem> PianoRollComponent::buildContentRenderItems() const
{
    std::vector<PianoRollRenderer::ContentRenderItem> items;
    items.reserve(timelineContentPlacements_.size());
    for (const auto& placement : timelineContentPlacements_) {
        if (!placement.isValid()) continue;
        if (auto item = buildContentRenderItem(placement)) {
            // 内容表面始终使用已提交的 TimeGrid；拖拽中的 working snapshot 仅由 Overlay 应用
            items.push_back(std::move(*item));
        }
    }
    return items;
}

ViewMapper PianoRollComponent::makeViewMapperForRasterView(const RasterView& rv) const noexcept
{
    return ViewMapper{
        rv.camera.visibleStartSeconds,
        rv.camera.pixelsPerSecond,
        pianoKeyWidth_,
        getTimelineContentViewportWidth(),
        getTimelineContentViewportHeight(),
        rv.pixelsPerSemitone,
        rv.verticalScrollOffset,
        maxMidi_
    };
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
    return true;
        }
    }

    // Fallback: notes changed but no valid F0 timeline mapping or no current curve.
    // Pure note edit 锟?seconds-based PianoRollNotePatchAction for undo.
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

        const auto committedSnap = contentCommands_->commitNotePatch(editedContentKey_, afterPatch);
        if (!committedSnap) {
            return false;
        }

        cachedNotes_ = committedSnap->notes;
        interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
        syncF0SelectionToSelectedNotes();

        // Build before-patch from original (unmodified) notes for undo.
        ContentNoteRangePatch beforePatch;
        beforePatch.affectedRange = afterPatch.affectedRange;
        for (const auto& n : originalNotes) {
            if (overlapsRange(n)) beforePatch.afterNotesInRange.push_back(n);
        }

        auto action = std::make_unique<PianoRollNotePatchAction>(
            contentCommands_,
            editedContentKey_,
            pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : TRANS("缂栬緫"),
            std::move(beforePatch),
            std::move(afterPatch));

        if (processor_ != nullptr)
            processor_->getUndoManager().addAction(std::move(action));

        pendingUndoDescription_ = {};
        undoSnapshotCaptured_ = false;
        lastKnownNotesRevision_ = committedSnap->notesRevision;
        requestContentRedraw();
        return true;
    }

    return false;
}

bool PianoRollComponent::applyParameterToFrameRange(float retuneSpeed, float vibratoDepth, float vibratoRate, int startFrame, int endFrameExclusive) {
    if (!currentCurve_ || endFrameExclusive <= startFrame) return false;
    if (!currentCurve_->hasCorrectionInRange(startFrame, endFrameExclusive)) return false;

    auto notes = getEditedContentNotesCopy();
    auto editedCurve = currentCurve_->clone();
    editedCurve->applyCorrectionToRange(notes, startFrame, endFrameExclusive,
                                        retuneSpeed, vibratoDepth, vibratoRate);

    const auto snap = editedCurve->getSnapshot();
    auto allSegments = snap->getCorrectionSegments();
    const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(startFrame,
                                                                          endFrameExclusive,
                                                                          currentF0Timeline().endFrameExclusive());

    if (!commitEditedContentNotesAndSegments(notes, allSegments, affectedRange)) {
        return false;
    }

    const int notifyEndFrame = std::max(affectedRange.startFrame, affectedRange.endFrameExclusive - 1);
    listeners_.call([affectedRange, notifyEndFrame](Listener& l) { l.pitchCurveEdited(affectedRange.startFrame, notifyEndFrame); });
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

        const float segY = makeViewMapper().freqToY(segFreq);
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
    // Note Split 閹貉冨煑闂婃娊鐝崚鍡橆唽闂冨牆鈧》绱檆ents锟?
    segmentationPolicy_.transitionThresholdCents = juce::jlimit(
        OpenTune::PitchControlConfig::kMinNoteSplitCents,
        OpenTune::PitchControlConfig::kMaxNoteSplitCents,
        value);

    // Note Split 娴犲懏娲块弬鏉垮瀻濞堢數鐡ラ悾銉ュ棘閺佸府绱濇稉宥埿曢崣?AUTO 闁插秵鏌婇悽鐔稿灇锟?
    // AUTO 閹垮秳缍旈悽杈╂暏閹磋渹瀵岄崝銊ㄐ曢崣鎴礉娴ｈ法鏁よぐ鎾冲缁涙牜鏆愰幍褑顢戦崚鍡橆唽锟?
    repaint();
}

void PianoRollComponent::resized() {

    auto bounds = getLocalBounds().reduced(12);

    // Reserve space for scrollbars
    horizontalScrollBar_.setBounds(bounds.removeFromBottom(UIColors::scrollBarThickness));
    verticalScrollBar_.setBounds(bounds.removeFromRight(UIColors::scrollBarThickness));

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

    staticDirty_ = true;
    contentDirty_ = true;

    overlay_->setBounds(getLocalBounds());

    // tryConsumeInitialF0View 成功时 activateTimelineCamera 完成一次最终 full raster（含 updateScrollBars）
    if (tryConsumeInitialF0View(editedContentKey_))
        return;

    updateScrollBars();
    rasterizeDirtySurfaces();
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

    if (editedContentKey_.isValid() && audioBuffer_ != nullptr && audioBuffer_->getNumSamples() > 0) {
        waveformMipmapCache_.setAudioSource(editedContentKey_, audioBuffer_);
    } else if (editedContentKey_.isValid() && waveformMipmapCache_.get(editedContentKey_) != nullptr) {
        waveformMipmapCache_.remove(editedContentKey_);
    }
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
    jassert(snap && snap->timeGrid);
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
    jassert(snap && snap->timeGrid);

    const double timeline = makeViewMapper().xToTime(x);
    const double output   = projection.projectTimelineTimeToContent(timeline);
    return snap->timeGrid->tauInverse(output);
}

SourceEditRange PianoRollComponent::sourceEditRange() const
{
    const auto snap = readEditedSnapshot();
    jassert(snap && snap->timeGrid);
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

    // Register waveform mipmap sources (before early-return so late-binding audio works)
    std::set<ContentKey> aliveContents;
    for (const auto& placement : placements) {
        aliveContents.insert(placement.contentKey);
        if (const auto snapshot = readSnapshotFor(placement.contentKey);
            snapshot != nullptr && snapshot->audioBuffer != nullptr) {
            waveformMipmapCache_.setAudioSource(placement.contentKey, snapshot->audioBuffer);
        }
    }
    waveformMipmapCache_.prune(aliveContents);

    explicitTimelineContentPlacements_ = explicitContract;
    if (!changed) {
        return false;
    }

    timelineContentPlacements_ = std::move(placements);

    userScrollHold_ = false;
    updateScrollBars();
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
    requestContentRedraw();
}

void PianoRollComponent::setTimelineContentPlacements(std::vector<TimelineContentPlacement> placements)
{
    if (applyTimelineContentPlacements(std::move(placements), true)) {
        pendingSingleContentProjection_ = activeContentProjection();
        requestContentRedraw();
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
        pendingUndoDescription_ = {};
        beforeUndoNotes_.clear();
        beforeUndoSegments_.clear();
        undoSnapshotCaptured_ = false;
        lastKnownNotesRevision_ = 0;
        lastKnownPitchRevision_ = 0;
        lastKnownTimeGridRevision_ = 0;
    }

    // notes 锟?pitchCurve 闁俺锟?commitNotesAndPitchCurve 閸氬苯鍟撻崚?store锟?
    // 鐠囪鏅舵稊鐔风箑妞よ鎮撶拠浼欑窗curveChanged 閺冭泛绻€锟?refresh notes閿涘苯鎯侀崚?undo/redo 锟?
    // 閸戣櫣锟?curve 閸ョ偤鈧偓锟?notes 鐟欏棜顫庡▓瀣殌閻ㄥ嫪绗夌€靛湱袨閿涘潏achedNotes_ 濠婄偛鎮楅敍澶堚偓?
    if (contentChanged || curveChanged) {
        refreshEditedContentNotes();
    }

    if (contentChanged || curveChanged) {
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
    if (contentChanged || curveChanged)
        requestContentRedraw();
    else {
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        repaint();
    }
}


void PianoRollComponent::requestInitialF0View(ContentKey contentKey)
{
    if (!contentKey.isValid())
        return;

    pendingInitialF0ViewRequests_.insert(contentKey);
    tryConsumeInitialF0View(contentKey);
}

void PianoRollComponent::onTimeGridRevisionChanged()
{
    auto snap = readEditedSnapshot();
    uint64_t rev = snap ? snap->timeGridRevision : 0;
    if (rev == lastKnownTimeGridRevision_) return;
    lastKnownTimeGridRevision_ = rev;
    requestContentRedraw();
}

void PianoRollComponent::onNotesRevisionChanged()
{
    auto snap = readEditedSnapshot();
    uint64_t rev = snap ? snap->notesRevision : 0;
    if (rev == lastKnownNotesRevision_) return;
    lastKnownNotesRevision_ = rev;
    refreshEditedContentNotes();
    requestContentRedraw();
}

void PianoRollComponent::onPitchRevisionChanged()
{
    auto snap = readEditedSnapshot();
    uint64_t rev = snap ? snap->pitchRevision : 0;
    if (rev == lastKnownPitchRevision_) return;
    lastKnownPitchRevision_ = rev;
    requestContentRedraw();
}

void PianoRollComponent::requestContentRedraw() {
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::requestThemeRedraw() {
    staticDirty_ = true;
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

juce::Rectangle<int> PianoRollComponent::getTimelineViewportBounds() const
{
    constexpr int panelInset = 12;
    const int viewportWidth = juce::jmax(0, getWidth() - panelInset - verticalScrollBar_.getWidth());
    const int viewportHeight = juce::jmax(0, getHeight() - panelInset - horizontalScrollBar_.getHeight());
    return { 0, 0, viewportWidth, viewportHeight };
}

juce::Rectangle<int> PianoRollComponent::timeAxisRect() const
{
    return juce::Rectangle<int>(pianoKeyWidth_, 0,
        getTimelineContentViewportWidth(),
        rulerHeight_ + getTimelineContentViewportHeight());
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
    req.currentVisibleStartSeconds = camera_.visibleStartSeconds;
    req.anchorViewportX = anchorViewportX;
    req.viewportWidth = getTimelineContentViewportWidth();
    req.pixelsPerSecond = pps;
    return req;
}

bool PianoRollComponent::tryConsumeInitialF0View(ContentKey contentKey)
{
    if (pendingInitialF0ViewRequests_.find(contentKey) == pendingInitialF0ViewRequests_.end()
        || contentKey != editedContentKey_
        || !isShowing()
        || !isVisible()
        || currentCurve_ == nullptr) {
        return false;
    }

    const auto snapshot = readSnapshotFor(contentKey);
    const auto curveSnapshot = currentCurve_->getSnapshot();
    const auto projection = activeContentProjection();
    const int contentWidth = getTimelineContentViewportWidth();
    const int contentHeight = getTimelineContentViewportHeight();
    if (snapshot == nullptr
        || snapshot->originalF0State != OriginalF0State::Ready
        || curveSnapshot == nullptr
        || snapshot->timeGrid == nullptr
        || snapshot->timeGrid->empty()
        || !projection.isValid()
        || curveSnapshot->getHopSize() <= 0
        || !std::isfinite(curveSnapshot->getSampleRate())
        || curveSnapshot->getSampleRate() <= 0.0
        || contentWidth <= 0
        || contentHeight <= 0
        || camera_.pixelsPerSecond <= 0.0) {
        return false;
    }

    const F0Timeline f0Timeline(curveSnapshot->getHopSize(),
                                curveSnapshot->getSampleRate(),
                                static_cast<int>(curveSnapshot->size()));
    const auto& originalF0 = curveSnapshot->getOriginalF0();
    int firstFrame = -1;
    float startFrequency = 0.0f;
    for (int frame = 0; frame < static_cast<int>(originalF0.size()); ++frame) {
        const float f0 = originalF0[static_cast<size_t>(frame)];
        if (std::isfinite(f0) && f0 >= 20.0f && f0 <= 2000.0f) {
            firstFrame = frame;
            startFrequency = f0;
            break;
        }
    }

    if (firstFrame < 0 || f0Timeline.isEmpty())
        return false;

    const double sourceSeconds = f0Timeline.timeAtFrame(firstFrame);
    const double contentSeconds = snapshot->timeGrid->tauForward(sourceSeconds);
    const double timelineSeconds = projection.projectContentTimeToTimeline(contentSeconds);
    if (!std::isfinite(timelineSeconds))
        return false;

    const auto request = makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        timelineSeconds,
        0.0,
        camera_.pixelsPerSecond);
    const auto mapper = makeViewMapper();
    const float startMidi = mapper.freqToMidi(startFrequency);
    verticalScrollOffset_ = (maxMidi_ - startMidi) * pixelsPerSemitone_ - contentHeight * 0.5f;
    verticalScrollOffset_ = std::clamp(verticalScrollOffset_, 0.0f, getTotalHeight() - contentHeight);

    // 纵向变化 + 相机定位 → 标记脏让 applyRasterCamera 全量重建
    staticDirty_ = true;
    contentDirty_ = true;
    activateTimelineCamera(TimelineViewportPolicy::resolve(request));
    pendingInitialF0ViewRequests_.erase(contentKey);
    return true;
}

void PianoRollComponent::onHeartbeatTick()
{
    if (!isShowing())
        return;

    tryConsumeInitialF0View(editedContentKey_);

    if (zoomPreviewActive_ && zoomDeadlineTicks_ > 0) {
        --zoomDeadlineTicks_;
        if (zoomDeadlineTicks_ == 0) {
            endZoomPreview();
        }
    }

    // DPI 变化检测 → 重建静态表面（标尺/琴键/网格 尺寸变化）
    {
        const int64_t currentDpiMilli = static_cast<int64_t>(
            std::llround(getDesktopScaleFactor() * 1000.0));
        if (currentDpiMilli != lastDpiMilli_) {
            lastDpiMilli_ = currentDpiMilli;
            staticDirty_ = true;
            rasterizeDirtySurfaces();
            repaint();
        }
    }

    if (showWaveform_) {
        bool progressed = false;
        if (inferenceActive_) {
            waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 8;
            if (waveformBuildTickCounter_ == 0)
                progressed = waveformMipmapCache_.buildIncremental(0.15);
        } else {
            waveformBuildTickCounter_ = 0;
            progressed = waveformMipmapCache_.buildIncremental(0.75);
        }

        if (progressed && waveformMipmapCache_.isComplete()) {
            contentDirty_ = true;
            rasterizeDirtySurfaces();
            repaint();
        }
    } else {
        waveformBuildTickCounter_ = 0;
    }

}

void PianoRollComponent::onScrollVBlankCallback(double timestampSec)
{
    if (!isShowing()) {
        return;
    }

    lastVBlankMs_ = juce::Time::getMillisecondCounterHiRes();
    juce::ignoreUnused(timestampSec);

    const bool playingNow = playHeadState_.isPlaying.load(std::memory_order_relaxed);
    bool playStateChanged = (playingNow != lastObservedPlayHeadPlaying_);

    // 旧播放头 bounds + fixedCentre（在 seek/camera 更新前用旧 state 计算）
    struct BoundsWithFixed { juce::Rectangle<int> bounds; bool fixedCentre = false; };
    auto playheadBoundsAt = [this](double t, bool isPlaying, bool continuousFollow) -> BoundsWithFixed {
        const auto mapper = makeViewMapper();
        const int timeX = mapper.timeToX(t);
        const int viewRight = getTimelineViewportBounds().getRight();
        const int centreX = (mapper.contentStartX + viewRight) / 2;
        const auto pres = TimelineViewportPolicy::computePlayheadPresentation(
            timeX, centreX, viewRight, mapper.contentStartX, isPlaying, continuousFollow);
        if (!pres.visible) return {};
        const int anchorX = pres.anchorX;
        const int playheadH = getHeight();
        return { juce::Rectangle<int>(anchorX - 6, 0, 13, playheadH).getIntersection(timeAxisRect()),
                 pres.fixedCentre };
    };

    const double oldCameraStart = camera_.visibleStartSeconds;
    const bool oldIsContFollow = scrollMode_ == ScrollMode::Continuous && !userScrollHold_;
    const bool oldPlaying = lastObservedPlayHeadPlaying_;
    const auto oldPH = playheadBoundsAt(playheadTimeForPaint_, oldPlaying, oldIsContFollow);

    // 播放状态切换
    if (playStateChanged) {
        userScrollHold_ = false;
        lastObservedPlayHeadPlaying_ = playingNow;
    }

    // 解 pending seek
    const double currentPlayheadTime = playHeadState_.timeInSeconds.load(std::memory_order_relaxed);
    double playheadTime = currentPlayheadTime;
    if (pendingSeekTime_ >= 0.0) {
        const auto hostRevision = playHeadState_.hostPositionRevision.load(std::memory_order_acquire);
        if (hostRevision != seekSentRevision_) {
            pendingSeekTime_ = -1.0;
            seekSentRevision_ = 0;
        } else {
            playheadTime = pendingSeekTime_;
        }
    }

    // 自动跟随：仅 playing && !userScrollHold_
    if (playingNow && !userScrollHold_) {
        const double pps = camera_.pixelsPerSecond;
        const auto kind = scrollMode_ == ScrollMode::Continuous
            ? TimelineViewportRequest::Kind::Cont
            : TimelineViewportRequest::Kind::Page;
        const auto resolved = TimelineViewportPolicy::resolve(
            makeViewportRequest(kind, playheadTime, 0.0, pps));
        activateTimelineCamera(resolved);
    }

    // 唯一 playheadTimeForPaint_ 写入
    const bool timeChanged = (playheadTime != playheadTimeForPaint_);
    playheadTimeForPaint_ = playheadTime;

    const bool cameraChanged = (camera_.visibleStartSeconds != oldCameraStart);
    const bool newIsContFollow = scrollMode_ == ScrollMode::Continuous && !userScrollHold_;
    const auto newPH = playheadBoundsAt(playheadTime, playingNow, newIsContFollow);

    // 稳定 CONT 居中且无状态变化 → 不重绘
    const bool stableCont = oldPH.fixedCentre && newPH.fixedCentre;
    const bool needsRepaint = (playStateChanged || timeChanged || cameraChanged) && !stableCont;
    if (needsRepaint) {
        juce::Rectangle<int> repaintBounds = oldPH.bounds.getUnion(newPH.bounds);
        if (!repaintBounds.isEmpty())
            overlay_->repaint(repaintBounds);
    }
}

void PianoRollComponent::commitViewportRequest(TimelineViewportRequest req)
{
    activateTimelineCamera(TimelineViewportPolicy::resolve(req));
}

void PianoRollComponent::activateTimelineCamera(TimelineViewportCamera camera)
{
    camera_ = camera;
    updateScrollBars();
    if (!zoomPreviewActive_)
        applyRasterCamera(camera);
    else
        repaint(timeAxisRect()); // 预览期间仅重绘现有 Image，不栅格化
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
    const ToolId previousTool = currentTool_;
    bool clearedAnchorPreview = false;
    if (interactionState_.drawing.isPlacingAnchors && tool != ToolId::LineAnchor) {
        interactionState_.drawing.isPlacingAnchors = false;
        interactionState_.drawing.pendingAnchors.clear();
        clearedAnchorPreview = true;
    }

    // 閳库槄锟?vocal-time-stretch 锟?.4 (Phase F) 锟?Time tool is mutually exclusive
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
            clearNoteDraft();
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
            // 锟?.4: Time tool uses normal cursor + per-handle hover hand cursor
            // applied by handleTimeToolMouseMove (via ctx.setMouseCursor).
            setMouseCursor(juce::MouseCursor::NormalCursor);
            break;
    }

    // 闁氨鐓￠惄鎴濇儔閼板懎浼愰崗宄板嚒閸掑洦宕查敍鍫濆棘閺佷即娼伴弶鍧楁付鐟曚礁鎮撳銉﹀瘻闁筋噣鐝禍顕嗙礆
    if (toolChanged) {
        listeners_.call([tool](Listener& l) { l.currentToolChanged(tool); });
        }

    if (toolChanged || clearedAnchorPreview) {
        // 只有进出 TimeTool 才改变琴键可见性 → 影响 staticSurface_
        const bool wasTimeView = (previousTool == ToolId::TimeTool);
        const bool isNowTimeView = (currentTool_ == ToolId::TimeTool);
        if (wasTimeView != isNowTimeView) {
            staticDirty_ = true;
            rasterizeDirtySurfaces();
            repaint();
        }
        overlay_->repaint();
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
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setShowLanes(bool shouldShow) {
    if (showLanes_ == shouldShow) return;
    showLanes_ = shouldShow;
    staticDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setNoteNameMode(NoteNameMode noteNameMode) {
    if (noteNameMode_ == noteNameMode) return;
    noteNameMode_ = noteNameMode;
    staticDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setShowUnvoicedFrames(bool shouldShow) {
    if (showUnvoicedFrames_ == shouldShow) return;
    showUnvoicedFrames_ = shouldShow;
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setShowOriginalF0(bool show) {
    if (showOriginalF0_ == show) return;
    showOriginalF0_ = show;
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setShowCorrectedF0(bool show) {
    if (showCorrectedF0_ == show) return;
    showCorrectedF0_ = show;
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setBpm(double bpm) {
    bpm_ = juce::jlimit(60.0, 240.0, bpm);
    staticDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setTimeSignature(int numerator, int denominator) {
    if (numerator <= 0 || denominator <= 0) return;
    timeSigNum_ = numerator;
    timeSigDenom_ = denominator;
    staticDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setTimeUnit(TimeUnit unit) {
    if (timeUnit_ == unit) return;
    timeUnit_ = unit;
    staticDirty_ = true;
    rasterizeDirtySurfaces();
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
    // 閳库槄锟?vocal-time-stretch 锟?.4 锟?Time tool double-click forwarded to handler.
    // Other tools currently have no double-click semantics, so the handler
    // ignores them by switching on currentTool_.
    toolHandler_->mouseDoubleClick(e);
}

void PianoRollComponent::mouseDown(const juce::MouseEvent& e) {
    // Ctrl+drag panning 锟?only on non-interactive area, so existing
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

    if (e.y < rulerHeight_ && e.x < pianoKeyWidth_) {
        return;
    }

    // Piano key audition: click in piano key area triggers note preview.
    if (shouldShowPianoKeys() && e.y >= rulerHeight_ && e.x < pianoKeyWidth_) {
        int midiNote = static_cast<int>(std::ceil(
            makeViewMapper().yToMidi(static_cast<float>(e.y - rulerHeight_))));
        midiNote = juce::jlimit(0, 127, midiNote);
        pressedPianoKey_ = midiNote;
        if (pianoKeyAudition_ != nullptr)
            pianoKeyAudition_->noteOn(midiNote);
        overlay_->repaint();
        return;
    }

    toolHandler_->mouseDown(e);
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent& e) {
    if (interactionState_.isPanning) {
        int deltaX = e.x - interactionState_.dragStartPos.x;
        int deltaY = e.y - interactionState_.dragStartPos.y;
        const double pps = camera_.pixelsPerSecond;

        // 先计算并钳制纵向偏移
        float newScrollY = dragStartVerticalScrollOffset_ - (float)deltaY;
        float maxScroll = getTotalHeight() - getHeight();
        newScrollY = juce::jlimit(0.0f, std::max(0.0f, maxScroll), newScrollY);
        const bool verticalChanged = (newScrollY != verticalScrollOffset_);

        if (verticalChanged) {
            verticalScrollOffset_ = newScrollY;
            staticDirty_ = true;
            contentDirty_ = true;
        }

        // 横向：仅提交视图请求，不设 dirty（applyRasterCamera 走条带路径）
        const double newVisibleStart = camera_.visibleStartSeconds - deltaX / pps;
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            newVisibleStart,
            0.0,
            pps);
        userScrollHold_ = true;
        commitViewportRequest(req);
        return;
    }

    // Piano key glissando: dragging across keys changes the note
    if (shouldShowPianoKeys() && pressedPianoKey_ >= 0) {
        int midiNote = static_cast<int>(std::ceil(
            makeViewMapper().yToMidi(static_cast<float>(e.y - rulerHeight_))));
        midiNote = juce::jlimit(0, 127, midiNote);
        if (midiNote != pressedPianoKey_) {
            if (pianoKeyAudition_ != nullptr) {
                pianoKeyAudition_->noteOff(pressedPianoKey_);
                pianoKeyAudition_->noteOn(midiNote);
            }
            pressedPianoKey_ = midiNote;
            overlay_->repaint();
        }
        return;
    }

    toolHandler_->mouseDrag(e);
    // Tool handlers invalidate either live note content or transient preview
    // feedback directly.
}

void PianoRollComponent::mouseUp(const juce::MouseEvent& e) {
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
        overlay_->repaint();
        return;
    }

    toolHandler_->mouseUp(e);
    grabKeyboardFocus();
}

void PianoRollComponent::handleVerticalZoomWheel(const juce::MouseEvent& e, float deltaY) {
    const auto& settings = zoomSensitivity_;
    float zoomFactor = 1.0f + (deltaY * settings.verticalZoomFactor);
    const float contentY = static_cast<float>(e.y - rulerHeight_);
    float mouseMidi = makeViewMapper().yToMidi(contentY);

    pixelsPerSemitone_ *= zoomFactor;
    pixelsPerSemitone_ = juce::jlimit(5.0f, 60.0f, pixelsPerSemitone_);
    userHasManuallyZoomed_ = true;

    float targetY = (maxMidi_ - mouseMidi) * pixelsPerSemitone_;
    verticalScrollOffset_ = targetY - contentY;
    
    float totalHeight = getTotalHeight();
    float visibleHeight = static_cast<float>(getHeight() - rulerHeight_ - UIColors::scrollBarThickness);
    float maxScroll = totalHeight - visibleHeight;
    if (maxScroll > 0.0f) {
        verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    } else {
        verticalScrollOffset_ = 0.0f;
    }

    // 进入缩放预览事务，冻结表面，到达截止时间后统一重建
    zoomPreviewActive_ = true;
    zoomDeadlineTicks_ = kZoomDeadlineTicks;

    updateScrollBars();
    repaint();
}

void PianoRollComponent::handleHorizontalScrollWheel(float deltaX, float deltaY) {
    const auto& settings = zoomSensitivity_;
    float scrollDelta = (deltaX != 0 ? deltaX : deltaY);
    const double pixelDelta = static_cast<double>(scrollDelta) * static_cast<double>(settings.scrollSpeed);
    const double pps = camera_.pixelsPerSecond;
    const double newVisibleStart = camera_.visibleStartSeconds - pixelDelta / pps;
    userScrollHold_ = true;
    const auto req = makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        newVisibleStart,
        0.0,
        pps);
    commitViewportRequest(req);
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
        staticDirty_ = true;
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        updateScrollBars();
        repaint();
    }

void PianoRollComponent::handleHorizontalZoomWheel(const juce::MouseEvent& e, float deltaY) {
    if (!zoomPreviewActive_ || zoomAnchorTime_ < 0.0) {
        beginZoomPreview(e, deltaY);
    } else {
        updateZoomPreview(deltaY);
    }
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

void PianoRollComponent::beginZoomPreview(const juce::MouseEvent& e, float deltaY) {
    const auto& settings = zoomSensitivity_;
    const double zoomFactor = 1.0 + static_cast<double>(deltaY) * settings.horizontalZoomFactor;
    const double oldPps = camera_.pixelsPerSecond;
    const double newPps = TimelineViewportPolicy::normalisePixelsPerSecond(
        oldPps * zoomFactor,
        TimelineViewportRequest::ViewKind::PianoRoll);
    const int mouseX = e.x - pianoKeyWidth_;
    const double mouseTime = camera_.visibleStartSeconds + mouseX / oldPps;

    zoomPreviewActive_ = true;
    zoomAnchorTime_ = mouseTime;
    zoomAnchorViewportX_ = mouseX;
    zoomDeadlineTicks_ = kZoomDeadlineTicks;

    userHasManuallyZoomed_ = true;
    commitViewportRequest(makeViewportRequest(
        TimelineViewportRequest::Kind::Zoom,
        zoomAnchorTime_,
        static_cast<double>(zoomAnchorViewportX_),
        newPps));
}

void PianoRollComponent::updateZoomPreview(float deltaY) {
    const auto& settings = zoomSensitivity_;
    const double zoomFactor = 1.0 + static_cast<double>(deltaY) * settings.horizontalZoomFactor;
    const double newPps = TimelineViewportPolicy::normalisePixelsPerSecond(
        camera_.pixelsPerSecond * zoomFactor,
        TimelineViewportRequest::ViewKind::PianoRoll);

    zoomDeadlineTicks_ = kZoomDeadlineTicks;
    commitViewportRequest(makeViewportRequest(
        TimelineViewportRequest::Kind::Zoom,
        zoomAnchorTime_,
        static_cast<double>(zoomAnchorViewportX_),
        newPps));
}

void PianoRollComponent::endZoomPreview() {
    zoomPreviewActive_ = false;
    zoomDeadlineTicks_ = 0;
    zoomAnchorTime_ = -1.0; // 重置锚点哨兵
    // 关闭预览后恰好一次完成两张表面的最终精确重建
    staticDirty_ = true;
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

bool PianoRollComponent::keyPressed(const juce::KeyPress& key) {
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

std::optional<PianoRollRenderer::ContentRenderItem> PianoRollComponent::buildContentRenderItem(
    const TimelineContentPlacement& placement) const
{
    auto snap = readSnapshotFor(placement.contentKey);
    if (!snap || !snap->timeGrid)
        return std::nullopt;

    PianoRollRenderer::ContentRenderItem item;
    item.contentKey = placement.contentKey;
    item.projection = placement.projection;
    item.timeGrid = snap->timeGrid;
    item.active = placement.contentKey == editedContentKey_;

    std::shared_ptr<PitchCurve> curve;
    if (item.active) {
        curve = currentCurve_;
        item.audioBuffer = audioBuffer_;
        item.ownerSnapshot = readEditedSnapshot();
        item.displayNotes = &getDisplayedNotes();  // includes live note draft when active
    } else {
        curve = snap->pitchCurve;
        item.audioBuffer = snap->audioBuffer;
        item.ownerSnapshot = snap;
        item.displayNotes = &item.ownerSnapshot->notes;
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

void PianoRollComponent::visibilityChanged()
{
    // When component becomes visible, automatically grab keyboard focus
    // This ensures user can use shortcuts (e.g., Ctrl+A to select all) without
    // manual click.
    if (isShowing() && isVisible())
    {
        tryConsumeInitialF0View(editedContentKey_);
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
    }
}

void PianoRollComponent::setReferenceOverlay(std::optional<PianoRollRenderer::ReferenceOverlay> overlay)
{
    if (overlay && overlay->enabled) {
        const auto& srcProj = overlay->sourceProjection;
        const TimelineContentPlacement* matchedPlacement = nullptr;
        bool ambiguousMatch = false;
        for (const auto& placement : timelineContentPlacements_) {
            const auto& p = placement.projection;
            if (std::abs(p.timelineStartSeconds - srcProj.timelineStartSeconds) < 1e-6
                && std::abs(p.timelineDurationSeconds - srcProj.timelineDurationSeconds) < 1e-6
                && std::abs(p.contentDurationSeconds - srcProj.contentDurationSeconds) < 1e-6) {
                if (matchedPlacement != nullptr) {
                    ambiguousMatch = true;
                    break;
                }
                matchedPlacement = &placement;
            }
        }
        if (matchedPlacement != nullptr && !ambiguousMatch) {
            if (auto snap = readSnapshotFor(matchedPlacement->contentKey); snap && snap->timeGrid) {
                overlay->timeGrid = snap->timeGrid;
            } else {
                overlay.reset();
            }
        } else {
            overlay.reset();
        }
    }
    referenceOverlay_ = std::move(overlay);
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

int PianoRollComponent::getTimelineContentViewportWidth() const
{
    return juce::jmax(0, getTimelineViewportBounds().getWidth() - pianoKeyWidth_);
}

int PianoRollComponent::getTimelineContentViewportHeight() const
{
    return juce::jmax(0, getTimelineViewportBounds().getHeight() - rulerHeight_);
}

void PianoRollComponent::setScale(int rootNote, int scaleType)
{
    const int clampedRoot = juce::jlimit(0, 11, rootNote);
    const int clampedType = juce::jlimit(1, 8, scaleType);
    if (scaleRootNote_ == clampedRoot && scaleType_ == clampedType)
        return;
    scaleRootNote_ = clampedRoot;
    scaleType_ = clampedType;
    staticDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::fitToScreen() {
    // 婵″倹鐏夐悽銊﹀煕瀹稿弶澧滈崝銊ㄧ殶閺佺绻冪紓鈺傛杹閿涘奔绗夐懛顏勫З鐟曞棛锟?
    if (userHasManuallyZoomed_) {
        return;
    }

    // 1. Vertical Fit: Show C1 to C8 (minMidi_ to maxMidi_)
    // Total range: maxMidi_ - minMidi_
    // Available height: getHeight()
    const auto timelineViewportBounds = getTimelineViewportBounds();
    float range = maxMidi_ - minMidi_ + 1.0f;
    if (range > 0 && timelineViewportBounds.getHeight() > 0) {
        pixelsPerSemitone_ = static_cast<float>(timelineViewportBounds.getHeight()) / range;
        
        // Reset scroll to show top
        verticalScrollOffset_ = 0; 
        staticDirty_ = true;
        contentDirty_ = true;
        // 不在此分支栅格；最终 commitViewportRequest 通过 applyRasterCamera 一次性完整重建
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
        commitViewportRequest(req);
    } else {
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            0.0,
            0.0,
            TimelineViewportCamera::kDefaultPixelsPerSecond);
        commitViewportRequest(req);
    }
}

float PianoRollComponent::getTotalHeight() const {
    return (maxMidi_ - minMidi_ + 1.0f) * pixelsPerSemitone_;
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

    auto snapshot = currentCurve_->getSnapshot();
    if (!snapshot) {
        return { AutoTuneApplyStatus::MissingCurveSnapshot };
    }

    const auto& originalF0 = snapshot->getOriginalF0();
    if (originalF0.empty()) {
        return { AutoTuneApplyStatus::EmptyOriginalF0 };
    }
    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) {
        return { AutoTuneApplyStatus::EmptyTimeline };
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
        return { AutoTuneApplyStatus::NoTargetSelection };
    }

    const auto targetRange = f0tl.rangeForFrames(targetDecision.range.startFrame,
                                                 targetDecision.range.endFrameExclusive);
    if (targetRange.isEmpty()) {
        return { AutoTuneApplyStatus::EmptyTargetRange };
    }

    const int startFrame = targetRange.startFrame;
    const int endFrame = targetRange.endFrameExclusive - 1;

    const bool useScaleSnap = (scaleType_ != 3);

    NoteGeneratorParams genParams;
    genParams.policy = segmentationPolicy_;
    genParams.retuneSpeed = currentRetuneSpeed_;
    genParams.vibratoDepth = currentVibratoDepth_;
    genParams.vibratoRate = currentVibratoRate_;

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

    // Synchronous generation (no worker)
    auto generatedNotes = LegacyNoteGenerator::generate(
        originalF0.data(),
        static_cast<int>(originalF0.size()),
        nullptr,
        startFrame,
        endFrame + 1,
        currentCurve_->getHopSize(),
        static_cast<float>(currentCurve_->getSampleRate()),
        genParams);

    if (postSnapCfg.has_value()) {
        postSnapCfg->applyToNotes(generatedNotes);
    }
    LegacyNoteGenerator::validate(generatedNotes);

    captureBeforeUndoSnapshot();
    pendingUndoDescription_ = TRANS("自动调音");

    if (!contentCommands_->commitAutoTuneGeneratedNotes(
            editedContentKey_,
            generatedNotes,
            startFrame,
            endFrame + 1,
            currentRetuneSpeed_,
            currentVibratoDepth_,
            currentVibratoRate_)) {
        return { AutoTuneApplyStatus::NoContent };
    }

    {
        auto autoSnap = readEditedSnapshot();
        if (autoSnap) {
            lastKnownNotesRevision_ = autoSnap->notesRevision;
            lastKnownPitchRevision_ = autoSnap->pitchRevision;
        }
    }

    refreshEditedContentNotes();

    auto committedCurve = readEditedSnapshot();
    if (committedCurve != nullptr && committedCurve->pitchCurve != nullptr) {
        setEditedContent(editedContentKey_, committedCurve->pitchCurve, audioBuffer_, static_cast<int>(audioBufferSampleRate_));
    }

    const auto autoTuneRange = PitchCurve::expandNoteBasedCorrectionRange(
        startFrame, endFrame + 1, f0tl.endFrameExclusive());
    recordUndoAction(pendingUndoDescription_, autoTuneRange);

    return { AutoTuneApplyStatus::Applied };
}

void PianoRollComponent::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) {
    if (scrollBar == &horizontalScrollBar_) {
        const double pps = camera_.pixelsPerSecond;
        userScrollHold_ = true;
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            newRangeStart,
            0.0,
            pps);
        commitViewportRequest(req);
    } else if (scrollBar == &verticalScrollBar_) {
        verticalScrollOffset_ = static_cast<float>(newRangeStart);
        staticDirty_ = true;
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        updateScrollBars();
        repaint();
    }
}

std::vector<Note> PianoRollComponent::getEditedContentNotesCopy() const {
    return cachedNotes_;
}

void PianoRollComponent::updateScrollBars() {
    int visibleWidth = getTimelineContentViewportWidth();
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
        pendingSeekTime_ >= 0.0 ? pendingSeekTime_ : playHeadState_.timeInSeconds.load(std::memory_order_relaxed));

    horizontalScrollBar_.setRangeLimits(
        range.absoluteStartSeconds,
        range.absoluteEndSeconds,
        juce::dontSendNotification);
    horizontalScrollBar_.setCurrentRange(
        range.visibleStartSeconds,
        range.visibleDuration,
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
    // Absolute timeline starts from zero.
    double maxEndSeconds = 0.0;

    for (const auto& placement : timelineContentPlacements_) {
        if (placement.isValid()) {
            maxEndSeconds = std::max(maxEndSeconds, placement.projection.timelineEndSeconds());
        }
    }

    // 锟?placement 鏃朵娇锟?audio 锟?notes 鐨勫疄锟?duration
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

} // namespace OpenTune
