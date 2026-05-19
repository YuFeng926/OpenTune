#include "PianoRollToolHandler.h"
#include "../../../Utils/AudioEditingScheme.h"
#include "../../../Utils/PitchUtils.h"
#include "../../../Utils/AppLogger.h"
#include "../../../Utils/KeyShortcutConfig.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace OpenTune {

using ManualOp = PianoRollToolHandler::ManualCorrectionOp;

namespace {

void selectNotesForEditedFrameRange(PianoRollToolHandler::Context& ctx,
                                    int startFrame,
                                    int endFrameExclusive)
{
    if (!AudioEditingScheme::shouldSelectNotesForEditedFrameRange(ctx.getAudioEditingScheme())
        || !ctx.selectNotesOverlappingFrames) {
        return;
    }

    if (ctx.clearLineAnchorSegmentSelection) {
        ctx.clearLineAnchorSegmentSelection();
    }

    ctx.selectNotesOverlappingFrames(startFrame, endFrameExclusive);
}

void appendManualCorrectionOps(std::vector<ManualOp>& outOps,
                               AudioEditingScheme::Scheme scheme,
                               const std::vector<float>& originalF0,
                               AudioEditingScheme::FrameRange requestedRange,
                               const std::function<float(int)>& valueForFrame,
                               CorrectedSegment::Source source)
{
    const auto trimmedRange = AudioEditingScheme::trimFrameRangeToEditableBounds(scheme, originalF0, requestedRange);
    if (!trimmedRange.isValid()) {
        return;
    }

    int currentOpStart = -1;
    std::vector<float> currentOpData;

    auto flushCurrentOp = [&](int endFrameExclusive) {
        if (currentOpStart < 0 || currentOpData.empty()) {
            currentOpStart = -1;
            currentOpData.clear();
            return;
        }

        ManualOp op;
        op.startFrame = currentOpStart;
        op.endFrameExclusive = endFrameExclusive;
        op.f0Data = std::move(currentOpData);
        op.source = source;
        outOps.push_back(std::move(op));

        currentOpStart = -1;
        currentOpData.clear();
    };

    for (int frame = trimmedRange.startFrame; frame < trimmedRange.endFrameExclusive; ++frame) {
        if (!AudioEditingScheme::canEditFrame(scheme, originalF0, frame)) {
            flushCurrentOp(frame);
            continue;
        }

        const float frameValue = valueForFrame(frame);
        if (frameValue <= 0.0f) {
            flushCurrentOp(frame);
            continue;
        }

        if (currentOpStart < 0) {
            currentOpStart = frame;
        }
        currentOpData.push_back(frameValue);
    }

    flushCurrentOp(trimmedRange.endFrameExclusive);
}

float interpolateLogF0(float leftF0, float rightF0, float t)
{
    const float logLeft = std::log2(std::max(leftF0, 1.0f));
    const float logRight = std::log2(std::max(rightF0, 1.0f));
    return std::pow(2.0f, logLeft + (logRight - logLeft) * t);
}

struct LogLineFit
{
    float intercept = 0.0f;
    float slope = 0.0f;
};

LogLineFit fitOriginalF0LogTrend(const std::vector<float>& originalF0,
                                 int startFrame,
                                 int endFrameExclusive)
{
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXX = 0.0;
    double sumXY = 0.0;
    int count = 0;

    for (int frame = startFrame; frame < endFrameExclusive; ++frame) {
        const float sourceF0 = originalF0[static_cast<std::size_t>(frame)];
        if (sourceF0 <= 0.0f) continue;

        const double x = static_cast<double>(frame - startFrame);
        const double y = std::log2(sourceF0);
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
        ++count;
    }

    if (count == 0) {
        return {};
    }

    const double n = static_cast<double>(count);
    const double denom = n * sumXX - sumX * sumX;
    LogLineFit fit;
    fit.slope = denom != 0.0
        ? static_cast<float>((n * sumXY - sumX * sumY) / denom)
        : 0.0f;
    fit.intercept = static_cast<float>((sumY - static_cast<double>(fit.slope) * sumX) / n);
    return fit;
}

float lineAnchorF0WithSourceShape(const std::vector<float>& originalF0,
                                  int frame,
                                  int startFrame,
                                  int endFrameExclusive,
                                  float leftTargetF0,
                                  float rightTargetF0,
                                  float retuneSpeed,
                                  LogLineFit sourceTrend)
{
    const int spanFrames = std::max(1, endFrameExclusive - startFrame);
    const float t = std::clamp(static_cast<float>(frame - startFrame) / static_cast<float>(spanFrames),
                               0.0f,
                               1.0f);
    const float targetLineF0 = interpolateLogF0(leftTargetF0, rightTargetF0, t);
    const float shapeAmount = 1.0f - std::clamp(retuneSpeed, 0.0f, 1.0f);
    if (shapeAmount <= 0.0f) {
        return targetLineF0;
    }

    const float sourceF0 = originalF0[static_cast<std::size_t>(frame)];
    if (sourceF0 <= 0.0f) {
        return targetLineF0;
    }

    const float logShape = std::log2(sourceF0)
        - (sourceTrend.intercept + sourceTrend.slope * static_cast<float>(frame - startFrame));
    return targetLineF0 * std::pow(2.0f, logShape * shapeAmount);
}

std::vector<Note>& workingDraftNotes(PianoRollToolHandler::Context& ctx)
{
    return ctx.getNoteDraft().workingNotes;
}

void resetDraftNotesToBaseline(PianoRollToolHandler::Context& ctx)
{
    ctx.getNoteDraft().workingNotes = ctx.getNoteDraft().baselineNotes;
}

std::vector<CorrectedSegment> buildSegmentsWithManualOps(
    const std::shared_ptr<PitchCurve>& pitchCurve,
    const std::vector<PianoRollToolHandler::ManualCorrectionOp>& ops)
{
    std::vector<CorrectedSegment> segments;
    if (pitchCurve == nullptr) {
        return segments;
    }

    auto editedCurve = pitchCurve->clone();
    for (const auto& op : ops) {
        if (op.endFrameExclusive <= op.startFrame) {
            continue;
        }

        editedCurve->setManualCorrectionRange(op.startFrame,
                                              op.endFrameExclusive,
                                              op.f0Data,
                                              op.source);
    }

    const auto snapshot = editedCurve->getSnapshot();
    segments.reserve(snapshot->getCorrectedSegments().size());
    for (const auto& segment : snapshot->getCorrectedSegments()) {
        segments.push_back(segment);
    }
    return segments;
}

bool commitNoteBasedCorrection(PianoRollToolHandler::Context& ctx,
                               const std::vector<Note>& notes,
                               const std::shared_ptr<PitchCurve>& pitchCurve,
                               F0FrameRange editRange)
{
    const auto f0tl = ctx.getF0Timeline();
    auto clonedCurve = pitchCurve->clone();
    clonedCurve->applyCorrectionToRange(notes,
                                        editRange.startFrame,
                                        editRange.endFrameExclusive,
                                        ctx.getRetuneSpeed(),
                                        ctx.getVibratoDepth(),
                                        ctx.getVibratoRate(),
                                        44100.0);

    const auto snap = clonedCurve->getSnapshot();
    const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(editRange.startFrame,
                                                                          editRange.endFrameExclusive,
                                                                          f0tl.endFrameExclusive());
    if (!ctx.commitNotesAndSegments(notes, snap->getCorrectedSegments(), affectedRange)) {
        return false;
    }

    ctx.notifyPitchCurveEdited(affectedRange.startFrame, affectedRange.endFrameExclusive - 1);
    return true;
}

const std::vector<Note>& committedNotes(PianoRollToolHandler::Context& ctx)
{
    return ctx.getCommittedNotes();
}

const std::vector<Note>& displayNotes(PianoRollToolHandler::Context& ctx)
{
    return ctx.getDisplayNotes();
}

const std::vector<Note>& draftBaselineNotes(PianoRollToolHandler::Context& ctx)
{
    return ctx.getNoteDraft().baselineNotes;
}

void clearNoteDragPreview(PianoRollToolHandler::Context& ctx)
{
    ctx.setNoteDragPreviewStartFrame(-1);
    ctx.setNoteDragPreviewEndFrameExclusive(-1);
    ctx.getNoteDragPreviewF0().clear();
}

bool hasNoteDragPreview(PianoRollToolHandler::Context& ctx)
{
    return ctx.getNoteDragPreviewStartFrame() >= 0
        && ctx.getNoteDragPreviewEndFrameExclusive() > ctx.getNoteDragPreviewStartFrame()
        && !ctx.getNoteDragPreviewF0().empty();
}

void updateNoteDragPreview(PianoRollToolHandler::Context& ctx, float shiftFactor)
{
    clearNoteDragPreview(ctx);

    const auto f0tl = ctx.getF0Timeline();
    if (f0tl.isEmpty()) return;
    const auto& manualTargets = ctx.getNoteDragInitialManualTargets();
    const double manualStartTime = ctx.getNoteDragManualStartTime();
    const double manualEndTime = ctx.getNoteDragManualEndTime();
    if (manualStartTime < 0.0 || manualEndTime <= manualStartTime || manualTargets.empty()) {
        return;
    }

    const auto manualRange = f0tl.rangeForTimes(manualStartTime, manualEndTime);
    if (manualRange.isEmpty()) {
        return;
    }

    ctx.setNoteDragPreviewStartFrame(manualRange.startFrame);
    ctx.setNoteDragPreviewEndFrameExclusive(manualRange.endFrameExclusive);

    auto& preview = ctx.getNoteDragPreviewF0();
    preview.assign(static_cast<std::size_t>(manualRange.endFrameExclusive - manualRange.startFrame), -1.0f);
    for (const auto& fv : manualTargets) {
        const int frame = f0tl.frameAtOrBefore(fv.first);
        const int relIndex = frame - manualRange.startFrame;
        if (relIndex >= 0 && relIndex < static_cast<int>(preview.size())) {
            preview[static_cast<std::size_t>(relIndex)] = fv.second * shiftFactor;
        }
    }
}

void invalidateIfNeeded(PianoRollToolHandler::Context& ctx, const juce::Rectangle<int>& dirtyArea)
{
    if (!dirtyArea.isEmpty()) {
        ctx.invalidateVisual(dirtyArea);
    }
}

void invalidateNoteChange(PianoRollToolHandler::Context& ctx,
                          const std::vector<Note>& before,
                          const std::vector<Note>& after)
{
    auto dirty = ctx.getNotesBounds(before).getUnion(ctx.getNotesBounds(after));
    dirty = dirty.getUnion(ctx.getSelectionBounds());
    dirty = dirty.getUnion(ctx.getNoteDragCurvePreviewBounds());
    invalidateIfNeeded(ctx, dirty);
}

}

// ============================================================================
// PianoRollToolHandler - 钢琴卷帘工具处理器实现
// ============================================================================

PianoRollToolHandler::PianoRollToolHandler(Context context)
    : ctx_(std::move(context))
{}

void PianoRollToolHandler::setTool(ToolId tool)
{
    if (currentTool_ != tool) {
        cancelActiveMouseGesture();
    }
    currentTool_ = tool;
}

double PianoRollToolHandler::pixelXToSourceTime(int pixelX) const
{
    // Stage 1: pixel → output (timeline coords).
    const double timelineTime = ctx_.xToTime(pixelX);
    // Stage 2: timeline → output (materialization coords).
    const double outputMatTime = ctx_.projectTimelineTimeToMaterialization
        ? ctx_.projectTimelineTimeToMaterialization(timelineTime)
        : timelineTime;
    // Stage 3: output → source via tauInverse (vocal-time-stretch §8.5).
    if (ctx_.getTimeGridSnapshot) {
        if (auto snap = ctx_.getTimeGridSnapshot()) {
            if (!snap->isIdentity()) {
                return snap->tauInverse(outputMatTime);
            }
        }
    }
    return outputMatTime;
}

