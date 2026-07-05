#include "PianoRollToolHandler.h"
#include "../../../Utils/AudioEditingScheme.h"
#include "../../../Utils/PitchUtils.h"
#include "../../../Utils/AppLogger.h"
#include "../../../Utils/KeyShortcutConfig.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

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
                               PitchCorrectionSegment::Source source)
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

std::vector<PitchCorrectionSegment> buildSegmentsWithManualOps(
    const std::shared_ptr<PitchCurve>& pitchCurve,
    const std::vector<PianoRollToolHandler::ManualCorrectionOp>& ops)
{
    std::vector<PitchCorrectionSegment> segments;
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
    segments.reserve(snapshot->getCorrectionSegments().size());
    for (const auto& segment : snapshot->getCorrectionSegments()) {
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
                                        ctx.getVibratoRate());

    const auto snap = clonedCurve->getSnapshot();
    const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(editRange.startFrame,
                                                                          editRange.endFrameExclusive,
                                                                          f0tl.endFrameExclusive());

    // Extract segments overlapping the affected range (range-scoped, not full)
    auto allSegments = snap->getCorrectionSegments();
    std::vector<PitchCorrectionSegment> segmentsInRange;
    for (const auto& seg : allSegments) {
        if (seg.startFrame < affectedRange.endFrameExclusive && seg.endFrame > affectedRange.startFrame)
            segmentsInRange.push_back(seg);
    }

    if (!ctx.commitNotesAndSegments(notes, segmentsInRange, affectedRange)) {
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
    const int manualStartFrame = ctx.getNoteDragManualStartFrame();
    const int manualEndFrameExclusive = ctx.getNoteDragManualEndFrameExclusive();
    if (manualStartFrame < 0 || manualEndFrameExclusive <= manualStartFrame || manualTargets.empty()) {
        return;
    }

    const F0FrameRange manualRange{manualStartFrame, manualEndFrameExclusive};

    ctx.setNoteDragPreviewStartFrame(manualRange.startFrame);
    ctx.setNoteDragPreviewEndFrameExclusive(manualRange.endFrameExclusive);

    auto& preview = ctx.getNoteDragPreviewF0();
    preview.assign(static_cast<std::size_t>(manualRange.endFrameExclusive - manualRange.startFrame), -1.0f);
    for (const auto& target : manualTargets) {
        const int frame = target.frame;
        const int relIndex = frame - manualRange.startFrame;
        if (relIndex >= 0 && relIndex < static_cast<int>(preview.size())) {
            preview[static_cast<std::size_t>(relIndex)] = target.f0 * shiftFactor;
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
    invalidateIfNeeded(ctx, dirty);
    if (ctx.repaintPreviewOverlay) ctx.repaintPreviewOverlay();
}

}

// ============================================================================
// PianoRollToolHandler - 钢琴卷帘工具处理器实�?
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

std::optional<double> PianoRollToolHandler::pixelXToSourceTime(int pixelX) const
{
    // Pipeline: pixelX → timeline → output(content) → tauInverse → source.
    // invalid projection 意味着没有 edit target，返回 nullopt。
    // 一旦 projection valid，TimeGrid 必须参与 output/source 转换，
    // identity TimeGrid 的 tauInverse 本身就是 identity，无需特判。
    const auto projection = ctx_.getContentProjection();
    if (!projection.isValid())
        return std::nullopt;

    const auto grid = ctx_.getTimeGridSnapshot();
    jassert(grid != nullptr);

    const double timelineSeconds = ctx_.getViewMapper().xToTime(pixelX);
    const double outputSeconds = projection.projectTimelineTimeToContent(timelineSeconds);
    return grid->tauInverse(outputSeconds);
}

double PianoRollToolHandler::sourceTimeToTimelineTime(double sourceSeconds) const
{
    // Pipeline: source → tauForward → output(content) → timeline.
    // projection valid 是调用方契约（入口已检查）。
    const auto projection = ctx_.getContentProjection();
    jassert(projection.isValid());

    const auto grid = ctx_.getTimeGridSnapshot();
    jassert(grid != nullptr);

    const double outputSeconds = grid->tauForward(sourceSeconds);
    return projection.projectContentTimeToTimeline(outputSeconds);
}

int PianoRollToolHandler::sourceTimeToScreenX(double sourceSeconds) const
{
    return ctx_.getViewMapper().timeToX(sourceTimeToTimelineTime(sourceSeconds));
}

SourceEditRange PianoRollToolHandler::sourceEditRange(double minDurationSeconds) const
{
    const auto grid = ctx_.getTimeGridSnapshot();
    jassert(grid != nullptr);
    return SourceEditRange::fromTimeGrid(*grid, minDurationSeconds);
}

void PianoRollToolHandler::mouseMove(const juce::MouseEvent& e)
// 鼠标移动处理：更新光标形状（音符边缘调整、线锚点预览�?
{
    if (e.mods.isCtrlDown()) {
        ctx_.setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }

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

    const auto projection = ctx_.getContentProjection();
    if (!projection.isValid())
        return;

    int edgeThreshold = 6;

    bool cursorSet = false;
    float mousePitch = ctx_.getViewMapper().yToFreq((float)e.y);
    float mouseMidiVal = 69.0f + 12.0f * std::log2(mousePitch / 440.0f) - 0.5f;

    for (const auto& note : displayNotes(ctx_)) {
        const int x1 = sourceTimeToScreenX(note.startTime);
        const int x2 = sourceTimeToScreenX(note.endTime);
        
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
        double clickedTime = ctx_.getViewMapper().xToTime(e.x);
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
        const auto& notes = committedNotes(ctx_);
        auto curve = ctx_.getPitchCurve();
        bool hasNotes = !notes.empty();
        bool hasCurve = curve && !curve->isEmpty();
        
        if (!hasNotes && !hasCurve) {
            return true;
        }
        
        if (hasNotes) {
            selectAllNotes(notes);
        }
        
        const auto& committed = committedNotes(ctx_);
        
        ctx_.getState().selection.hasSelectionArea = true;
        ctx_.getState().selection.selectionStartMidi = ctx_.getMinMidi();
        ctx_.getState().selection.selectionEndMidi = ctx_.getMaxMidi();
        ctx_.getState().selection.selectionStartTime = 0.0;
        
        if (hasCurve) {
            const auto f0tl = ctx_.getF0Timeline();
            ctx_.getState().selection.selectionEndTime = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(f0tl.endFrameExclusive());
        } else {
            double maxEnd = 0.0;
            for (const auto& n : committed) {
                maxEnd = std::max(maxEnd, n.endTime);
            }
            ctx_.getState().selection.selectionEndTime = maxEnd;
        }
        
        updateF0SelectionFromNotes(committed);
        invalidateNoteChange(ctx_, beforeNotes, committedNotes(ctx_));
        if (ctx_.invalidateInteractionVisual) {
            ctx_.invalidateInteractionVisual();
        }
        return true;
    }

    // ==== Tool switching (via configurable shortcuts) ====
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::ToolDrawNote, key)) {
        ctx_.setCurrentTool(ToolId::DrawNote);
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::ToolSelect, key)) {
        ctx_.setCurrentTool(ToolId::Select);
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::ToolLineAnchor, key)) {
        ctx_.setCurrentTool(ToolId::LineAnchor);
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::ToolHandDraw, key)) {
        ctx_.setCurrentTool(ToolId::HandDraw);
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::ToolTimeTool, key)) {
        ctx_.setCurrentTool(ToolId::TimeTool);
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::ToolAutoTune, key)) {
        ctx_.notifyAutoTuneRequested();
        return true;
    }
    // ==== End tool switching ====

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::PlayPause, key)) {
        ctx_.notifyPlayPauseToggle();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Stop, key)) {
        ctx_.notifyStopPlayback();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Delete, key)) {
        // Time tool always consumes Delete to avoid accidentally deleting notes
        // when a handle isn't selected. No-op when nothing's selected.
        if (currentTool_ == ToolId::TimeTool) {
            handleTimeToolDeleteSelected();
            return true;
        }
        handleDeleteKey();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::CancelSelection, key)) {
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

    // TimeTool �?mouseDown 自己处理 hit-test handle / 空区两种语义�?
    // 不能被空区意图捕获，否则 handleTimeToolMouseDown 永不触发，handle 无法拖动�?
    if (currentTool_ == ToolId::TimeTool) {
        return false;
    }

    if (hitsNoteBodyOrResizeEdge(e)) {
        return false;
    }

    int f0Frame = -1;
    if (currentTool_ == ToolId::Select && hitTestF0Curve(e, f0Frame)) {
        return false;
    }

    return true;
}