void PianoRollToolHandler::mouseMove(const juce::MouseEvent& e)
// 鼠标移动处理：更新光标形状（音符边缘调整、线锚点预览）
{
    if (currentTool_ == ToolId::LineAnchor && ctx_.getState().drawing.isPlacingAnchors) {
        const auto dirtyBefore = ctx_.getLineAnchorPreviewBounds();
        ctx_.getState().drawing.currentMousePos = e.position;
        invalidateIfNeeded(ctx_, dirtyBefore.getUnion(ctx_.getLineAnchorPreviewBounds()));
        return;
    }

    if (currentTool_ == ToolId::TimeTool) {
        handleTimeToolMouseMove(e);
        return;
    }

    if (currentTool_ != ToolId::Select) {
        return;
    }

    int edgeThreshold = 6;
    
    bool cursorSet = false;
    float mousePitch = ctx_.yToFreq((float)e.y);
    float mouseMidiVal = 69.0f + 12.0f * std::log2(mousePitch / 440.0f) - 0.5f;

    for (const auto& note : displayNotes(ctx_)) {
        int x1 = ctx_.timeToX(ctx_.projectMaterializationTimeToTimeline(note.startTime));
        int x2 = ctx_.timeToX(ctx_.projectMaterializationTimeToTimeline(note.endTime));
        
        bool nearLeft = std::abs(e.x - x1) <= edgeThreshold;
        bool nearRight = std::abs(e.x - x2) <= edgeThreshold;
        
        float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f) - 0.5f;
        bool onNote = std::abs(mouseMidiVal - noteMidi) < 1.0f;
        
        if ((nearLeft || nearRight) && onNote) {
            ctx_.setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            cursorSet = true;
            break;
        } else if (e.x >= x1 && e.x <= x2 && onNote) {
            ctx_.setMouseCursor(juce::MouseCursor::UpDownLeftRightResizeCursor);
            cursorSet = true;
            break;
        }
    }

    if (!cursorSet) {
        ctx_.setMouseCursor(juce::MouseCursor::NormalCursor);
    }
}

void PianoRollToolHandler::mouseDown(const juce::MouseEvent& e)
{
    ctx_.grabKeyboardFocus();
    ctx_.getState().emptySpaceIntent.clear();

    if (e.mods.isPopupMenu()) {
        if (currentTool_ == ToolId::LineAnchor && ctx_.getState().drawing.isPlacingAnchors) {
            const auto dirtyBefore = ctx_.getLineAnchorPreviewBounds();
            ctx_.getState().drawing.isPlacingAnchors = false;
            ctx_.getState().drawing.pendingAnchors.clear();
            ctx_.clearLineAnchorSegmentSelection();
            invalidateIfNeeded(ctx_, dirtyBefore.getUnion(ctx_.getLineAnchorPreviewBounds()));
            return;
        }
        showToolContextMenu(e);
        return;
    }

    constexpr int inset = 12;
    constexpr int rulerHeight = 30;
    constexpr int timelineExtendedHitArea = 20;
    const int timelineBottomExtended = inset + rulerHeight + timelineExtendedHitArea;
    if (e.y < timelineBottomExtended && e.x > ctx_.getPianoKeyWidth()) {
        double clickedTime = ctx_.xToTime(e.x);
        if (clickedTime >= 0) {
            ctx_.notifyPlayheadChange(clickedTime);
        }
        return;
    }

    dragStartPos_ = e.getPosition();

    if (isEmptySpaceMouseDown(e)) {
        beginEmptySpaceIntent(e);
        return;
    }

    switch (currentTool_) {
        case ToolId::AutoTune:
            handleAutoTuneTool(e);
            break;
        case ToolId::Select:
            handleSelectTool(e);
            break;
        case ToolId::DrawNote:
            handleDrawNoteMouseDown(e);
            break;
        case ToolId::HandDraw:
            ctx_.getState().handDrawPendingDrag = true;
            break;
        case ToolId::LineAnchor:
            handleLineAnchorMouseDown(e);
            break;
        case ToolId::TimeTool:
            handleTimeToolMouseDown(e);
            break;
        default:
            AppLogger::warn("[PianoRollToolHandler] mouseDown: unknown tool " + juce::String(static_cast<int>(currentTool_)));
            break;
    }
}

void PianoRollToolHandler::mouseDrag(const juce::MouseEvent& e)
{
    if (consumeEmptySpaceIntentDrag(e)) {
        return;
    }

    switch (currentTool_) {
        case ToolId::Select:
            handleSelectDrag(e);
            break;
        case ToolId::DrawNote:
            handleDrawNoteDrag(e);
            break;
        case ToolId::HandDraw:
            if (ctx_.getState().handDrawPendingDrag) {
                int dx = e.x - dragStartPos_.x;
                int dy = e.y - dragStartPos_.y;
                int threshold = ctx_.getDragThreshold();
                if (dx * dx + dy * dy > threshold * threshold) {
                    ctx_.getState().handDrawPendingDrag = false;
                    handleDrawCurveTool(e);
                }
            } else if (ctx_.getState().drawing.isDrawingF0) {
                handleDrawCurveTool(e);
            }
            break;
        case ToolId::LineAnchor:
            handleLineAnchorMouseDrag(e);
            break;
        case ToolId::TimeTool:
            handleTimeToolMouseDrag(e);
            break;
        default:
            break;
    }
}

void PianoRollToolHandler::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (currentTool_ == ToolId::TimeTool) {
        handleTimeToolMouseDoubleClick(e);
    }
    // Other tools: no-op (could be extended later for note resize / etc.)
}

void PianoRollToolHandler::mouseUp(const juce::MouseEvent& e)
{
    if (consumeEmptySpaceIntentUp(e)) {
        return;
    }

    if (ctx_.getState().selection.hasSelectionArea) {
        double timeDelta = std::abs(ctx_.getState().selection.selectionEndTime - ctx_.getState().selection.selectionStartTime);
        float midiDelta = std::abs(ctx_.getState().selection.selectionEndMidi - ctx_.getState().selection.selectionStartMidi);
        if (timeDelta < 0.01 || midiDelta < 0.5f) {
            ctx_.getState().selection.hasSelectionArea = false;
        }
    }

    switch (currentTool_) {
        case ToolId::Select:
            handleSelectUp(e);
            break;
        case ToolId::HandDraw:
            handleDrawCurveUp(e);
            break;
        case ToolId::DrawNote:
            handleDrawNoteUp(e);
            break;
        case ToolId::TimeTool:
            handleTimeToolMouseUp(e);
            break;
        default:
            ctx_.getState().noteDrag.draggedNoteIndex = -1;
            break;
    }
}

bool PianoRollToolHandler::keyPressed(const juce::KeyPress& key)
{
    const auto& shortcutSettings = ctx_.getShortcutSettings();

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::SelectAll, key)) {
        const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));
        ctx_.beginNoteDraft();
        auto& notes = workingDraftNotes(ctx_);
        auto curve = ctx_.getPitchCurve();
        bool hasNotes = !notes.empty();
        bool hasCurve = curve && !curve->isEmpty();
        
        if (!hasNotes && !hasCurve) {
            return true;
        }
        
        if (hasNotes) {
            selectAllNotes(notes);
            ctx_.setUndoDescription(juce::String("编辑音符"));
            ctx_.commitNoteDraft();
        }
        
        ctx_.getState().selection.hasSelectionArea = true;
        ctx_.getState().selection.selectionStartMidi = ctx_.getMinMidi();
        ctx_.getState().selection.selectionEndMidi = ctx_.getMaxMidi();
        ctx_.getState().selection.selectionStartTime = 0.0;
        
        if (hasCurve) {
            const auto f0tl = ctx_.getF0Timeline();
            ctx_.getState().selection.selectionEndTime = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(f0tl.endFrameExclusive());
        } else {
            double maxEnd = 0.0;
            for (const auto& n : notes) {
                maxEnd = std::max(maxEnd, n.endTime);
            }
            ctx_.getState().selection.selectionEndTime = maxEnd;
        }
        
        updateF0SelectionFromNotes(notes);
        invalidateNoteChange(ctx_, beforeNotes, committedNotes(ctx_));
        return true;
    }

    if (!key.getModifiers().isAnyModifierKeyDown()) {
        if (key.getTextCharacter() == '2') {
            ctx_.setCurrentTool(ToolId::DrawNote);
            return true;
        }

        if (key.getTextCharacter() == '3') {
            ctx_.setCurrentTool(ToolId::Select);
            return true;
        }

        if (key.getTextCharacter() == '4') {
            ctx_.setCurrentTool(ToolId::LineAnchor);
            return true;
        }

        if (key.getTextCharacter() == '5') {
            ctx_.setCurrentTool(ToolId::HandDraw);
            return true;
        }

        // ⚡️ vocal-time-stretch §8.1 (Phase E scaffolding):
        // Time tool key shortcut. Phase F adds the full ToolHandler / Renderer
        // wiring + UI cursor / handle dragging behavior; this commit lands the
        // tool selection plumbing only.
        if (key.getTextCharacter() == 't' || key.getTextCharacter() == 'T') {
            ctx_.setCurrentTool(ToolId::TimeTool);
            return true;
        }

        if (key.getTextCharacter() == '6') {
            ctx_.notifyAutoTuneRequested();
            return true;
        }
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::PlayPause, key)) {
        ctx_.notifyPlayPauseToggle();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Stop, key)) {
        ctx_.notifyStopPlayback();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Delete, key) ||
        key.getTextCharacter() == '1') {
        // ⚡️ vocal-time-stretch §8.4 (Phase G): Time tool always consumes
        // Delete to avoid accidentally deleting notes when a handle isn't
        // selected.  No-op when nothing's selected.
        if (currentTool_ == ToolId::TimeTool) {
            handleTimeToolDeleteSelected();
            return true;
        }
        handleDeleteKey();
        return true;
    }

    if (key == juce::KeyPress::escapeKey) {
        ctx_.notifyEscapeKey();
        return true;
    }

    return false;
}

bool PianoRollToolHandler::isEmptySpaceMouseDown(const juce::MouseEvent& e)
{
    if (e.x <= ctx_.getPianoKeyWidth()) {
        return false;
    }

    if (currentTool_ == ToolId::LineAnchor) {
        return false;
    }

    if (hitsNoteBodyOrResizeEdge(e)) {
        return false;
    }

    return true;
}

bool PianoRollToolHandler::hitsNoteBodyOrResizeEdge(const juce::MouseEvent& e)
{
    const auto projection = ctx_.getMaterializationProjection();
    double time = ctx_.projectTimelineTimeToMaterialization(ctx_.xToTime(e.x));
    if (projection.isValid()) {
        time = projection.clampMaterializationTime(time);
    }

    if (time < 0.0) {
        return false;
    }

    const float clickedPitch = ctx_.yToFreq(static_cast<float>(e.y));
    const float mouseMidi = 69.0f + 12.0f * std::log2(clickedPitch / 440.0f) - 0.5f;
    constexpr int edgeThreshold = 6;

    for (const auto& note : displayNotes(ctx_)) {
        const float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f) - 0.5f;
        if (std::abs(mouseMidi - noteMidi) >= 1.0f) {
            continue;
        }

        const int x1 = ctx_.timeToX(ctx_.projectMaterializationTimeToTimeline(note.startTime));
        const int x2 = ctx_.timeToX(ctx_.projectMaterializationTimeToTimeline(note.endTime));
        const bool insideBody = e.x >= x1 && e.x <= x2;
        const bool nearEdge = std::abs(e.x - x1) <= edgeThreshold || std::abs(e.x - x2) <= edgeThreshold;
        if (insideBody || nearEdge) {
            return true;
        }
    }

    return false;
}

void PianoRollToolHandler::beginEmptySpaceIntent(const juce::MouseEvent& e)
{
    auto& intent = ctx_.getState().emptySpaceIntent;
    intent.active = true;
    intent.tool = currentTool_;
    intent.mouseDownPos = e.getPosition();
    intent.mouseDownTime = ctx_.xToTime(e.x);
}

bool PianoRollToolHandler::consumeEmptySpaceIntentDrag(const juce::MouseEvent& e)
{
    auto& intent = ctx_.getState().emptySpaceIntent;
    if (!intent.active) {
        return false;
    }

    const int dx = e.x - intent.mouseDownPos.x;
    const int dy = e.y - intent.mouseDownPos.y;
    if (dx * dx + dy * dy <= kEmptySpaceDragThreshold * kEmptySpaceDragThreshold) {
        return true;
    }

    const auto startEvent = eventAtEmptySpaceMouseDown(e);
    const ToolId tool = intent.tool;
    intent.clear();

    switch (tool) {
        case ToolId::Select:
            handleSelectTool(startEvent);
            handleSelectDrag(e);
            return true;
        case ToolId::DrawNote:
            handleDrawNoteMouseDown(startEvent);
            handleDrawNoteDrag(e);
            return true;
        case ToolId::HandDraw:
            handleDrawCurveTool(startEvent);
            handleDrawCurveTool(e);
            return true;
        default:
            return true;
    }
}

bool PianoRollToolHandler::consumeEmptySpaceIntentUp(const juce::MouseEvent& e)
{
    auto& intent = ctx_.getState().emptySpaceIntent;
    if (!intent.active) {
        return false;
    }

    const int dx = e.x - intent.mouseDownPos.x;
    const int dy = e.y - intent.mouseDownPos.y;
    if (dx * dx + dy * dy > kEmptySpaceDragThreshold * kEmptySpaceDragThreshold) {
        const ToolId tool = intent.tool;
        if (!consumeEmptySpaceIntentDrag(e)) {
            return true;
        }
        switch (tool) {
            case ToolId::Select:
                handleSelectUp(e);
                break;
            case ToolId::HandDraw:
                handleDrawCurveUp(e);
                break;
            case ToolId::DrawNote:
                handleDrawNoteUp(e);
                break;
            default:
                break;
        }
        return true;
    }

    if (intent.mouseDownTime >= 0.0) {
        ctx_.notifyPlayheadChange(intent.mouseDownTime);
    }

    intent.clear();
    return true;
}

juce::MouseEvent PianoRollToolHandler::eventAtEmptySpaceMouseDown(const juce::MouseEvent& e)
{
    return e.withNewPosition(ctx_.getState().emptySpaceIntent.mouseDownPos.toFloat());
}

void PianoRollToolHandler::cancelActiveMouseGesture()
{
    auto& state = ctx_.getState();
    state.emptySpaceIntent.clear();
    state.drawNoteToolPendingDrag = false;
    state.handDrawPendingDrag = false;
    state.noteDrag.clear();
    state.noteResize.clear();
    state.selection.isSelectingArea = false;
    state.drawing.isDrawingF0 = false;
    state.drawing.handDrawBuffer.clear();
    state.drawing.isDrawingNote = false;
    state.drawing.isPlacingAnchors = false;
    state.drawing.pendingAnchors.clear();
    ctx_.clearNoteDraft();
}

void PianoRollToolHandler::handleDeleteKey()
// 删除键处理：删除选中的音符和选区内的内容，同时清除对应的音高修正
{
    const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));
    ctx_.beginNoteDraft();
    auto& notes = workingDraftNotes(ctx_);
    const auto selectedIndices = collectSelectedNoteIndices(notes);

    int globalDirtyStartFrame = INT_MAX;
    int globalDirtyEndFrame = INT_MIN;

    double deleteStartTime = 1e30;
    double deleteEndTime = -1e30;

    if (ctx_.getState().selection.hasSelectionArea) {
        deleteStartTime = std::min(ctx_.getState().selection.selectionStartTime, ctx_.getState().selection.selectionEndTime);
        deleteEndTime = std::max(ctx_.getState().selection.selectionStartTime, ctx_.getState().selection.selectionEndTime);
    }

    for (int noteIndex : selectedIndices) {
        const auto& note = notes[static_cast<size_t>(noteIndex)];
        deleteStartTime = std::min(deleteStartTime, note.startTime);
        deleteEndTime = std::max(deleteEndTime, note.endTime);
    }

    if (deleteEndTime <= deleteStartTime && selectedIndices.empty() && !ctx_.getState().selection.hasSelectionArea) {
        ctx_.clearNoteDraft();
        return;
    }

    int deleteStartFrame = -1;
    int deleteEndFrameExclusive = -1;
    auto curve = ctx_.getPitchCurve();
    const auto f0tl = ctx_.getF0Timeline();

    if (deleteEndTime > deleteStartTime && curve && !curve->isEmpty()) {
        const auto deleteRange = f0tl.rangeForTimes(deleteStartTime, deleteEndTime);
        deleteStartFrame = deleteRange.startFrame;
        deleteEndFrameExclusive = deleteRange.endFrameExclusive;
    }

    const bool willDeleteSelectionArea =
        ctx_.getState().selection.hasSelectionArea && deleteStartFrame >= 0 && deleteEndFrameExclusive > deleteStartFrame;
    if (selectedIndices.empty() && !willDeleteSelectionArea) {
        return;
    }

    bool handled = false;
    // 本地累积需要清除的修正范围，不立即提交，最后一次性与音符原子提交
    std::vector<F0FrameRange> correctionClearRanges;

    if (!selectedIndices.empty()) {
        double deletedNotesStartTime = 1e30;
        double deletedNotesEndTime = -1e30;
        for (int noteIndex : selectedIndices) {
            const auto& note = notes[static_cast<size_t>(noteIndex)];
            deletedNotesStartTime = std::min(deletedNotesStartTime, note.startTime);
            deletedNotesEndTime = std::max(deletedNotesEndTime, note.endTime);
        }

        if (curve && deletedNotesEndTime > deletedNotesStartTime) {
            const auto noteRange = f0tl.rangeForTimes(deletedNotesStartTime, deletedNotesEndTime);
            if (!noteRange.isEmpty()) {
                correctionClearRanges.push_back(noteRange);
                globalDirtyStartFrame = std::min(globalDirtyStartFrame, noteRange.startFrame);
                globalDirtyEndFrame = std::max(globalDirtyEndFrame, noteRange.endFrameExclusive - 1);
            }
        }

        deleteSelectedNotes(notes);
        handled = true;
    }

    if (willDeleteSelectionArea) {
        double startTime = std::min(ctx_.getState().selection.selectionStartTime, ctx_.getState().selection.selectionEndTime);
        double endTime = std::max(ctx_.getState().selection.selectionStartTime, ctx_.getState().selection.selectionEndTime);

        notes.erase(
            std::remove_if(notes.begin(), notes.end(),
                [startTime, endTime](const Note& n) {
                    return n.endTime > startTime && n.startTime < endTime;
                }),
            notes.end()
        );

        if (curve) {
            F0FrameRange selRange;
            selRange.startFrame = deleteStartFrame;
            selRange.endFrameExclusive = deleteEndFrameExclusive;
            correctionClearRanges.push_back(selRange);
        }

        globalDirtyStartFrame = std::min(globalDirtyStartFrame, deleteStartFrame);
        globalDirtyEndFrame = std::max(globalDirtyEndFrame, deleteEndFrameExclusive - 1);
        ctx_.getState().selection.hasSelectionArea = false;
        handled = true;
    }

    if (handled) {
        ctx_.getNoteDraft().workingNotes = notes;
        ctx_.setUndoDescription(juce::String("删除音符"));

        // 同步计算清除修正 + 删除音符 → 一次性原子提交
        bool committed = false;
        if (curve && !correctionClearRanges.empty()) {
            auto clonedCurve = curve->clone();
            for (const auto& range : correctionClearRanges) {
                clonedCurve->clearCorrectionRange(range.startFrame, range.endFrameExclusive);
            }
            auto snap = clonedCurve->getSnapshot();
            // delete 路径：affectedRange = globalDirty*Frame 的覆盖范围（含端点），
            // 转 F0FrameRange 的 endFrameExclusive 语义。
            const F0FrameRange affectedRange{globalDirtyStartFrame, globalDirtyEndFrame + 1};
            committed = ctx_.commitNotesAndSegments(notes, snap->getCorrectedSegments(), affectedRange);
            if (committed) {
                ctx_.notifyPitchCurveEdited(globalDirtyStartFrame, globalDirtyEndFrame);
            }
        } else {
            committed = ctx_.commitNoteDraft();
        }

        if (committed) {
            invalidateNoteChange(ctx_, beforeNotes, committedNotes(ctx_));
        }
        return;
    }

    ctx_.clearNoteDraft();
}

void PianoRollToolHandler::handleSelectTool(const juce::MouseEvent& e)
// 选择工具鼠标按下处理：检测音符边缘调整、音符选中/取消选中、框选区域开始
{
    const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));
    ctx_.beginNoteDraft();
    auto& notes = workingDraftNotes(ctx_);

    ctx_.getState().noteResize.isResizing = false;
    ctx_.getState().noteResize.noteIndex = -1;
    ctx_.getState().noteResize.edge = NoteResizeEdge::None;

    const auto projection = ctx_.getMaterializationProjection();
    // §8.5 — pixelX → SOURCE time (Note tool writes startTime in source time).
    double trackRelativeTime = pixelXToSourceTime(e.x);
    if (projection.isValid()) {
        trackRelativeTime = projection.clampMaterializationTime(trackRelativeTime);
    }

    float clickedPitch = ctx_.yToFreq((float)e.y);

    const int clickedNoteIndex = trackRelativeTime >= 0
        ? findNoteIndexAt(notes, trackRelativeTime, clickedPitch, 100.0f)
        : -1;

    bool isCtrlDown = e.mods.isCtrlDown() || e.mods.isCommandDown();

    int edgeThreshold = 6;
    float mouseMidi = 69.0f + 12.0f * std::log2(clickedPitch / 440.0f) - 0.5f;
    bool isShiftDown = e.mods.isShiftDown();

    for (int noteIndex = 0; noteIndex < static_cast<int>(notes.size()); ++noteIndex) {
        auto& note = notes[static_cast<size_t>(noteIndex)];
        int x1 = ctx_.timeToX(ctx_.projectMaterializationTimeToTimeline(note.startTime));
        int x2 = ctx_.timeToX(ctx_.projectMaterializationTimeToTimeline(note.endTime));

        bool nearLeft = std::abs(e.x - x1) <= edgeThreshold;
        bool nearRight = std::abs(e.x - x2) <= edgeThreshold;

        if (nearLeft || nearRight) {
            float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f) - 0.5f;
            if (std::abs(mouseMidi - noteMidi) < 1.0f) {
                ctx_.getState().noteResize.isResizing = true;
                ctx_.getState().noteResize.isDirty = false;
                ctx_.getState().noteResize.noteIndex = noteIndex;
                ctx_.getState().noteResize.edge = nearLeft ? NoteResizeEdge::Left : NoteResizeEdge::Right;
                ctx_.getState().noteResize.originalStartTime = note.startTime;
                ctx_.getState().noteResize.originalEndTime = note.endTime;

                if (!note.selected && !isCtrlDown && !isShiftDown) {
                    deselectAllNotes(notes);
                }
                note.selected = true;

                updateF0SelectionFromNotes(notes);
                invalidateNoteChange(ctx_, beforeNotes, notes);
                return;
            }
        }
    }

    if (clickedNoteIndex >= 0) {
        if (isCtrlDown) {
            notes[static_cast<size_t>(clickedNoteIndex)].selected = !notes[static_cast<size_t>(clickedNoteIndex)].selected;
        } else if (isShiftDown) {
            int lastSelectedIndex = findLastSelectedNoteIndex(notes);
            if (lastSelectedIndex >= 0 && lastSelectedIndex != clickedNoteIndex) {
                selectNotesBetween(notes, lastSelectedIndex, clickedNoteIndex);
            } else {
                notes[static_cast<size_t>(clickedNoteIndex)].selected = true;
            }
        } else if (!notes[static_cast<size_t>(clickedNoteIndex)].selected) {
            deselectAllNotes(notes);
            notes[static_cast<size_t>(clickedNoteIndex)].selected = true;
        }

        updateF0SelectionFromNotes(notes);

        if (notes[static_cast<size_t>(clickedNoteIndex)].selected) {
            ctx_.getState().noteDrag.draggedNoteIndex = clickedNoteIndex;
            ctx_.getState().noteDrag.draggedNoteIndices = collectSelectedNoteIndices(notes);
            clearNoteDragPreview(ctx_);

            ctx_.setNoteDragManualStartTime(-1.0);
            ctx_.setNoteDragManualEndTime(-1.0);
            ctx_.getNoteDragInitialManualTargets().clear();

            auto pitchCurve = ctx_.getPitchCurve();
            if (pitchCurve && !ctx_.getState().noteDrag.draggedNoteIndices.empty()) {
                double rangeStart = 0;
                double rangeEnd = 0;

                if (ctx_.getState().selection.hasSelectionArea) {
                    rangeStart = std::min(ctx_.getState().selection.selectionStartTime, ctx_.getState().selection.selectionEndTime);
                    rangeEnd = std::max(ctx_.getState().selection.selectionStartTime, ctx_.getState().selection.selectionEndTime);
                } else {
                    rangeStart = 1e30;
                    rangeEnd = -1e30;
                    for (int selectedIndex : ctx_.getState().noteDrag.draggedNoteIndices) {
                        const auto& selectedNote = notes[static_cast<size_t>(selectedIndex)];
                        rangeStart = std::min(rangeStart, selectedNote.startTime);
                        rangeEnd = std::max(rangeEnd, selectedNote.endTime);
                    }
                }

                if (rangeStart < 0) rangeStart = 0;
                
                if (rangeEnd < rangeStart) std::swap(rangeStart, rangeEnd);

                auto originalF0 = ctx_.getOriginalF0();
                const auto f0tl = ctx_.getF0Timeline();
                const auto manualRange = f0tl.rangeForTimes(rangeStart, rangeEnd);

                if (!manualRange.isEmpty() && pitchCurve->hasCorrectionInRange(manualRange.startFrame, manualRange.endFrameExclusive)) {
                        int rangeSize = manualRange.endFrameExclusive - manualRange.startFrame;
                        std::vector<float> renderedF0(rangeSize, -1.0f);

                        pitchCurve->renderF0Range(manualRange.startFrame, manualRange.endFrameExclusive,
                            [&](int frameIndex, const float* data, int length) {
                                if (data == nullptr || length <= 0) return;

                                int relStart = frameIndex - manualRange.startFrame;
                                int copyOffset = 0;
                                if (relStart < 0) {
                                    copyOffset = -relStart;
                                    relStart = 0;
                                }

                                if (relStart >= rangeSize || copyOffset >= length) return;

                                int copyLength = std::min(length - copyOffset, rangeSize - relStart);
                                if (copyLength <= 0) return;

                                std::copy_n(data + copyOffset, copyLength, renderedF0.begin() + relStart);
                            });

                        ctx_.setNoteDragManualStartTime(f0tl.timeAtFrame(manualRange.startFrame));
                        ctx_.setNoteDragManualEndTime(f0tl.timeAtFrame(manualRange.endFrameExclusive));

                        auto& manualTargets = ctx_.getNoteDragInitialManualTargets();
                        for (int relIdx = 0; relIdx < rangeSize; ++relIdx) {
                            int f = manualRange.startFrame + relIdx;
                            float v = renderedF0[relIdx];
                            if (v <= 0.0f) continue;
                            manualTargets.push_back({ f0tl.timeAtFrame(f), v });
                        }

                        if (manualTargets.empty()) {
                            ctx_.setNoteDragManualStartTime(-1.0);
                            ctx_.setNoteDragManualEndTime(-1.0);
                        }
                    }
                }
        } else {
            // Note was not selected - start selection area or clear
            if (!isCtrlDown) {
                ctx_.getState().noteDrag.draggedNoteIndex = -1;
                ctx_.getState().noteDrag.draggedNoteIndices.clear();
                clearNoteDragPreview(ctx_);
            }
        }

        invalidateNoteChange(ctx_, beforeNotes, notes);
    } else {
        if (!isCtrlDown) {
            deselectAllNotes(notes);
            updateF0SelectionFromNotes(notes);
            ctx_.getState().noteDrag.draggedNoteIndex = -1;
            ctx_.getState().noteDrag.draggedNoteIndices.clear();
            ctx_.getState().noteResize.isResizing = false;
            ctx_.getState().noteResize.noteIndex = -1;
            ctx_.getState().noteResize.edge = NoteResizeEdge::None;
            if (e.x > ctx_.getPianoKeyWidth()) {
                ctx_.getState().selection.isSelectingArea = true;
                ctx_.getState().selection.hasSelectionArea = true;
                ctx_.getState().selection.selectionStartTime = std::max(0.0, trackRelativeTime);
                ctx_.getState().selection.selectionEndTime = ctx_.getState().selection.selectionStartTime;
                float midiVal = 69.0f + 12.0f * std::log2(clickedPitch / 440.0f) - 0.5f;
                ctx_.getState().selection.selectionStartMidi = midiVal;
                ctx_.getState().selection.selectionEndMidi = midiVal;
            } else {
                ctx_.getState().selection.isSelectingArea = false;
                ctx_.getState().selection.hasSelectionArea = false;
            }
            invalidateNoteChange(ctx_, beforeNotes, notes);
        } else {
            ctx_.clearNoteDraft();
        }
    }
}