bool PianoRollToolHandler::hitsNoteBodyOrResizeEdge(const juce::MouseEvent& e)
{
    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return false;
    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime))
        return false;

    const float clickedPitch = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y));
    const float mouseMidi = 69.0f + 12.0f * std::log2(clickedPitch / 440.0f) - 0.5f;
    constexpr int edgeThreshold = 6;

    for (const auto& note : displayNotes(ctx_)) {
        const float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f) - 0.5f;
        if (std::abs(mouseMidi - noteMidi) >= 1.0f) {
            continue;
        }

        const int x1 = sourceTimeToScreenX(note.startTime);
        const int x2 = sourceTimeToScreenX(note.endTime);
        const bool insideBody = e.x >= x1 && e.x <= x2;
        const bool nearEdge = std::abs(e.x - x1) <= edgeThreshold || std::abs(e.x - x2) <= edgeThreshold;
        if (insideBody || nearEdge) {
            return true;
        }
    }

    return false;
}

bool PianoRollToolHandler::hitTestF0Curve(const juce::MouseEvent& e, int& frameIndex) const
{
    frameIndex = -1;
    if (e.x <= ctx_.getPianoKeyWidth()) {
        return false;
    }

    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) {
        return false;
    }

    auto curve = ctx_.getPitchCurve();
    if (curve == nullptr) {
        return false;
    }

    auto snapshot = curve->getSnapshot();
    if (snapshot == nullptr || snapshot->isEmpty()) {
        return false;
    }

    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return false;
    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime)) {
        return false;
    }

    const int frameCount = static_cast<int>(snapshot->size());
    const int centerFrame = juce::jlimit(0, frameCount - 1, f0tl.frameAtOrBefore(*sourceTime));
    const int startFrame = std::max(0, centerFrame - 2);
    const int endFrameExclusive = std::min(frameCount, centerFrame + 3);
    const auto& originalF0 = snapshot->getOriginalF0();

    std::vector<float> correctedF0(static_cast<size_t>(endFrameExclusive - startFrame), 0.0f);
    if (snapshot->hasCorrectionLayer()) {
        snapshot->renderCorrectionLayerF0Range(
            startFrame,
            endFrameExclusive,
            [startFrame, &correctedF0](int offsetFrame, const float* data, int length) {
                const int relStart = offsetFrame - startFrame;
                if (data == nullptr || length <= 0 || relStart >= static_cast<int>(correctedF0.size())) {
                    return;
                }
                const int copyStart = std::max(0, relStart);
                const int sourceOffset = copyStart - relStart;
                const int copyLength = std::min(length - sourceOffset,
                                                static_cast<int>(correctedF0.size()) - copyStart);
                if (copyLength > 0) {
                    std::copy_n(data + sourceOffset,
                                copyLength,
                                correctedF0.begin() + copyStart);
                }
            });
    }

    constexpr float kHitTolerancePx = 7.0f;
    float bestDistanceSquared = kHitTolerancePx * kHitTolerancePx;
    int bestFrame = -1;

    auto testCandidate = [&](int frame, float frequency) {
        if (frequency <= 0.0f) {
            return;
        }

        const int x = sourceTimeToScreenX(f0tl.timeAtFrame(frame));
        const float y = ctx_.getViewMapper().freqToY(frequency);
        const float dx = static_cast<float>(e.x - x);
        const float dy = static_cast<float>(e.y) - y;
        const float distanceSquared = dx * dx + dy * dy;
        if (distanceSquared <= bestDistanceSquared) {
            bestDistanceSquared = distanceSquared;
            bestFrame = frame;
        }
    };

    for (int frame = startFrame; frame < endFrameExclusive; ++frame) {
        if (frame < static_cast<int>(originalF0.size())) {
            testCandidate(frame, originalF0[static_cast<size_t>(frame)]);
        }
        const float corrected = correctedF0[static_cast<size_t>(frame - startFrame)];
        testCandidate(frame, corrected);
    }

    frameIndex = bestFrame;
    return bestFrame >= 0;
}