void PianoRollToolHandler::handleDrawCurveTool(const juce::MouseEvent& e)
// 手绘曲线工具处理：将鼠标位置转换为F0值，在帧间进行对数插值，记录脏区域
{
    auto pitchCurve = ctx_.getPitchCurve();
    if (!pitchCurve) {
        return;
    }

    const auto dirtyBefore = ctx_.getHandDrawPreviewBounds();

    const auto projection = ctx_.getMaterializationProjection();
    // §8.5 — F0 hand-draw writes into PitchCurve which is indexed by SOURCE time.
    double curveTime = pixelXToSourceTime(e.x);
    if (projection.isValid()) {
        curveTime = projection.clampMaterializationTime(curveTime);
    }

    float targetF0 = ctx_.yToFreq((float)e.y);

    const auto& originalF0 = ctx_.getOriginalF0();
    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) return;
    int frameIndex = f0tl.frameAtOrBefore(curveTime);

    const auto scheme = ctx_.getAudioEditingScheme();

    if (!ctx_.getState().drawing.isDrawingF0) {
        ctx_.getState().drawing.isDrawingF0 = true;
        ctx_.setDirtyStartTime(-1.0);
        ctx_.setDirtyEndTime(-1.0);
        lastDrawPoint_ = juce::Point<float>(static_cast<float>(curveTime), targetF0);

        auto& handDrawBuffer = ctx_.getState().drawing.handDrawBuffer;
        handDrawBuffer.clear();
        handDrawBuffer.resize(originalF0.size(), -1.0f);
        
    }

    auto& handDrawBuffer = ctx_.getState().drawing.handDrawBuffer;
    
    double lastTime = static_cast<double>(lastDrawPoint_.x);

    auto writeFrame = [&](int f, float v) -> void {
        if (f < 0 || static_cast<size_t>(f) >= originalF0.size()) {
            return;
        }
        if (!AudioEditingScheme::canEditFrame(scheme, originalF0, f)) {
            return;
        }
        handDrawBuffer[(size_t)f] = v;
        double frameTime = f0tl.timeAtFrame(f);
        double dirtyStart = ctx_.getDirtyStartTime();
        double dirtyEnd = ctx_.getDirtyEndTime();
        dirtyStart = (dirtyStart < 0.0) ? frameTime : std::min(dirtyStart, frameTime);
        dirtyEnd = (dirtyEnd < 0.0) ? frameTime : std::max(dirtyEnd, frameTime);
        ctx_.setDirtyStartTime(dirtyStart);
        ctx_.setDirtyEndTime(dirtyEnd);
    };

    int lastFrame = f0tl.frameAtOrBefore(lastTime);
    float lastF0 = lastDrawPoint_.y;
    writeFrame(frameIndex, targetF0);

    int startFrame = std::min(lastFrame, frameIndex);
    int endFrame = std::max(lastFrame, frameIndex);

    if (endFrame > startFrame && lastF0 > 0.0f && targetF0 > 0.0f) {
        float logA = std::log2(lastF0);
        float logB = std::log2(targetF0);
        for (int f = startFrame + 1; f < endFrame; ++f) {
            float t = static_cast<float>(f - startFrame) / static_cast<float>(endFrame - startFrame);
            float logV = logA + (logB - logA) * t;
            float v = std::pow(2.0f, logV);
            writeFrame(f, v);
        }
    }

    lastDrawPoint_ = juce::Point<float>(static_cast<float>(curveTime), targetF0);
    invalidateIfNeeded(ctx_, dirtyBefore.getUnion(ctx_.getHandDrawPreviewBounds()));
}

void PianoRollToolHandler::handleDrawNoteMouseDown(const juce::MouseEvent& e)
// 绘制音符工具鼠标按下处理：检测是否点击已有音符进行选择，设置待拖拽状态
{
    const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));
    ctx_.beginNoteDraft();
    auto& notes = workingDraftNotes(ctx_);
    
    int existingNoteIndex = -1;
    float clickedPitch = ctx_.yToFreq((float)e.y);
    float mouseMidi = 69.0f + 12.0f * std::log2(clickedPitch / 440.0f) - 0.5f;
    
    for (int noteIndex = 0; noteIndex < static_cast<int>(notes.size()); ++noteIndex) {
        const auto& note = notes[static_cast<size_t>(noteIndex)];
        int x1 = ctx_.timeToX(ctx_.projectMaterializationTimeToTimeline(note.startTime));
        int x2 = ctx_.timeToX(ctx_.projectMaterializationTimeToTimeline(note.endTime));
        float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f) - 0.5f;
        
        if (e.x >= x1 && e.x <= x2 && std::abs(mouseMidi - noteMidi) < 1.0f) {
            existingNoteIndex = noteIndex;
            break;
        }
    }
    
    if (existingNoteIndex >= 0) {
        bool isCtrlDown = e.mods.isCtrlDown() || e.mods.isCommandDown();
        if (isCtrlDown) {
            notes[static_cast<size_t>(existingNoteIndex)].selected = !notes[static_cast<size_t>(existingNoteIndex)].selected;
        } else {
            if (!notes[static_cast<size_t>(existingNoteIndex)].selected) {
                deselectAllNotes(notes);
                notes[static_cast<size_t>(existingNoteIndex)].selected = true;
            }
        }
        invalidateNoteChange(ctx_, beforeNotes, notes);
    }
    
    ctx_.setDrawNoteToolPendingDrag(true);
    ctx_.setDrawNoteToolMouseDownPos(e.getPosition());
}

void PianoRollToolHandler::handleDrawNoteTool(const juce::MouseEvent& e)
// 绘制音符工具处理：创建新音符或更新正在绘制的音符，音高自动对齐到半音
{
    const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));
    if (!ctx_.getNoteDraft().active) {
        return;
    }

    auto& notes = workingDraftNotes(ctx_);
    const auto projection = ctx_.getMaterializationProjection();
    // §8.5 — Note drag writes startTime/endTime in SOURCE time.
    double currentTime = pixelXToSourceTime(e.x);
    if (currentTime < 0) {
        currentTime = 0;
    }
    if (projection.isValid()) {
        currentTime = projection.clampMaterializationTime(currentTime);
    }

    float targetF0 = ctx_.yToFreq((float)e.y);
    float midiNote = 69.0f + 12.0f * std::log2(targetF0 / 440.0f);
    int roundedMidi = static_cast<int>(std::round(midiNote));
    float snappedF0 = 440.0f * std::pow(2.0f, (roundedMidi - 69) / 12.0f);

    if (!ctx_.getState().drawing.isDrawingNote) {
        ctx_.getState().drawing.isDrawingNote = true;
        ctx_.setDrawingNoteStartTime(currentTime);
        ctx_.setDrawingNoteEndTime(currentTime);
        ctx_.setDrawingNotePitch(snappedF0);

        Note newNote;
        newNote.startTime = currentTime;
        newNote.endTime = currentTime;
        newNote.pitch = snappedF0;
        newNote.originalPitch = snappedF0;
        newNote.pitchOffset = 0.0f;
        newNote.selected = false;
        newNote.dirty = true;

        auto insertPos = std::lower_bound(notes.begin(), notes.end(), currentTime,
            [](const Note& n, double t) { return n.startTime < t; });
        int insertIdx = static_cast<int>(std::distance(notes.begin(), insertPos));
        notes.insert(insertPos, newNote);
        ctx_.setDrawingNoteIndex(insertIdx);
        invalidateNoteChange(ctx_, beforeNotes, notes);
        return;
    }

    if (ctx_.getDrawingNoteIndex() >= 0) {
        ctx_.setDrawingNoteEndTime(currentTime);
        int idx = ctx_.getDrawingNoteIndex();
        if (idx < static_cast<int>(notes.size())) {
            double startTime = ctx_.getDrawingNoteStartTime();
            double endTime = ctx_.getDrawingNoteEndTime();
            notes[(size_t)idx].startTime = std::min(startTime, endTime);
            notes[(size_t)idx].endTime = std::max(startTime, endTime);
            notes[(size_t)idx].dirty = true;
        }
    }

    invalidateNoteChange(ctx_, beforeNotes, notes);
}

void PianoRollToolHandler::handleAutoTuneTool(const juce::MouseEvent& e)
// 自动音调工具处理：触发自动音调生成请求
{
    juce::ignoreUnused(e);
    ctx_.notifyAutoTuneRequested();
}

void PianoRollToolHandler::handleSelectDrag(const juce::MouseEvent& e)
// 选择工具拖拽处理：框选区域、音符边缘调整、音符拖拽移动
{
    const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));

    if (ctx_.getState().selection.isSelectingArea) {
        // §8.5 — selection box bounds compared against note.startTime (source time).
        double currentTime = pixelXToSourceTime(e.x);
        const auto projection = ctx_.getMaterializationProjection();
        if (projection.isValid()) {
            currentTime = projection.clampMaterializationTime(currentTime);
        }
        ctx_.getState().selection.selectionEndTime = std::max(0.0, currentTime);

        float currentMidi = 69.0f + 12.0f * std::log2(ctx_.yToFreq((float)e.y) / 440.0f) - 0.5f;
        ctx_.getState().selection.selectionEndMidi = currentMidi;

        double selStartTime = std::min(ctx_.getState().selection.selectionStartTime, ctx_.getState().selection.selectionEndTime);
        double selEndTime = std::max(ctx_.getState().selection.selectionStartTime, ctx_.getState().selection.selectionEndTime);
        float selMinMidi = std::min(ctx_.getState().selection.selectionStartMidi, ctx_.getState().selection.selectionEndMidi);
        float selMaxMidi = std::max(ctx_.getState().selection.selectionStartMidi, ctx_.getState().selection.selectionEndMidi);

        auto& notes = workingDraftNotes(ctx_);
        for (auto& note : notes) {
            float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f) - 0.5f;
            bool timeOverlap = (note.endTime > selStartTime && note.startTime < selEndTime);
            bool pitchOverlap = (noteMidi >= selMinMidi - 0.5f && noteMidi <= selMaxMidi + 0.5f);
            note.selected = timeOverlap && pitchOverlap;
        }
        invalidateNoteChange(ctx_, beforeNotes, notes);
        return;
    }

    if (ctx_.getState().noteResize.isResizing && ctx_.getState().noteResize.noteIndex >= 0) {
        if (!ctx_.getState().noteResize.isDirty) {
            ctx_.getState().noteResize.isDirty = true;
        }

        auto& notes = workingDraftNotes(ctx_);
        resetDraftNotesToBaseline(ctx_);
        if (ctx_.getState().noteResize.noteIndex >= static_cast<int>(notes.size())) {
            return;
        }

        // §8.5 — Note resize edge writes startTime/endTime in SOURCE time.
        double currentTime = pixelXToSourceTime(e.x);
        const auto projection = ctx_.getMaterializationProjection();
        if (projection.isValid()) {
            currentTime = projection.clampMaterializationTime(currentTime);
        }
        currentTime = std::max(0.0, currentTime);
        double minDuration = 0.02;

        if (ctx_.getState().noteResize.edge == NoteResizeEdge::Left) {
            double newStart = std::min(currentTime, notes[static_cast<size_t>(ctx_.getState().noteResize.noteIndex)].endTime - minDuration);
            newStart = std::max(0.0, newStart);
            notes[static_cast<size_t>(ctx_.getState().noteResize.noteIndex)].startTime = newStart;
        } else if (ctx_.getState().noteResize.edge == NoteResizeEdge::Right) {
            double newEnd = std::max(currentTime, notes[static_cast<size_t>(ctx_.getState().noteResize.noteIndex)].startTime + minDuration);
            notes[static_cast<size_t>(ctx_.getState().noteResize.noteIndex)].endTime = newEnd;
        }

        notes[static_cast<size_t>(ctx_.getState().noteResize.noteIndex)].dirty = true;
        invalidateNoteChange(ctx_, beforeNotes, notes);
        return;
    }

    if (ctx_.getState().noteDrag.draggedNoteIndex >= 0) {
        if (ctx_.getState().noteDrag.draggedNoteIndices.empty()) {
            return;
        }

        if (!ctx_.getState().noteDrag.isDraggingNotes) {
            ctx_.getState().noteDrag.isDraggingNotes = true;
        }

        float startF0 = ctx_.yToFreq((float)dragStartPos_.y);
        float currentF0 = ctx_.yToFreq((float)e.y);

        float deltaSemitones = 0.0f;
        if (startF0 > 0.0f && currentF0 > 0.0f) {
            deltaSemitones = 12.0f * std::log2(currentF0 / startF0);
        }

        auto& notes = workingDraftNotes(ctx_);
        resetDraftNotesToBaseline(ctx_);
        for (int noteIndex : ctx_.getState().noteDrag.draggedNoteIndices) {
            auto& note = notes[static_cast<size_t>(noteIndex)];
            float initialOffset = draftBaselineNotes(ctx_)[static_cast<size_t>(noteIndex)].pitchOffset;
            int baseMidi = note.getBaseMidiNote();
            float targetMidi = static_cast<float>(baseMidi) + initialOffset + deltaSemitones;
            float snappedOffset = std::round(targetMidi) - static_cast<float>(baseMidi);
            note.pitchOffset = snappedOffset;
            note.dirty = true;
        }

        float appliedDeltaSemitones = 0.0f;
        if (!ctx_.getState().noteDrag.draggedNoteIndices.empty()) {
            const int firstIndex = ctx_.getState().noteDrag.draggedNoteIndices.front();
            appliedDeltaSemitones = notes[static_cast<size_t>(firstIndex)].pitchOffset
                - draftBaselineNotes(ctx_)[static_cast<size_t>(firstIndex)].pitchOffset;
        }

        updateNoteDragPreview(ctx_, std::pow(2.0f, appliedDeltaSemitones / 12.0f));
        invalidateNoteChange(ctx_, beforeNotes, notes);
        return;
    }

    const auto& notes = displayNotes(ctx_);
    const auto selected = collectSelectedNoteIndices(notes);
    if (!selected.empty()) {
        ctx_.getState().noteDrag.draggedNoteIndex = selected.front();
        ctx_.getState().noteDrag.draggedNoteIndices = selected;
        ctx_.getState().noteDrag.isDraggingNotes = true;
    }
}

void PianoRollToolHandler::handleDrawNoteDrag(const juce::MouseEvent& e)
{
    if (ctx_.getDrawNoteToolPendingDrag()) {
        int dx = e.x - ctx_.getDrawNoteToolMouseDownPos().x;
        int dy = e.y - ctx_.getDrawNoteToolMouseDownPos().y;
        int threshold = ctx_.getDragThreshold();
        if (dx * dx + dy * dy > threshold * threshold) {
            ctx_.setDrawNoteToolPendingDrag(false);
            juce::Point<int> downPos = ctx_.getDrawNoteToolMouseDownPos();
            juce::MouseEvent startEvent = e.withNewPosition(downPos.toFloat());
            handleDrawNoteTool(startEvent);
        }
    } else if (ctx_.getState().drawing.isDrawingNote) {
        handleDrawNoteTool(e);
    }
}

void PianoRollToolHandler::handleSelectUp(const juce::MouseEvent& e)
// 选择工具鼠标释放处理：完成音符拖拽/调整/框选，提交音高修正
{
    juce::ignoreUnused(e);

    const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));
    bool queuedAsyncCommit = false;
    bool suppressFinalNoteDraftCommit = false;

    auto notes = beforeNotes;
    if (ctx_.getNoteDraft().active) {
        notes = ctx_.getNoteDraft().workingNotes;
    }
    const auto f0tl = ctx_.getF0Timeline();

    if (ctx_.getState().noteDrag.draggedNoteIndex >= 0 && ctx_.getState().noteDrag.isDraggingNotes) {
        double dirtyStartTime = 1e30;
        double dirtyEndTime = -1e30;

        for (int noteIndex : ctx_.getState().noteDrag.draggedNoteIndices) {
            float initialOffset = draftBaselineNotes(ctx_)[static_cast<size_t>(noteIndex)].pitchOffset;
            float finalOffset = notes[static_cast<size_t>(noteIndex)].pitchOffset;

            if (std::abs(finalOffset - initialOffset) > 0.001f) {
                dirtyStartTime = std::min(dirtyStartTime, notes[static_cast<size_t>(noteIndex)].startTime);
                dirtyEndTime = std::max(dirtyEndTime, notes[static_cast<size_t>(noteIndex)].endTime);
                ctx_.notifyNoteOffsetChanged(static_cast<size_t>(noteIndex), initialOffset, finalOffset);
                float newPip = ctx_.recalculatePIP(notes[static_cast<size_t>(noteIndex)]);
                if (newPip > 0.0f) {
                    notes[static_cast<size_t>(noteIndex)].originalPitch = newPip;
                }
            }
        }

        const bool hasManualTargets = hasNoteDragPreview(ctx_);
        if (dirtyEndTime > dirtyStartTime || hasManualTargets) {
            auto pitchCurve = ctx_.getPitchCurve();
            double rangeStartTime = hasManualTargets ? ctx_.getNoteDragManualStartTime() : dirtyStartTime;
            double rangeEndTime = hasManualTargets ? ctx_.getNoteDragManualEndTime() : dirtyEndTime;
            if (dirtyEndTime > dirtyStartTime && hasManualTargets) {
                rangeStartTime = std::min(rangeStartTime, dirtyStartTime);
                rangeEndTime = std::max(rangeEndTime, dirtyEndTime);
            }

            F0FrameRange editRange;
            if (pitchCurve) {
                editRange = f0tl.rangeForTimes(rangeStartTime, rangeEndTime);
            }

            if (hasManualTargets) {
                suppressFinalNoteDraftCommit = true;

                std::vector<ManualOp> ops;
                ManualOp op;
                op.startFrame = ctx_.getNoteDragPreviewStartFrame();
                op.endFrameExclusive = ctx_.getNoteDragPreviewEndFrameExclusive();
                op.f0Data = ctx_.getNoteDragPreviewF0();
                op.source = CorrectedSegment::Source::HandDraw;
                ops.push_back(std::move(op));

                if (pitchCurve != nullptr && ctx_.commitNotesAndSegments) {
                    const auto updatedSegments = buildSegmentsWithManualOps(pitchCurve, ops);
                    ctx_.setUndoDescription(juce::String("移动音符"));
                    if (ctx_.commitNotesAndSegments(notes, updatedSegments, editRange)) {
                        ctx_.notifyPitchCurveEdited(editRange.startFrame,
                                                    editRange.endFrameExclusive - 1);
                    }
                }
            } else {
                ctx_.getNoteDraft().workingNotes = notes;
                ctx_.setUndoDescription(juce::String("移动音符"));

                // 同步计算修正并一次性提交音符和F0段
                if (pitchCurve && !editRange.isEmpty()) {
                    commitNoteBasedCorrection(ctx_, notes, pitchCurve, editRange);
                } else {
                    ctx_.commitNoteDraft();
                }
                suppressFinalNoteDraftCommit = true;
            }
        }

        ctx_.getState().noteDrag.draggedNoteIndices.clear();
        ctx_.setNoteDragManualStartTime(-1.0);
        ctx_.setNoteDragManualEndTime(-1.0);
        ctx_.getNoteDragInitialManualTargets().clear();
        clearNoteDragPreview(ctx_);
    }

    bool resizeWasDirty = false;
    double resizedStartTime = 0.0;
    double resizedEndTime = 0.0;
    if (ctx_.getState().noteResize.isResizing
        && ctx_.getState().noteResize.noteIndex >= 0
        && ctx_.getState().noteResize.isDirty
        && ctx_.getState().noteResize.noteIndex < static_cast<int>(notes.size())) {
        resizeWasDirty = notes[static_cast<size_t>(ctx_.getState().noteResize.noteIndex)].dirty;
        resizedStartTime = notes[static_cast<size_t>(ctx_.getState().noteResize.noteIndex)].startTime;
        resizedEndTime = notes[static_cast<size_t>(ctx_.getState().noteResize.noteIndex)].endTime;
    }

    notes.erase(
        std::remove_if(notes.begin(), notes.end(), [](const Note& n) {
            return n.startTime >= n.endTime;
        }),
        notes.end());

    if (ctx_.getState().noteResize.isResizing && resizeWasDirty) {
        double dirtyStartTime = std::min(ctx_.getState().noteResize.originalStartTime, resizedStartTime);
        double dirtyEndTime = std::max(ctx_.getState().noteResize.originalEndTime, resizedEndTime);
        auto pitchCurve = ctx_.getPitchCurve();
        F0FrameRange editRange;
        if (pitchCurve) {
            editRange = f0tl.rangeForTimes(dirtyStartTime, dirtyEndTime);
        }

        ctx_.getNoteDraft().workingNotes = notes;
        ctx_.setUndoDescription(juce::String("调整音符长度"));

        // 同步计算修正并一次性提交音符和F0段
        if (pitchCurve && !editRange.isEmpty()) {
            commitNoteBasedCorrection(ctx_, notes, pitchCurve, editRange);
        } else {
            ctx_.commitNoteDraft();
        }
        queuedAsyncCommit = true;
    }

    if (!queuedAsyncCommit) {
        if (ctx_.getNoteDraft().active && !suppressFinalNoteDraftCommit) {
            ctx_.getNoteDraft().workingNotes = notes;
            ctx_.setUndoDescription(juce::String("编辑音符"));
            ctx_.commitNoteDraft();
        }
    }

    ctx_.getState().noteResize.isResizing = false;
    ctx_.getState().noteResize.isDirty = false;
    ctx_.getState().noteResize.noteIndex = -1;
    ctx_.getState().noteResize.edge = NoteResizeEdge::None;

    if (ctx_.getState().selection.isSelectingArea) {
        ctx_.getState().selection.isSelectingArea = false;
        double timeDelta = std::abs(ctx_.getState().selection.selectionEndTime - ctx_.getState().selection.selectionStartTime);
        float midiDelta = std::abs(ctx_.getState().selection.selectionEndMidi - ctx_.getState().selection.selectionStartMidi);
        if (timeDelta < 0.01 || midiDelta < 0.5f) {
            ctx_.getState().selection.hasSelectionArea = false;
        }
        updateF0SelectionFromNotes(notes);
    }

    ctx_.getState().noteDrag.draggedNoteIndex = -1;
    ctx_.clearNoteDraft();
    invalidateNoteChange(ctx_, beforeNotes, committedNotes(ctx_));
}