void PianoRollToolHandler::beginF0SelectionAt(const juce::MouseEvent& e, int frameIndex)
{
    juce::ignoreUnused(e);
    auto& state = ctx_.getState();
    state.noteSelection.clear();
    state.noteDrag.clear();
    state.noteResize.clear();
    state.selection.hasSelectionArea = false;
    state.selection.isSelectingArea = false;
    state.selection.isSelectingF0 = true;
    state.selection.f0SelectionAnchorFrame = frameIndex;
    state.selection.setF0Range(frameIndex, frameIndex + 1);
    ctx_.clearNoteDraft();
    if (ctx_.invalidateInteractionVisual) {
        ctx_.invalidateInteractionVisual();
    }
}

void PianoRollToolHandler::updateF0SelectionDrag(const juce::MouseEvent& e)
{
    auto& selection = ctx_.getState().selection;
    if (!selection.isSelectingF0 || selection.f0SelectionAnchorFrame < 0) {
        return;
    }

    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) {
        selection.clearF0Selection();
        return;
    }

    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return;

    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime))
        return;

    const int frame = juce::jlimit(0, f0tl.endFrameExclusive() - 1, f0tl.frameAtOrBefore(*sourceTime));
    const int startFrame = std::min(selection.f0SelectionAnchorFrame, frame);
    const int endFrameExclusive = std::max(selection.f0SelectionAnchorFrame, frame) + 1;
    selection.setF0Range(startFrame, endFrameExclusive);
    selection.isSelectingF0 = true;
    if (ctx_.invalidateInteractionVisual) {
        ctx_.invalidateInteractionVisual();
    }
}