void PianoRollToolHandler::handleDrawCurveUp(const juce::MouseEvent& e)
// 手绘曲线工具鼠标释放处理：将绘制的F0数据提交到音高修正队列
{
    juce::ignoreUnused(e);
    const auto dirtyBefore = ctx_.getHandDrawPreviewBounds();

    if (ctx_.getState().handDrawPendingDrag) {
        ctx_.getState().handDrawPendingDrag = false;
        return;
    }

    if (!ctx_.getState().drawing.isDrawingF0) {
        return;
    }
    
    auto pitchCurve = ctx_.getPitchCurve();
    auto& handDrawBuffer = ctx_.getState().drawing.handDrawBuffer;
    if (pitchCurve && ctx_.getDirtyStartTime() >= 0.0 && ctx_.getDirtyEndTime() >= 0.0 && !handDrawBuffer.empty()) {
        const auto& originalF0 = ctx_.getOriginalF0();
        if (originalF0.empty()) {
            ctx_.getState().drawing.isDrawingF0 = false;
            ctx_.setDirtyStartTime(-1.0);
            ctx_.setDirtyEndTime(-1.0);
            ctx_.getState().drawing.handDrawBuffer.clear();
            invalidateIfNeeded(ctx_, dirtyBefore.getUnion(ctx_.getHandDrawPreviewBounds()));
            return;
        }
        const auto f0tl = ctx_.getF0Timeline();
        const auto drawnRange = f0tl.rangeForTimes(ctx_.getDirtyStartTime(), ctx_.getDirtyEndTime());

        std::vector<ManualOp> ops;
        appendManualCorrectionOps(ops,
                                  ctx_.getAudioEditingScheme(),
                                  originalF0,
                                   { drawnRange.startFrame, drawnRange.endFrameExclusive },
                                  [&](int frame) {
                                      return frame >= 0 && frame < static_cast<int>(handDrawBuffer.size())
                                          ? handDrawBuffer[static_cast<std::size_t>(frame)]
                                          : -1.0f;
                                  },
                                  CorrectedSegment::Source::HandDraw);

        if (!ops.empty()) {
            const int editedStartFrame = ops.front().startFrame;
            const int editedEndFrameExclusive = ops.back().endFrameExclusive;
            ctx_.setUndoDescription(juce::String("手绘曲线"));
            ctx_.applyManualCorrection(std::move(ops), editedStartFrame, editedEndFrameExclusive - 1, false);
            ctx_.notifyPitchCurveEdited(editedStartFrame, editedEndFrameExclusive - 1);
            selectNotesForEditedFrameRange(ctx_, editedStartFrame, editedEndFrameExclusive);
        }
    }


    ctx_.getState().drawing.isDrawingF0 = false;
    ctx_.setDirtyStartTime(-1.0);
    ctx_.setDirtyEndTime(-1.0);
    ctx_.getState().drawing.handDrawBuffer.clear();
    invalidateIfNeeded(ctx_, dirtyBefore.getUnion(ctx_.getHandDrawPreviewBounds()));
}

void PianoRollToolHandler::handleDrawNoteUp(const juce::MouseEvent& e)
// 绘制音符工具鼠标释放处理：完成音符绘制，分割重叠音符，应用最小时长
{
    const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));

    if (ctx_.getDrawNoteToolPendingDrag()) {
        ctx_.setDrawNoteToolPendingDrag(false);
        if (ctx_.getNoteDraft().active) {
            ctx_.setUndoDescription(juce::String("绘制音符"));
            ctx_.commitNoteDraft();
            invalidateNoteChange(ctx_, beforeNotes, committedNotes(ctx_));
            ctx_.clearNoteDraft();
        }
        return;
    }
    
    if (!ctx_.getState().drawing.isDrawingNote) {
        return;
    }
    
    ctx_.getState().drawing.isDrawingNote = false;

    const auto projection = ctx_.getMaterializationProjection();
    // §8.5 — DrawNote release writes endTime in SOURCE time.
    double releaseTime = pixelXToSourceTime(e.x);
    if (projection.isValid()) {
        releaseTime = projection.clampMaterializationTime(releaseTime);
    }
    if (releaseTime < 0) releaseTime = 0;

    ctx_.setDrawingNoteEndTime(releaseTime);

    double startTime = std::min(ctx_.getDrawingNoteStartTime(), ctx_.getDrawingNoteEndTime());
    double endTime = std::max(ctx_.getDrawingNoteStartTime(), ctx_.getDrawingNoteEndTime());
    double minDuration = 0.02;

    if ((endTime - startTime) < minDuration) {
        if (ctx_.getDrawingNoteEndTime() >= ctx_.getDrawingNoteStartTime()) {
            endTime = startTime + minDuration;
        } else {
            startTime = endTime - minDuration;
        }
        if (startTime < 0) {
            endTime -= startTime;
            startTime = 0;
        }
    }

    auto notes = ctx_.getNoteDraft().workingNotes;
    if (ctx_.getDrawingNotePitch() > 0.0f) {
        std::vector<Note> updatedNotes;
        updatedNotes.reserve(notes.size() + 2);

        for (size_t i = 0; i < notes.size(); ++i) {
            if (ctx_.getDrawingNoteIndex() >= 0 && static_cast<int>(i) == ctx_.getDrawingNoteIndex()) {
                continue;
            }

            const auto& note = notes[i];
            bool overlap = note.endTime > startTime && note.startTime < endTime;
            if (!overlap) {
                updatedNotes.push_back(note);
                continue;
            }

            if (note.startTime < startTime) {
                Note left = note;
                left.endTime = startTime;
                if (left.endTime > left.startTime) {
                    updatedNotes.push_back(left);
                }
            }

            if (note.endTime > endTime) {
                Note right = note;
                right.startTime = endTime;
                if (right.endTime > right.startTime) {
                    updatedNotes.push_back(right);
                }
            }
        }

        notes = std::move(updatedNotes);

        Note finalNote;
        finalNote.startTime = startTime;
        finalNote.endTime = endTime;
        finalNote.pitch = ctx_.getDrawingNotePitch();
        finalNote.pitchOffset = 0.0f;
        finalNote.retuneSpeed = ctx_.getRetuneSpeed();
        finalNote.vibratoDepth = ctx_.getVibratoDepth();
        finalNote.vibratoRate = ctx_.getVibratoRate();
        finalNote.selected = true;
        finalNote.dirty = true;

        float newPip = ctx_.recalculatePIP(finalNote);
        if (newPip > 0.0f) {
            float sourcePitch = Note::midiToFrequency(Note::frequencyToMidi(newPip));
            finalNote.pitch = sourcePitch;
            finalNote.originalPitch = newPip;

            int targetMidi = Note::frequencyToMidi(ctx_.getDrawingNotePitch());
            int sourceMidi = Note::frequencyToMidi(sourcePitch);
            finalNote.pitchOffset = static_cast<float>(targetMidi - sourceMidi);
        } else {
            finalNote.pitch = ctx_.getDrawingNotePitch();
            finalNote.originalPitch = ctx_.getDrawingNotePitch();
            finalNote.pitchOffset = 0.0f;
        }

        NoteSequence finalSequence;
        finalSequence.setNotesSorted(notes);
        finalSequence.insertNoteSorted(finalNote);
        notes = finalSequence.getNotes();

        deselectAllNotes(notes);
        double midTime = (startTime + endTime) / 2.0;
        int newSelectedIndex = findNoteIndexAt(notes, midTime, finalNote.getAdjustedPitch(), 100.0f);
        if (newSelectedIndex >= 0) {
            notes[static_cast<size_t>(newSelectedIndex)].selected = true;
        }
    }

    ctx_.getNoteDraft().workingNotes = notes;
    ctx_.setUndoDescription(juce::String("绘制音符"));
    ctx_.setDrawingNoteIndex(-1);

    // 同步计算修正并一次性提交音符和F0段，避免产生两个Undo Action
    auto pitchCurve = ctx_.getPitchCurve();
    if (pitchCurve) {
        const auto f0tl = ctx_.getF0Timeline();
        const F0FrameRange noteRange = f0tl.rangeForTimes(startTime, endTime);

        if (!noteRange.isEmpty()) {
            commitNoteBasedCorrection(ctx_, notes, pitchCurve, noteRange);
        } else {
            ctx_.commitNoteDraft();
        }
    } else {
        ctx_.commitNoteDraft();
    }

    ctx_.clearNoteDraft();
    invalidateNoteChange(ctx_, beforeNotes, committedNotes(ctx_));
}

void PianoRollToolHandler::showToolContextMenu(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    // add-note-confirmed-handles §4.2: Time tool 空白右键 → 提供 Re-seed handles from notes 入口
    // 与原 tool selection menu 共存:Time tool active 时显示组合菜单。
    if (currentTool_ == ToolId::TimeTool && ctx_.reSeedTimeGridFromNotes) {
        juce::PopupMenu menu;
        const bool canReSeed = ctx_.canReSeedTimeGridFromNotes
                                ? ctx_.canReSeedTimeGridFromNotes()
                                : false;
        menu.addItem(/*itemId*/1,
                     juce::String("Re-seed handles from notes"),
                     /*isActive*/canReSeed,
                     /*isTicked*/false);
        if (!canReSeed) {
            menu.addSectionHeader(juce::String("(needs auto note generation enabled)"));
        }
        menu.addSeparator();
        menu.addItem(/*itemId*/2, juce::String("Switch tool..."));
        menu.showMenuAsync(juce::PopupMenu::Options{},
                           [this](int result) {
                               if (result == 1) {
                                   if (ctx_.reSeedTimeGridFromNotes) ctx_.reSeedTimeGridFromNotes();
                               } else if (result == 2) {
                                   if (ctx_.showToolSelectionMenu) ctx_.showToolSelectionMenu();
                               }
                           });
        return;
    }
    ctx_.showToolSelectionMenu();
}

void PianoRollToolHandler::deleteSelectedNotes(std::vector<Note>& notes)
{
    notes.erase(
        std::remove_if(notes.begin(), notes.end(), [](const Note& n) {
            return n.selected;
        }),
        notes.end()
    );
}