void PianoRollToolHandler::beginEmptySpaceIntent(const juce::MouseEvent& e)
{
    auto& intent = ctx_.getState().emptySpaceIntent;
    intent.active = true;
    intent.tool = currentTool_;
    intent.mouseDownPos = e.getPosition();
    intent.mouseDownTime = ctx_.getViewMapper().xToTime(e.x);
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
    state.selection.isSelectingF0 = false;
    state.selection.f0SelectionAnchorFrame = -1;
    state.isPanning = false;
    state.selectedLineAnchorSegmentIds.clear();
    state.drawing.isDrawingF0 = false;
    state.drawing.handDrawBuffer.clear();
    state.drawing.isDrawingNote = false;
    state.drawing.isPlacingAnchors = false;
    state.drawing.pendingAnchors.clear();
    state.timeTool.clear();
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
        ctx_.clearNoteDraft();
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
        ctx_.getNoteDraft().contentDirty = true;
        ctx_.getNoteDraft().workingNotes = notes;
        ctx_.setUndoDescription(juce::String("删除音符"));

        // 同步计算清除修正 + 删除音符 �?一次性原子提�?
        bool committed = false;
        if (curve && !correctionClearRanges.empty()) {
            auto clonedCurve = curve->clone();
            for (const auto& range : correctionClearRanges) {
                clonedCurve->clearCorrectionRange(range.startFrame, range.endFrameExclusive);
            }
            auto snap = clonedCurve->getSnapshot();
            // delete 路径：affectedRange = globalDirty*Frame 的覆盖范围（含端点）�?
            // �?F0FrameRange �?endFrameExclusive 语义�?
            const F0FrameRange affectedRange{globalDirtyStartFrame, globalDirtyEndFrame + 1};

            // Extract segments overlapping the affected range (range-scoped, not full)
            auto allSegments = snap->getCorrectionSegments();
            std::vector<PitchCorrectionSegment> segmentsInRange;
            for (const auto& seg : allSegments) {
                if (seg.startFrame < affectedRange.endFrameExclusive && seg.endFrame > affectedRange.startFrame)
                    segmentsInRange.push_back(seg);
            }

            committed = ctx_.commitNotesAndSegments(notes, segmentsInRange, affectedRange);
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
// 选择工具鼠标按下处理：检测音符边缘调整、音符选中/取消选中、框选区域开�?
{
    const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));
    const auto& notes = committedNotes(ctx_);

    ctx_.getState().noteResize.isResizing = false;
    ctx_.getState().noteResize.noteIndex = -1;
    ctx_.getState().noteResize.edge = NoteResizeEdge::None;

    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return;

    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime))
        return;

    const double trackRelativeTime = *sourceTime;

    float clickedPitch = ctx_.getViewMapper().yToFreq((float)e.y);

    const int clickedNoteIndex = findNoteIndexAt(notes, *sourceTime, clickedPitch, 100.0f);

    bool isCtrlDown = e.mods.isCtrlDown() || e.mods.isCommandDown();

    int edgeThreshold = 6;
    float mouseMidi = 69.0f + 12.0f * std::log2(clickedPitch / 440.0f) - 0.5f;
    bool isShiftDown = e.mods.isShiftDown();

    for (int noteIndex = 0; noteIndex < static_cast<int>(notes.size()); ++noteIndex) {
        const auto& note = notes[static_cast<size_t>(noteIndex)];
        const int x1 = sourceTimeToScreenX(note.startTime);
        const int x2 = sourceTimeToScreenX(note.endTime);

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

                auto& noteSelection = ctx_.getState().noteSelection;
                const int noteCount = static_cast<int>(notes.size());
                if (!noteSelection.isSelected(noteIndex) && !isCtrlDown && !isShiftDown) {
                    noteSelection.setSingle(noteIndex, noteCount);
                } else {
                    noteSelection.add(noteIndex, noteCount);
                }

                updateF0SelectionFromNotes(notes);
                invalidateNoteChange(ctx_, beforeNotes, notes);
                return;
            }
        }
    }

    if (clickedNoteIndex >= 0) {
        auto& noteSelection = ctx_.getState().noteSelection;
        const int noteCount = static_cast<int>(notes.size());
        if (isCtrlDown) {
            noteSelection.toggle(clickedNoteIndex, noteCount);
        } else if (isShiftDown) {
            int lastSelectedIndex = findLastSelectedNoteIndex(notes);
            if (lastSelectedIndex >= 0 && lastSelectedIndex != clickedNoteIndex) {
                selectNotesBetween(notes, lastSelectedIndex, clickedNoteIndex);
            } else {
                noteSelection.add(clickedNoteIndex, noteCount);
            }
        } else if (!noteSelection.isSelected(clickedNoteIndex)) {
            noteSelection.setSingle(clickedNoteIndex, noteCount);
        }

        updateF0SelectionFromNotes(notes);

        if (noteSelection.isSelected(clickedNoteIndex)) {
            ctx_.getState().noteDrag.draggedNoteIndex = clickedNoteIndex;
            ctx_.getState().noteDrag.draggedNoteIndices = collectSelectedNoteIndices(notes);
            clearNoteDragPreview(ctx_);

            ctx_.setNoteDragManualStartFrame(-1);
            ctx_.setNoteDragManualEndFrameExclusive(-1);
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

                        pitchCurve->renderFinalF0Range(manualRange.startFrame, manualRange.endFrameExclusive,
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

                        ctx_.setNoteDragManualStartFrame(manualRange.startFrame);
                        ctx_.setNoteDragManualEndFrameExclusive(manualRange.endFrameExclusive);

                        auto& manualTargets = ctx_.getNoteDragInitialManualTargets();
                        for (int relIdx = 0; relIdx < rangeSize; ++relIdx) {
                            int f = manualRange.startFrame + relIdx;
                            float v = renderedF0[relIdx];
                            if (v <= 0.0f) continue;
                            manualTargets.push_back({ f, v });
                        }

                        if (manualTargets.empty()) {
                            ctx_.setNoteDragManualStartFrame(-1);
                            ctx_.setNoteDragManualEndFrameExclusive(-1);
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
        int f0Frame = -1;
        if (hitTestF0Curve(e, f0Frame)) {
            beginF0SelectionAt(e, f0Frame);
            return;
        }

        if (!isCtrlDown) {
            deselectAllNotes();
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
// 手绘曲线工具处理：将鼠标位置转换为F0值，在帧间进行对数插值，记录脏区�?
{
    auto pitchCurve = ctx_.getPitchCurve();
    if (!pitchCurve) {
        return;
    }

    const auto dirtyBefore = ctx_.getHandDrawPreviewBounds();

    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return;

    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime)) {
        return;
    }

    const double curveTime = *sourceTime;

    float targetF0 = ctx_.getViewMapper().yToFreq((float)e.y);

    const auto& originalF0 = ctx_.getOriginalF0();
    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) return;
    int frameIndex = f0tl.frameAtOrBefore(*sourceTime);

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
    if (ctx_.repaintPreviewOverlay) ctx_.repaintPreviewOverlay();
}

void PianoRollToolHandler::handleDrawNoteMouseDown(const juce::MouseEvent& e)
// 绘制音符工具鼠标按下处理：检测是否点击已有音符进行选择，设置待拖拽状�?
{
    const auto projection = ctx_.getContentProjection();
    if (!projection.isValid())
        return;

    // Clicking an existing note changes only the editor-local selection model.
    const auto& committedNotes = ctx_.getCommittedNotes();
    float clickedPitch = ctx_.getViewMapper().yToFreq((float)e.y);
    float mouseMidi = 69.0f + 12.0f * std::log2(clickedPitch / 440.0f) - 0.5f;

    int existingNoteIndex = -1;
    for (int noteIndex = 0; noteIndex < static_cast<int>(committedNotes.size()); ++noteIndex) {
        const auto& note = committedNotes[static_cast<size_t>(noteIndex)];
        int x1 = sourceTimeToScreenX(note.startTime);
        int x2 = sourceTimeToScreenX(note.endTime);
        float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f) - 0.5f;
        
        if (e.x >= x1 && e.x <= x2 && std::abs(mouseMidi - noteMidi) < 1.0f) {
            existingNoteIndex = noteIndex;
            break;
        }
    }
    
    if (existingNoteIndex >= 0) {
        const auto beforeNotes = committedNotes;
        bool isCtrlDown = e.mods.isCtrlDown() || e.mods.isCommandDown();
        auto& noteSelection = ctx_.getState().noteSelection;
        const int noteCount = static_cast<int>(committedNotes.size());
        if (isCtrlDown) {
            noteSelection.toggle(existingNoteIndex, noteCount);
        } else {
            if (!noteSelection.isSelected(existingNoteIndex)) {
                noteSelection.setSingle(existingNoteIndex, noteCount);
            }
        }
        updateF0SelectionFromNotes(committedNotes);
        invalidateNoteChange(ctx_, beforeNotes, committedNotes);
    }
    
    ctx_.setDrawNoteToolPendingDrag(true);
    ctx_.setDrawNoteToolMouseDownPos(e.getPosition());
}

void PianoRollToolHandler::handleDrawNoteTool(const juce::MouseEvent& e)
// 绘制音符工具处理：更�?DrawingState 预览状态（不创�?noteDraft），overlay 负责渲染
{
    const auto projection = ctx_.getContentProjection();
    if (!projection.isValid())
        return;

    const auto currentTime = pixelXToSourceTime(e.x);
    if (!currentTime)
        return;

    const auto editRange = sourceEditRange();
    if (!editRange.contains(*currentTime))
        return;

    float targetF0 = ctx_.getViewMapper().yToFreq((float)e.y);
    float midiNote = 69.0f + 12.0f * std::log2(targetF0 / 440.0f);
    int roundedMidi = static_cast<int>(std::round(midiNote));
    float snappedF0 = 440.0f * std::pow(2.0f, (roundedMidi - 69) / 12.0f);

    if (!ctx_.getState().drawing.isDrawingNote) {
        // First drag frame: initialize drawing state
        ctx_.getState().drawing.isDrawingNote = true;
        ctx_.setDrawingNoteStartTime(*currentTime);
        ctx_.setDrawingNoteEndTime(*currentTime);
        ctx_.setDrawingNotePitch(snappedF0);
        ctx_.setDrawingNoteIndex(-1);
    } else {
        // Subsequent drag frames: update end time
        ctx_.setDrawingNoteEndTime(*currentTime);
    }

    // Only repaint the lightweight preview overlay �?no render model rebuild
    if (ctx_.repaintPreviewOverlay) {
        ctx_.repaintPreviewOverlay();
    }
}

void PianoRollToolHandler::handleAutoTuneTool(const juce::MouseEvent& e)
// 自动音调工具处理：触发自动音调生成请�?
{
    juce::ignoreUnused(e);
    ctx_.notifyAutoTuneRequested();
}

void PianoRollToolHandler::handleSelectDrag(const juce::MouseEvent& e)
// 选择工具拖拽处理：框选区域、音符边缘调整、音符拖拽移�?
{
    const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));

    if (ctx_.getState().selection.isSelectingArea) {
        // §8.5 — selection box bounds compared against note.startTime (source time).
        const auto currentTime = pixelXToSourceTime(e.x);
        if (!currentTime)
            return;

        const auto editRange = sourceEditRange();
        if (!editRange.contains(*currentTime))
            return;

        ctx_.getState().selection.selectionEndTime = std::max(0.0, *currentTime);

        float currentMidi = 69.0f + 12.0f * std::log2(ctx_.getViewMapper().yToFreq((float)e.y) / 440.0f) - 0.5f;
        ctx_.getState().selection.selectionEndMidi = currentMidi;

        double selStartTime = std::min(ctx_.getState().selection.selectionStartTime, ctx_.getState().selection.selectionEndTime);
        double selEndTime = std::max(ctx_.getState().selection.selectionStartTime, ctx_.getState().selection.selectionEndTime);
        float selMinMidi = std::min(ctx_.getState().selection.selectionStartMidi, ctx_.getState().selection.selectionEndMidi);
        float selMaxMidi = std::max(ctx_.getState().selection.selectionStartMidi, ctx_.getState().selection.selectionEndMidi);

        const auto& notes = displayNotes(ctx_);
        std::vector<int> selectedIndices;
        selectedIndices.reserve(notes.size());
        for (int noteIndex = 0; noteIndex < static_cast<int>(notes.size()); ++noteIndex) {
            const auto& note = notes[static_cast<size_t>(noteIndex)];
            float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f) - 0.5f;
            bool timeOverlap = (note.endTime > selStartTime && note.startTime < selEndTime);
            bool pitchOverlap = (noteMidi >= selMinMidi - 0.5f && noteMidi <= selMaxMidi + 0.5f);
            if (timeOverlap && pitchOverlap) {
                selectedIndices.push_back(noteIndex);
            }
        }
        ctx_.getState().noteSelection.setFromIndices(std::move(selectedIndices),
                                                     static_cast<int>(notes.size()));
        updateF0SelectionFromNotes(notes);
        invalidateNoteChange(ctx_, beforeNotes, notes);
        return;
    }

    if (ctx_.getState().selection.isSelectingF0) {
        updateF0SelectionDrag(e);
        return;
    }

    if (ctx_.getState().noteResize.isResizing && ctx_.getState().noteResize.noteIndex >= 0) {
        if (!ctx_.getState().noteResize.isDirty) {
            ctx_.getState().noteResize.isDirty = true;
        }
        if (!ctx_.getNoteDraft().active) {
            ctx_.beginNoteDraft();
        }
        ctx_.getNoteDraft().contentDirty = true;

        auto& notes = workingDraftNotes(ctx_);
        resetDraftNotesToBaseline(ctx_);
        if (ctx_.getState().noteResize.noteIndex >= static_cast<int>(notes.size())) {
            return;
        }

        // §8.5 �?Note resize edge writes startTime/endTime in SOURCE time.
        const auto currentTime = pixelXToSourceTime(e.x);
        if (!currentTime)
            return;

        const auto editRange = sourceEditRange();
        if (!editRange.contains(*currentTime))
            return;

        double minDuration = 0.02;

        if (ctx_.getState().noteResize.edge == NoteResizeEdge::Left) {
            double newStart = std::min(*currentTime, notes[static_cast<size_t>(ctx_.getState().noteResize.noteIndex)].endTime - minDuration);
            newStart = std::max(0.0, newStart);
            notes[static_cast<size_t>(ctx_.getState().noteResize.noteIndex)].startTime = newStart;
        } else if (ctx_.getState().noteResize.edge == NoteResizeEdge::Right) {
            double newEnd = std::max(*currentTime, notes[static_cast<size_t>(ctx_.getState().noteResize.noteIndex)].startTime + minDuration);
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
        if (!ctx_.getNoteDraft().active) {
            ctx_.beginNoteDraft();
        }
        ctx_.getNoteDraft().contentDirty = true;

        float startF0 = ctx_.getViewMapper().yToFreq((float)dragStartPos_.y);
        float currentF0 = ctx_.getViewMapper().yToFreq((float)e.y);

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
// 选择工具鼠标释放处理：完成音符拖�?调整/框选，提交音高修正
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
            F0FrameRange editRange;
            if (pitchCurve) {
                if (hasManualTargets) {
                    const int manualStartFrame = ctx_.getNoteDragManualStartFrame();
                    const int manualEndFrameExclusive = ctx_.getNoteDragManualEndFrameExclusive();
                    if (dirtyEndTime > dirtyStartTime) {
                        const auto noteDirtyRange = f0tl.rangeForTimes(dirtyStartTime, dirtyEndTime);
                        editRange.startFrame = std::min(manualStartFrame, noteDirtyRange.startFrame);
                        editRange.endFrameExclusive = std::max(manualEndFrameExclusive, noteDirtyRange.endFrameExclusive);
                    } else {
                        editRange = F0FrameRange{manualStartFrame, manualEndFrameExclusive};
                    }
                } else {
                    editRange = f0tl.rangeForTimes(dirtyStartTime, dirtyEndTime);
                }
            }

            if (hasManualTargets) {
                suppressFinalNoteDraftCommit = true;

                std::vector<ManualOp> ops;
                ManualOp op;
                op.startFrame = ctx_.getNoteDragPreviewStartFrame();
                op.endFrameExclusive = ctx_.getNoteDragPreviewEndFrameExclusive();
                op.f0Data = ctx_.getNoteDragPreviewF0();
                op.source = PitchCorrectionSegment::Source::HandDraw;
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

                // 同步计算修正并一次性提交音符和F0�?
                if (pitchCurve && !editRange.isEmpty()) {
                    commitNoteBasedCorrection(ctx_, notes, pitchCurve, editRange);
                } else {
                    ctx_.commitNoteDraft();
                }
                suppressFinalNoteDraftCommit = true;
            }
        }

        ctx_.getState().noteDrag.draggedNoteIndices.clear();
        ctx_.setNoteDragManualStartFrame(-1);
        ctx_.setNoteDragManualEndFrameExclusive(-1);
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

        // 同步计算修正并一次性提交音符和F0�?
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

    ctx_.getState().selection.isSelectingF0 = false;
    ctx_.getState().selection.f0SelectionAnchorFrame = -1;
    ctx_.getState().noteDrag.draggedNoteIndex = -1;
    ctx_.clearNoteDraft();
    invalidateNoteChange(ctx_, beforeNotes, committedNotes(ctx_));
}