void PianoRollToolHandler::handleLineAnchorMouseDown(const juce::MouseEvent& e)
// 线锚点工具鼠标按下处理：放置锚点，在锚点间生成线性插值的F0曲线
{
    const auto projection = ctx_.getMaterializationProjection();
    // §8.5 — LineAnchor places anchors at SOURCE time (PitchCurve indexing).
    double clickTime = pixelXToSourceTime(e.x);
    if (projection.isValid()) {
        clickTime = projection.clampMaterializationTime(clickTime);
    }
    float clickFreq = ctx_.yToFreq(static_cast<float>(e.y));
    clickFreq = std::max(20.0f, clickFreq);
    const auto scheme = ctx_.getAudioEditingScheme();

    auto pitchCurve = ctx_.getPitchCurve();
    if (!pitchCurve) return;
    const auto& originalF0 = ctx_.getOriginalF0();
    if (originalF0.empty()) return;
    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) return;

    float midiNote = 69.0f + 12.0f * std::log2(clickFreq / 440.0f);
    int roundedMidi = static_cast<int>(std::round(midiNote));
    float snappedFreq = 440.0f * std::pow(2.0f, static_cast<float>(roundedMidi - 69) / 12.0f);

    int clickFrame = f0tl.frameAtOrBefore(clickTime);

    if (e.getNumberOfClicks() >= 2 && ctx_.getState().drawing.isPlacingAnchors) {
        clearLineAnchorPreview();
        return;
    }

    if (!AudioEditingScheme::canEditFrame(scheme, originalF0, clickFrame)) {
        return;
    }

    if (!ctx_.getState().drawing.isPlacingAnchors) {
        if (AudioEditingScheme::allowsLineAnchorSegmentSelection(scheme)) {
            int segmentIdx = ctx_.findLineAnchorSegmentNear(e.x, e.y);
            if (segmentIdx >= 0) {
                if (e.mods.isCtrlDown() || e.mods.isCommandDown()) {
                    ctx_.toggleLineAnchorSegmentSelection(segmentIdx);
                } else {
                    ctx_.selectLineAnchorSegment(segmentIdx);
                }
                invalidateIfNeeded(ctx_, ctx_.getLineAnchorPreviewBounds());
                return;
            }
        }

        ctx_.clearLineAnchorSegmentSelection();

        ctx_.getState().drawing.isPlacingAnchors = true;
        ctx_.getState().drawing.pendingAnchors.clear();
        LineAnchor firstAnchor;
        firstAnchor.time = clickTime;
        firstAnchor.freq = snappedFreq;
        firstAnchor.id = 0;
        firstAnchor.selected = false;
        ctx_.getState().drawing.pendingAnchors.push_back(firstAnchor);
        ctx_.getState().drawing.currentMousePos = e.position;
        invalidateIfNeeded(ctx_, ctx_.getLineAnchorPreviewBounds());
        return;
    }

    auto& anchors = ctx_.getState().drawing.pendingAnchors;
    const auto& prev = anchors.back();

    auto anchorRange = f0tl.nonEmptyRangeForTimes(prev.time, clickTime);
    const int startFrame = anchorRange.startFrame;
    const int endFrameExclusive = anchorRange.endFrameExclusive;
    const bool previousAnchorIsLeft = prev.time <= clickTime;
    const float leftTargetF0 = previousAnchorIsLeft ? prev.freq : snappedFreq;
    const float rightTargetF0 = previousAnchorIsLeft ? snappedFreq : prev.freq;
    const float retuneSpeed = ctx_.getRetuneSpeed();
    const auto sourceTrend = fitOriginalF0LogTrend(originalF0, startFrame, endFrameExclusive);

    std::vector<ManualOp> ops;
    appendManualCorrectionOps(ops,
                              scheme,
                              originalF0,
                              { startFrame, endFrameExclusive },
                              [&](int frame) {
                                  return lineAnchorF0WithSourceShape(originalF0,
                                                                     frame,
                                                                     startFrame,
                                                                     endFrameExclusive,
                                                                     leftTargetF0,
                                                                     rightTargetF0,
                                                                     retuneSpeed,
                                                                     sourceTrend);
                              },
                              CorrectedSegment::Source::LineAnchor);

    if (ops.empty()) {
        return;
    }

    const int editedStartFrame = ops.front().startFrame;
    const int editedEndFrameExclusive = ops.back().endFrameExclusive;
    ctx_.setUndoDescription(juce::String("锚点修正"));
    ctx_.applyManualCorrection(std::move(ops), editedStartFrame, editedEndFrameExclusive - 1, false);
    ctx_.notifyPitchCurveEdited(editedStartFrame, editedEndFrameExclusive - 1);
    selectNotesForEditedFrameRange(ctx_, editedStartFrame, editedEndFrameExclusive);

    LineAnchor newAnchor;
    newAnchor.time = clickTime;
    newAnchor.freq = snappedFreq;
    newAnchor.id = static_cast<int>(anchors.size());
    newAnchor.selected = false;
    anchors.push_back(newAnchor);
    ctx_.getState().drawing.currentMousePos = e.position;
    invalidateIfNeeded(ctx_, ctx_.getLineAnchorPreviewBounds());
}

void PianoRollToolHandler::handleLineAnchorMouseDrag(const juce::MouseEvent& e) {
    if (!ctx_.getState().drawing.isPlacingAnchors) return;
    const auto dirtyBefore = ctx_.getLineAnchorPreviewBounds();
    ctx_.getState().drawing.currentMousePos = e.position;
    invalidateIfNeeded(ctx_, dirtyBefore.getUnion(ctx_.getLineAnchorPreviewBounds()));
}

void PianoRollToolHandler::clearLineAnchorPreview()
{
    const auto dirtyBefore = ctx_.getLineAnchorPreviewBounds();
    ctx_.getState().drawing.isPlacingAnchors = false;
    ctx_.getState().drawing.pendingAnchors.clear();
    invalidateIfNeeded(ctx_, dirtyBefore.getUnion(ctx_.getLineAnchorPreviewBounds()));
}

int PianoRollToolHandler::findNoteIndexAt(const std::vector<Note>& notes,
                                          double time,
                                          float targetPitchHz,
                                          float pitchToleranceHz)
{
    for (int i = 0; i < static_cast<int>(notes.size()); ++i) {
        const auto& note = notes[static_cast<size_t>(i)];
        if (time >= note.startTime && time < note.endTime) {
            const float adjustedPitch = note.getAdjustedPitch();
            if (std::abs(adjustedPitch - targetPitchHz) <= pitchToleranceHz) {
                return i;
            }
        }
    }
    return -1;
}

std::vector<int> PianoRollToolHandler::collectSelectedNoteIndices(const std::vector<Note>& notes)
{
    std::vector<int> selectedIndices;
    for (int i = 0; i < static_cast<int>(notes.size()); ++i) {
        if (notes[static_cast<size_t>(i)].selected) {
            selectedIndices.push_back(i);
        }
    }
    return selectedIndices;
}

void PianoRollToolHandler::deselectAllNotes(std::vector<Note>& notes)
{
    for (auto& note : notes) {
        note.selected = false;
    }
}

void PianoRollToolHandler::selectAllNotes(std::vector<Note>& notes)
{
    for (auto& note : notes) {
        note.selected = true;
    }
}

int PianoRollToolHandler::findLastSelectedNoteIndex(const std::vector<Note>& notes)
{
    const auto selectedIndices = collectSelectedNoteIndices(notes);
    if (selectedIndices.empty()) {
        return -1;
    }

    int lastSelectedIndex = selectedIndices.front();
    for (int index : selectedIndices) {
        if (notes[static_cast<size_t>(index)].startTime > notes[static_cast<size_t>(lastSelectedIndex)].startTime) {
            lastSelectedIndex = index;
        }
    }
    return lastSelectedIndex;
}

void PianoRollToolHandler::selectNotesBetween(std::vector<Note>& notes, int startIndex, int endIndex)
{
    if (startIndex < 0 || endIndex < 0
        || startIndex >= static_cast<int>(notes.size())
        || endIndex >= static_cast<int>(notes.size())) {
        return;
    }

    const double minTime = std::min(notes[static_cast<size_t>(startIndex)].startTime,
                                    notes[static_cast<size_t>(endIndex)].startTime);
    const double maxTime = std::max(notes[static_cast<size_t>(startIndex)].startTime,
                                    notes[static_cast<size_t>(endIndex)].startTime);

    for (auto& note : notes) {
        if (note.startTime >= minTime && note.startTime <= maxTime) {
            note.selected = true;
        }
    }
}

void PianoRollToolHandler::updateF0SelectionFromNotes(const std::vector<Note>& notes)
{
    const auto selectedIndices = collectSelectedNoteIndices(notes);
    if (selectedIndices.empty()) {
        ctx_.getState().selection.clearF0Selection();
        return;
    }

    double minStart = 1e30;
    double maxEnd = -1e30;
    for (int index : selectedIndices) {
        const auto& note = notes[static_cast<size_t>(index)];
        minStart = std::min(minStart, note.startTime);
        maxEnd = std::max(maxEnd, note.endTime);
    }

    auto curve = ctx_.getPitchCurve();
    if (!curve) {
        ctx_.getState().selection.clearF0Selection();
        return;
    }
    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) {
        ctx_.getState().selection.clearF0Selection();
        return;
    }
    const auto selectedRange = f0tl.nonEmptyRangeForTimes(minStart, maxEnd);
    ctx_.getState().selection.setF0Range(selectedRange.startFrame,
                                         selectedRange.endFrameExclusive);
}

// ============================================================================
// vocal-time-stretch §8.4 (Phase F) — Time tool handlers
//
// Minimal scaffolding: hover detection + selection + drag + commit.
// Phase G will add: double-click-insert, delete-handle, Alt-snap-disable,
// group multi-handle drag, and 30 ms minimum spacing clamp.
// ============================================================================

uint64_t PianoRollToolHandler::hitTestTimeGridHandle(const juce::MouseEvent& e) const
{
    if (!ctx_.getTimeGridSnapshot) return 0;
    auto snap = ctx_.getTimeGridSnapshot();
    if (snap == nullptr) return 0;

    constexpr int kHitToleranceX = 5;  // pixels

    // add-note-confirmed-handles fix: use uint64_t throughout. Previous
    // `static_cast<int>(h.id)` truncated 64-bit handle ids to 32-bit signed,
    // which silently broke handles whose id value used high bits (e.g.
    // NoteOnly handles produced by HandleNoteMerger with `1ULL<<62` tag bit).
    uint64_t closestId = 0;
    int closestDistance = std::numeric_limits<int>::max();
    for (const auto& h : snap->handles()) {
        if (h.locked) continue;   // endpoints not selectable
        const int handleX = ctx_.timeToX(h.output_seconds);
        const int dx = std::abs(e.x - handleX);
        if (dx <= kHitToleranceX && dx < closestDistance) {
            closestId = h.id;
            closestDistance = dx;
        }
    }
    return closestId;
}

void PianoRollToolHandler::handleTimeToolMouseMove(const juce::MouseEvent& e)
{
    auto& tt = ctx_.getState().timeTool;
    const uint64_t prevHovered = tt.hoveredHandleId;
    const uint64_t hovered = hitTestTimeGridHandle(e);
    tt.hoveredHandleId = hovered;

    if (hovered != 0) {
        ctx_.setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    } else {
        ctx_.setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    if (prevHovered != hovered && ctx_.notifyTimeGridChanged) {
        ctx_.notifyTimeGridChanged();
    }
}

void PianoRollToolHandler::handleTimeToolMouseDown(const juce::MouseEvent& e)
{
    auto& tt = ctx_.getState().timeTool;
    const uint64_t hitId = hitTestTimeGridHandle(e);

    if (hitId == 0) {
        // Clicked empty space — clear selection (unless Shift held).
        if (!e.mods.isShiftDown()) {
            if ((tt.selectedHandleId != 0 || !tt.additionalSelectedIds.empty())
                && ctx_.notifyTimeGridChanged) {
                ctx_.notifyTimeGridChanged();
            }
            tt.selectedHandleId = 0;
            tt.additionalSelectedIds.clear();
        }
        tt.isDraggingHandle = false;
        return;
    }

    // Found a handle — select + arm drag.
    auto snap = ctx_.getTimeGridSnapshot ? ctx_.getTimeGridSnapshot() : nullptr;
    if (snap == nullptr) {
        AppLogger::warn("[TimeTool] mouseDown: no TimeGridSnapshot available");
        return;
    }

    const TimeHandle* hitHandle = nullptr;
    for (const auto& h : snap->handles()) {
        if (h.id == hitId) { hitHandle = &h; break; }
    }
    if (hitHandle == nullptr || hitHandle->locked) return;

    // ⚡️ §8.4 (Phase H) — Shift+click toggles in additionalSelectedIds
    // (multi-select).  Bare click replaces the selection.
    if (e.mods.isShiftDown()) {
        if (tt.selectedHandleId == 0) {
            tt.selectedHandleId = hitId;
        } else if (hitId == tt.selectedHandleId) {
            // No-op: clicking primary selection again with Shift is
            // typically a no-op in DAW conventions (would otherwise demote
            // primary to secondary which is confusing).
        } else {
            // Toggle in additionalSelectedIds
            auto it = std::find(tt.additionalSelectedIds.begin(),
                                 tt.additionalSelectedIds.end(), hitId);
            if (it != tt.additionalSelectedIds.end()) {
                tt.additionalSelectedIds.erase(it);
            } else {
                tt.additionalSelectedIds.push_back(hitId);
            }
        }
    } else {
        tt.selectedHandleId = hitId;
        tt.additionalSelectedIds.clear();
    }

    tt.isDraggingHandle = true;
    tt.draggedHandleId = hitId;
    tt.dragSnapDisabled = e.mods.isAltDown();   // Phase H: Alt disables clamp
    tt.dragOriginalSnapshot = snap;
    tt.dragWorkingSnapshot = snap;   // identity at drag start
    tt.dragStartOutputSeconds = hitHandle->output_seconds;
    tt.dragStartPixel = e.getPosition();

    if (ctx_.notifyTimeGridChanged) ctx_.notifyTimeGridChanged();
}

void PianoRollToolHandler::handleTimeToolMouseDrag(const juce::MouseEvent& e)
{
    auto& tt = ctx_.getState().timeTool;
    if (!tt.isDraggingHandle || tt.dragOriginalSnapshot == nullptr) return;

    const double newOutputTime = ctx_.xToTime(e.x);
    if (newOutputTime < 0.0) return;

    const auto& origHandles = tt.dragOriginalSnapshot->handles();

    // Locate the dragged handle's index.
    int draggedIdx = -1;
    for (int i = 0; i < static_cast<int>(origHandles.size()); ++i) {
        if (origHandles[i].id == tt.draggedHandleId) { draggedIdx = i; break; }
    }
    if (draggedIdx <= 0 || draggedIdx >= static_cast<int>(origHandles.size()) - 1) {
        return;   // endpoints can't be dragged
    }

    // ⚡️ §8.4 (Phase H) — group-drag detection.
    // If the user has multi-selected handles AND the dragged handle is part
    // of that selection, every selected handle moves by the same delta
    // (uniformDelta).  Otherwise only the dragged handle moves.
    const bool isGroupDrag = (!tt.additionalSelectedIds.empty())
                              && tt.isSelected(tt.draggedHandleId);

    // Phase G/H spacing rule: 30 ms minimum between adjacent handles.
    // Alt held at drag start (`dragSnapDisabled`) bypasses the clamp for
    // power-user nudging into tight regions.
    const double kMinSpacingSec = tt.dragSnapDisabled ? 0.000 : 0.030;
    const double minOutput = origHandles[static_cast<size_t>(draggedIdx - 1)].output_seconds + kMinSpacingSec;
    const double maxOutput = origHandles[static_cast<size_t>(draggedIdx + 1)].output_seconds - kMinSpacingSec;
    const double clampedOutput = juce::jlimit(minOutput, maxOutput, newOutputTime);
    const double uniformDelta = clampedOutput - tt.dragStartOutputSeconds;

    // Build new handle vector.  For group drag we shift every selected,
    // non-locked handle by uniformDelta and clamp each individually to its
    // own neighbor bounds.  Non-selected handles keep their original output.
    std::vector<TimeHandle> newHandles(origHandles.begin(), origHandles.end());

    auto isHandleSelected = [&tt](uint64_t id) -> bool {
        return tt.isSelected(id);
    };

    if (isGroupDrag) {
        // First pass: tentative outputs by uniform delta.
        std::vector<double> tentative(newHandles.size());
        for (size_t i = 0; i < newHandles.size(); ++i) {
            tentative[i] = newHandles[i].output_seconds;
        }
        for (size_t i = 1; i < newHandles.size() - 1; ++i) {
            if (newHandles[i].locked) continue;
            if (isHandleSelected(newHandles[i].id)) {
                tentative[i] = newHandles[i].output_seconds + uniformDelta;
            }
        }

        // Second pass: clamp each tentative to its already-clamped neighbors
        // (left-to-right sweep ensures monotonicity is preserved).
        for (size_t i = 1; i < tentative.size() - 1; ++i) {
            const double localMin = tentative[i - 1] + kMinSpacingSec;
            const double localMax = tentative[i + 1] - kMinSpacingSec;
            if (localMin > localMax) {
                // Region too tight — abort the drag step (keep last valid
                // working snapshot).
                return;
            }
            tentative[i] = juce::jlimit(localMin, localMax, tentative[i]);
        }

        for (size_t i = 0; i < newHandles.size(); ++i) {
            newHandles[i].output_seconds = tentative[i];
        }
    } else {
        newHandles[static_cast<size_t>(draggedIdx)].output_seconds = clampedOutput;
    }

    auto newSnap = TimeGridSnapshot::makeFromHandles(
        std::move(newHandles),
        /*revision=*/tt.dragOriginalSnapshot->revision() + 1);
    if (newSnap == nullptr) {
        AppLogger::warn("[TimeTool] makeFromHandles failed during drag (validation)");
        return;
    }

    tt.dragWorkingSnapshot = newSnap;
    if (ctx_.notifyTimeGridChanged) ctx_.notifyTimeGridChanged();
}

void PianoRollToolHandler::handleTimeToolMouseUp(const juce::MouseEvent& /*e*/)
{
    auto& tt = ctx_.getState().timeTool;
    if (!tt.isDraggingHandle) return;

    if (tt.dragWorkingSnapshot != nullptr
        && tt.dragWorkingSnapshot != tt.dragOriginalSnapshot
        && ctx_.commitTimeGrid) {
        // Compute affected source frame range from the dragged handle's
        // source_seconds neighborhood (per undo-affected-range-invariant.md
        // — the range must be supplied by the UI at edit time, never derived
        // from snapshot diff afterwards).
        const auto& handles = tt.dragWorkingSnapshot->handles();
        int draggedIdx = -1;
        for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
            if (handles[i].id == tt.draggedHandleId) { draggedIdx = i; break; }
        }

        int64_t affectedStartFrame = 0;
        int64_t affectedEndFrame   = 0;
        if (draggedIdx > 0 && draggedIdx < static_cast<int>(handles.size()) - 1) {
            const F0Timeline f0tl = ctx_.getF0Timeline ? ctx_.getF0Timeline() : F0Timeline{};
            const double srcStartSec = handles[static_cast<size_t>(draggedIdx - 1)].source_seconds;
            const double srcEndSec   = handles[static_cast<size_t>(draggedIdx + 1)].source_seconds;
            if (!f0tl.isEmpty()) {
                const auto range = f0tl.rangeForTimes(srcStartSec, srcEndSec);
                affectedStartFrame = range.startFrame;
                affectedEndFrame   = range.endFrameExclusive;
            } else {
                affectedStartFrame = static_cast<int64_t>(srcStartSec * 100.0);
                affectedEndFrame   = static_cast<int64_t>(srcEndSec   * 100.0);
            }
        }

        ctx_.commitTimeGrid(tt.dragWorkingSnapshot,
                             tt.dragOriginalSnapshot,
                             affectedStartFrame,
                             affectedEndFrame,
                             juce::String("拖动时间手柄"));
    }

    tt.isDraggingHandle = false;
    tt.draggedHandleId = 0;
    tt.dragOriginalSnapshot.reset();
    tt.dragWorkingSnapshot.reset();
    if (ctx_.notifyTimeGridChanged) ctx_.notifyTimeGridChanged();
}

// ============================================================================
// §8.4 (Phase G) — Double-click to insert UserAdded handle
//
// Constraints (per spec time-tool-interaction.md):
//   - Click must be on empty area (no existing handle within ±5 px)
//   - Click position must be ≥0.030 s away from any neighbor's output_seconds
//   - source_seconds initially equals output_seconds (identity insertion);
//     subsequent drag operations modify only output_seconds
// ============================================================================
void PianoRollToolHandler::handleTimeToolMouseDoubleClick(const juce::MouseEvent& e)
{
    if (!ctx_.getTimeGridSnapshot || !ctx_.commitTimeGrid) return;
    auto snap = ctx_.getTimeGridSnapshot();
    if (snap == nullptr) return;

    const double clickedTime = ctx_.xToTime(e.x);
    if (clickedTime <= 0.0) return;

    const auto& handles = snap->handles();
    if (handles.size() < 2) return;

    // Reject if too close to existing handle (would violate 30ms spacing).
    constexpr double kMinSpacingSec = 0.030;
    for (const auto& h : handles) {
        if (std::abs(h.output_seconds - clickedTime) < kMinSpacingSec) {
            AppLogger::log("[TimeTool] insert rejected: within 30ms of existing handle");
            return;
        }
    }

    // Find insertion index by output_seconds order.
    int insertIdx = -1;
    for (int i = 0; i < static_cast<int>(handles.size()) - 1; ++i) {
        if (handles[static_cast<size_t>(i)].output_seconds < clickedTime
            && clickedTime < handles[static_cast<size_t>(i + 1)].output_seconds) {
            insertIdx = i + 1;
            break;
        }
    }
    if (insertIdx <= 0 || insertIdx >= static_cast<int>(handles.size())) {
        AppLogger::log("[TimeTool] insert rejected: click outside [ClipStart, ClipEnd] range");
        return;
    }

    // Build new handle vector with the inserted UserAdded handle.  Generate a
    // fresh handle id by taking max-existing + 1 (matches TimeGrid's stable-id
    // semantics; survives validation because it's monotonic w.r.t. existing).
    std::vector<TimeHandle> newHandles(handles.begin(), handles.end());
    uint64_t maxId = 0;
    for (const auto& h : newHandles) maxId = std::max(maxId, h.id);

    TimeHandle newHandle;
    newHandle.id = maxId + 1;
    newHandle.source_seconds = clickedTime;   // identity insert
    newHandle.output_seconds = clickedTime;
    newHandle.kind = HandleKind::UserAdded;
    newHandle.locked = false;
    newHandles.insert(newHandles.begin() + insertIdx, newHandle);

    auto newSnap = TimeGridSnapshot::makeFromHandles(
        std::move(newHandles), /*revision=*/snap->revision() + 1);
    if (newSnap == nullptr) {
        AppLogger::warn("[TimeTool] insert: makeFromHandles validation failed");
        return;
    }

    // Compute affected source frame range from neighbors.
    int64_t affectedStart = 0;
    int64_t affectedEnd   = 0;
    {
        const F0Timeline f0tl = ctx_.getF0Timeline ? ctx_.getF0Timeline() : F0Timeline{};
        const double srcStartSec = handles[static_cast<size_t>(insertIdx - 1)].source_seconds;
        const double srcEndSec   = handles[static_cast<size_t>(insertIdx)].source_seconds;
        if (!f0tl.isEmpty()) {
            const auto range = f0tl.rangeForTimes(srcStartSec, srcEndSec);
            affectedStart = range.startFrame;
            affectedEnd   = range.endFrameExclusive;
        } else {
            affectedStart = static_cast<int64_t>(srcStartSec * 100.0);
            affectedEnd   = static_cast<int64_t>(srcEndSec   * 100.0);
        }
    }

    ctx_.commitTimeGrid(newSnap, snap, affectedStart, affectedEnd,
                         juce::String("插入时间手柄"));

    // Auto-select the newly-inserted handle so user can immediately drag.
    auto& tt = ctx_.getState().timeTool;
    tt.selectedHandleId = newHandle.id;
    if (ctx_.notifyTimeGridChanged) ctx_.notifyTimeGridChanged();
}

// ============================================================================
// §8.4 (Phase G) — Delete key removes selected handle (non-endpoint only)
//
// Returns true when a handle was deleted (caller should not fall through to
// note-delete logic).  Returns false when nothing was selected or the only
// selected handle is a locked endpoint.
// ============================================================================
bool PianoRollToolHandler::handleTimeToolDeleteSelected()
{
    if (!ctx_.getTimeGridSnapshot || !ctx_.commitTimeGrid) return false;
    auto& tt = ctx_.getState().timeTool;
    if (tt.selectedHandleId == 0) return false;

    auto snap = ctx_.getTimeGridSnapshot();
    if (snap == nullptr) return false;

    const auto& handles = snap->handles();
    int targetIdx = -1;
    for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
        if (handles[static_cast<size_t>(i)].id == tt.selectedHandleId) {
            targetIdx = i;
            break;
        }
    }
    if (targetIdx <= 0 || targetIdx >= static_cast<int>(handles.size()) - 1) {
        // Endpoint or not found — cannot delete
        AppLogger::log("[TimeTool] delete rejected: cannot delete endpoint or unknown handle");
        return false;
    }
    if (handles[static_cast<size_t>(targetIdx)].locked) {
        AppLogger::log("[TimeTool] delete rejected: handle is locked");
        return false;
    }

    // Build new handle vector without the target.
    std::vector<TimeHandle> newHandles(handles.begin(), handles.end());
    newHandles.erase(newHandles.begin() + targetIdx);

    auto newSnap = TimeGridSnapshot::makeFromHandles(
        std::move(newHandles), /*revision=*/snap->revision() + 1);
    if (newSnap == nullptr) {
        AppLogger::warn("[TimeTool] delete: makeFromHandles validation failed");
        return false;
    }

    // Affected range = source span between the deleted handle's neighbors.
    int64_t affectedStart = 0;
    int64_t affectedEnd   = 0;
    {
        const F0Timeline f0tl = ctx_.getF0Timeline ? ctx_.getF0Timeline() : F0Timeline{};
        const double srcStartSec = handles[static_cast<size_t>(targetIdx - 1)].source_seconds;
        const double srcEndSec   = handles[static_cast<size_t>(targetIdx + 1)].source_seconds;
        if (!f0tl.isEmpty()) {
            const auto range = f0tl.rangeForTimes(srcStartSec, srcEndSec);
            affectedStart = range.startFrame;
            affectedEnd   = range.endFrameExclusive;
        } else {
            affectedStart = static_cast<int64_t>(srcStartSec * 100.0);
            affectedEnd   = static_cast<int64_t>(srcEndSec   * 100.0);
        }
    }

    ctx_.commitTimeGrid(newSnap, snap, affectedStart, affectedEnd,
                         juce::String("删除时间手柄"));

    tt.selectedHandleId = 0;
    tt.hoveredHandleId  = 0;
    if (ctx_.notifyTimeGridChanged) ctx_.notifyTimeGridChanged();
    return true;
}

} // namespace OpenTune