void PianoRollToolHandler::handleDrawCurveUp(const juce::MouseEvent& e)
// 手绘曲线工具鼠标释放处理：将绘制的F0数据提交到音高修正队�?
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
                                  PitchCorrectionSegment::Source::HandDraw);

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
// 绘制音符工具鼠标释放处理：完成音符绘制，分割重叠音符，应用最小时�?
// Option B: noteDraft 仅在 mouseUp 时一次性创建并提交
{
    const auto beforeNotes = std::vector<Note>(committedNotes(ctx_));

    if (ctx_.getDrawNoteToolPendingDrag()) {
        ctx_.setDrawNoteToolPendingDrag(false);
        // Repaint overlay to clear any stale preview
        if (ctx_.repaintPreviewOverlay) ctx_.repaintPreviewOverlay();
        return;
    }
    
    if (!ctx_.getState().drawing.isDrawingNote) {
        return;
    }
    
    ctx_.getState().drawing.isDrawingNote = false;

    const auto editRange = sourceEditRange(0.02);
    // §8.5 — DrawNote release writes endTime in SOURCE time.
    const auto releaseTime = pixelXToSourceTime(e.x);
    if (!releaseTime)
        return;

    ctx_.setDrawingNoteEndTime(*releaseTime);

    double startTime = ctx_.getDrawingNoteStartTime();
    double endTime = *releaseTime;
    editRange.normalizeForCommit(startTime, endTime);

    // One-shot: begin noteDraft from committed notes, build final state, commit
    ctx_.beginNoteDraft();
    auto notes = ctx_.getNoteDraft().workingNotes;  // copy of committed notes

    if (ctx_.getDrawingNotePitch() > 0.0f) {
        std::vector<Note> updatedNotes;
        updatedNotes.reserve(notes.size() + 2);

        for (size_t i = 0; i < notes.size(); ++i) {
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

        double midTime = (startTime + endTime) / 2.0;
        int newSelectedIndex = findNoteIndexAt(notes, midTime, finalNote.getAdjustedPitch(), 100.0f);
        if (newSelectedIndex >= 0) {
            ctx_.getState().noteSelection.setSingle(newSelectedIndex,
                                                    static_cast<int>(notes.size()));
            updateF0SelectionFromNotes(notes);
        }
    }

    ctx_.getNoteDraft().contentDirty = true;
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
    ctx_.showToolSelectionMenu();
}

void PianoRollToolHandler::deleteSelectedNotes(std::vector<Note>& notes)
{
    const auto selectedIndices = collectSelectedNoteIndices(notes);
    if (selectedIndices.empty()) {
        return;
    }

    std::vector<char> deleteMask(notes.size(), 0);
    for (int noteIndex : selectedIndices) {
        if (noteIndex >= 0 && noteIndex < static_cast<int>(deleteMask.size())) {
            deleteMask[static_cast<size_t>(noteIndex)] = 1;
        }
    }

    int noteIndex = 0;
    notes.erase(
        std::remove_if(notes.begin(), notes.end(), [&deleteMask, &noteIndex](const Note&) {
            const bool shouldDelete = deleteMask[static_cast<size_t>(noteIndex)] != 0;
            ++noteIndex;
            return shouldDelete;
        }),
        notes.end()
    );
    ctx_.getState().noteSelection.clear();
    ctx_.getState().selection.clearF0Selection();
}

void PianoRollToolHandler::handleLineAnchorMouseDown(const juce::MouseEvent& e)
// 线锚点工具鼠标按下处理：放置锚点，在锚点间生成线性插值的F0曲线
{
    const auto editRange = sourceEditRange();
    // §8.5 — LineAnchor places anchors at SOURCE time (PitchCurve indexing).
    const auto clickTime = pixelXToSourceTime(e.x);
    if (!clickTime)
        return;

    if (!editRange.contains(*clickTime))
        return;

    float clickFreq = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y));
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

    int clickFrame = f0tl.frameAtOrBefore(*clickTime);

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
        firstAnchor.time = *clickTime;
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

    auto anchorRange = f0tl.nonEmptyRangeForTimes(prev.time, *clickTime);
    const int startFrame = anchorRange.startFrame;
    const int endFrameExclusive = anchorRange.endFrameExclusive;
    const bool previousAnchorIsLeft = prev.time <= *clickTime;
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
                              PitchCorrectionSegment::Source::LineAnchor);

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
    newAnchor.time = *clickTime;
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
    if (ctx_.repaintPreviewOverlay) ctx_.repaintPreviewOverlay();
}

void PianoRollToolHandler::clearLineAnchorPreview()
{
    const auto dirtyBefore = ctx_.getLineAnchorPreviewBounds();
    ctx_.getState().drawing.isPlacingAnchors = false;
    ctx_.getState().drawing.pendingAnchors.clear();
    invalidateIfNeeded(ctx_, dirtyBefore.getUnion(ctx_.getLineAnchorPreviewBounds()));
    if (ctx_.repaintPreviewOverlay) ctx_.repaintPreviewOverlay();
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
    auto& selection = ctx_.getState().noteSelection;
    selection.trimToNoteCount(static_cast<int>(notes.size()));
    return selection.selectedIndices;
}

void PianoRollToolHandler::deselectAllNotes()
{
    ctx_.getState().noteSelection.clear();
}

void PianoRollToolHandler::selectAllNotes(const std::vector<Note>& notes)
{
    ctx_.getState().noteSelection.selectAll(static_cast<int>(notes.size()));
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

void PianoRollToolHandler::selectNotesBetween(const std::vector<Note>& notes, int startIndex, int endIndex)
{
    ctx_.getState().noteSelection.selectRange(startIndex, endIndex, notes);
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
// vocal-time-stretch §8.4 (Phase F) �?Time tool handlers
//
// Minimal scaffolding: hover detection + selection + drag + commit.
// Phase G will add: double-click-insert, delete-handle, Alt-snap-disable,
// group multi-handle drag, and output spacing clamp.
// ============================================================================

uint64_t PianoRollToolHandler::hitTestTimeGridHandle(const juce::MouseEvent& e) const
{
    if (!ctx_.getTimeGridSnapshot) return 0;
    auto snap = ctx_.getTimeGridSnapshot();
    if (snap == nullptr) return 0;

    constexpr int kHitToleranceX = 5;  // pixels

    // Use uint64_t throughout; handle ids are stable opaque values.
    uint64_t closestId = 0;
    int closestDistance = std::numeric_limits<int>::max();
    for (const auto& h : snap->handles()) {
        if (h.locked) continue;   // endpoints not selectable

        // output_seconds �?timeline via projectContentTimeToTimeline �?screen X via timeToX.
        // Uses the same active projection as drawTimeGridHandles for consistent hit-testing.
        const auto projection = ctx_.getContentProjection();
        if (!projection.isValid()) continue;
        const int handleX = ctx_.getViewMapper().timeToX(
            projection.projectContentTimeToTimeline(h.output_seconds));
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

    if (prevHovered != hovered && ctx_.repaintTimeGridHandles) {
        ctx_.repaintTimeGridHandles();
    }
}

void PianoRollToolHandler::handleTimeToolMouseDown(const juce::MouseEvent& e)
{
    auto& tt = ctx_.getState().timeTool;
    const uint64_t hitId = hitTestTimeGridHandle(e);

    if (hitId == 0) {
        // Clicked empty space �?seek playhead + clear selection.
        // §8.4 (Phase I): TimeTool 下主区空白点击现在也会重定位播放头，
        // 与标尺区点击行为一致，消除"点击无响�?的用户困惑�?
        // §8.4 (Phase I bugfix): playhead seek must use TIMELINE time
        // (host-absolute), NOT content-local time.  As a general
        // rule, everything that's "seek/play/pause/transport" operates in
        // timeline time; everything that's "edit handle/grid" operates in
        // content-local output time.
        const double timelineTime = ctx_.getViewMapper().xToTime(e.x);
        if (timelineTime >= 0.0 && ctx_.notifyPlayheadChange) {
            ctx_.notifyPlayheadChange(timelineTime);
        }
        if (!e.mods.isShiftDown()) {
            if ((tt.selectedHandleId != 0 || !tt.additionalSelectedIds.empty())
                && ctx_.repaintTimeGridHandles) {
                ctx_.repaintTimeGridHandles();
            }
            tt.selectedHandleId = 0;
            tt.additionalSelectedIds.clear();
        }
        tt.isDraggingHandle = false;
        return;
    }

    // Found a handle �?select + arm drag.
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

    // ⚡️ §8.4 (Phase H) �?Shift+click toggles in additionalSelectedIds
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

    // §8.4 (Phase I): 命中 handle 后先进入 pending 状态�?
    // mouseDrag 越过阈值后才转为真正拖拽，防止轻微抖动触发 undo�?
    tt.dragPending = true;
    tt.isDraggingHandle = false;
    tt.draggedHandleId = hitId;
    tt.dragSnapDisabled = e.mods.isAltDown();   // Phase H: Alt disables clamp
    tt.dragOriginalSnapshot = snap;
    tt.dragWorkingSnapshot = snap;   // identity at drag start
    tt.dragStartOutputSeconds = hitHandle->output_seconds;
    tt.dragStartPixel = e.getPosition();

    if (ctx_.repaintTimeGridHandles) ctx_.repaintTimeGridHandles();
}

void PianoRollToolHandler::handleTimeToolMouseDrag(const juce::MouseEvent& e)
{
    auto& tt = ctx_.getState().timeTool;

    // §8.4 (Phase I): 检�?dragPending 阈值�?
    // Time handles only move horizontally; use X-axis-only threshold so
    // vertical jitter does not start a drag that produces an identical output.
    if (tt.dragPending) {
        const int dx = std::abs(e.x - tt.dragStartPixel.x);
        constexpr int kHandleDragThreshold = 6;
        if (dx <= kHandleDragThreshold) {
            return;   // 尚未越过阈值，保持 pending
        }
        tt.dragPending = false;
        tt.isDraggingHandle = true;
    }

    if (!tt.isDraggingHandle || tt.dragOriginalSnapshot == nullptr) return;

    // pixel �?timeline time �?content-local output time
    const double timelineTime = ctx_.getViewMapper().xToTime(e.x);
    const auto projection = ctx_.getContentProjection();
    if (!projection.isValid()) return;
    const double newOutputTime = projection.projectTimelineTimeToContent(timelineTime);
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

    // ⚡️ §8.4 (Phase H) �?group-drag detection.
    // If the user has multi-selected handles AND the dragged handle is part
    // of that selection, every selected handle moves by the same delta
    // (uniformDelta).  Otherwise only the dragged handle moves.
    const bool isGroupDrag = (!tt.additionalSelectedIds.empty())
                              && tt.isSelected(tt.draggedHandleId);

    // Phase G/H spacing rule between adjacent outputs.
    // Alt held at drag start (`dragSnapDisabled`) bypasses the clamp for
    // power-user nudging into tight regions.
    const double kMinSpacingSec = tt.dragSnapDisabled ? 0.000 : TimeGridSnapshot::kMinOutputSpacingSeconds;
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
                // Region too tight �?abort the drag step (keep last valid
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
    if (ctx_.repaintTimeGridHandles) ctx_.repaintTimeGridHandles();
}

void PianoRollToolHandler::handleTimeToolMouseUp(const juce::MouseEvent& /*e*/)
{
    auto& tt = ctx_.getState().timeTool;

    // §8.4 (Phase I): 如果从未越过拖动阈值，仅保�?selection 不提交�?
    if (tt.dragPending) {
        tt.dragPending = false;
        tt.dragOriginalSnapshot.reset();
        tt.dragWorkingSnapshot.reset();
        return;
    }

    if (!tt.isDraggingHandle) return;

    if (tt.dragWorkingSnapshot != nullptr
        && tt.dragWorkingSnapshot != tt.dragOriginalSnapshot
        && ctx_.commitTimeGrid) {
        ctx_.commitTimeGrid(tt.dragWorkingSnapshot,
                             tt.dragOriginalSnapshot,
                             juce::String("拖动时间手柄"));
    }

    tt.isDraggingHandle = false;
    tt.draggedHandleId = 0;
    tt.dragOriginalSnapshot.reset();
    tt.dragWorkingSnapshot.reset();
}

// ============================================================================
// §8.4 (Phase G) �?Double-click to insert UserAdded handle
//
// Constraints (per spec time-tool-interaction.md):
//   - Click must be on empty area (no existing handle within ±5 px)
//   - Click position must satisfy TimeGrid output/source spacing
//   - source_seconds initially equals output_seconds (identity insertion);
//     subsequent drag operations modify only output_seconds
// ============================================================================
void PianoRollToolHandler::handleTimeToolMouseDoubleClick(const juce::MouseEvent& e)
{
    if (!ctx_.getTimeGridSnapshot || !ctx_.commitTimeGrid) return;
    auto snap = ctx_.getTimeGridSnapshot();
    if (snap == nullptr) return;

    const double timelineTime = ctx_.getViewMapper().xToTime(e.x);
    const auto projection = ctx_.getContentProjection();
    if (!projection.isValid()) return;
    const double clickedTime = projection.projectTimelineTimeToContent(timelineTime);
    if (clickedTime <= 0.0) return;

    const auto& handles = snap->handles();
    if (handles.size() < 2) return;

    // Reject if too close to existing handle in output or source time.
    //
    // § Phase I bugfix: clickedTime is output/content time.
    // Source spacing check must compare against source_seconds, so
    // compute clickedOutput �?clickedSource via tauInverse.
    // Identity grid �?tauInverse is identity �?same value.
    const double clickedSourceSeconds = snap->tauInverse(clickedTime);
    for (const auto& h : handles) {
        if (std::abs(h.output_seconds - clickedTime) < TimeGridSnapshot::kMinOutputSpacingSeconds) {
            AppLogger::log("[TimeTool] insert rejected: output spacing invariant");
            return;
        }
        const double previous = std::min(h.source_seconds, clickedSourceSeconds);
        const double current = std::max(h.source_seconds, clickedSourceSeconds);
        if (!TimeGridSnapshot::hasMinimumSourceSpacing(previous, current)) {
            AppLogger::log("[TimeTool] insert rejected: source spacing invariant");
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
    // For non-identity TimeGrid, map output (display) time back to source
    // time via tauInverse.  Identity grid �?tauInverse is identity.
    newHandle.source_seconds = snap->tauInverse(clickedTime);
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

    ctx_.commitTimeGrid(newSnap, snap, juce::String("插入时间手柄"));

    // Auto-select the newly-inserted handle so user can immediately drag.
    // commitTimeGrid already triggers cache dirty internally.
    auto& tt = ctx_.getState().timeTool;
    tt.selectedHandleId = newHandle.id;
}

// ============================================================================
// §8.4 (Phase G) �?Delete key removes selected handle (non-endpoint only)
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
        // Endpoint or not found �?cannot delete
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

    ctx_.commitTimeGrid(newSnap, snap, juce::String("删除时间手柄"));

    tt.selectedHandleId = 0;
    tt.hoveredHandleId  = 0;
    // commitTimeGrid already triggers cache dirty internally.
    return true;
}

} // namespace OpenTune
