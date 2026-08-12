#include "PianoRollToolHandler.h"
#include "../../../Utils/AudioEditingScheme.h"
#include "../../../Utils/AppLogger.h"
#include "../../../Utils/KeyShortcutConfig.h"
#include "../../../Utils/ScissorsUndoAction.h"
#include "../../../Utils/PitchUtils.h"
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

// ============================================================================
// buildNoteBasedCorrectionState — note-based correction 唯一状态构建器
//
// 实时预览（updateNoteBasedCorrectionPreview）与提交（commitNoteBasedCorrection）
// 共用：复制当前 EditableContentSnapshot，替换 notes 与 clone 后经权威
// applyCorrectionToRange（含全局 pitch-shift 比例）烘焙的 pitchCurve。
// ============================================================================
std::shared_ptr<const EditableContentSnapshot> buildNoteBasedCorrectionState(
    PianoRollToolHandler::Context& ctx,
    const std::vector<Note>& notes,
    const std::shared_ptr<PitchCurve>& pitchCurve,
    F0FrameRange editRange)
{
    const auto contentSnapshot = ctx.getEditableContentSnapshot();
    if (contentSnapshot == nullptr || pitchCurve == nullptr)
        return nullptr;

    auto snap = std::make_shared<EditableContentSnapshot>(*contentSnapshot);
    snap->notes = notes;

    auto clonedCurve = pitchCurve->clone();
    clonedCurve->applyCorrectionToRange(notes,
                                        editRange.startFrame,
                                        editRange.endFrameExclusive,
                                        static_cast<float>(contentSnapshot->pitchShiftSettings.getPitchRatio()),
                                        ctx.getRetuneSpeed(),
                                        ctx.getVibratoDepth(),
                                        ctx.getVibratoRate());
    snap->pitchCurve = std::move(clonedCurve);
    return snap;
}

bool commitNoteBasedCorrection(PianoRollToolHandler::Context& ctx,
                               const std::vector<Note>& notes,
                               const std::shared_ptr<PitchCurve>& pitchCurve,
                               F0FrameRange editRange)
{
    const auto f0tl = ctx.getF0Timeline();
    const auto snap = buildNoteBasedCorrectionState(ctx, notes, pitchCurve, editRange);
    if (snap == nullptr)
        return false;

    const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(editRange.startFrame,
                                                                          editRange.endFrameExclusive,
                                                                          f0tl.endFrameExclusive());

    // Extract segments overlapping the affected range (range-scoped, not full)
    auto allSegments = snap->pitchCurve->getSnapshot()->getCorrectionSegments();
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

const std::vector<Note>& draftBaselineNotes(const PianoRollToolHandler::Context& ctx)
{
    return ctx.getNoteDraft().baselineNotes;
}

}

// ============================================================================
// PianoRollToolHandler - 閽㈢惔鍗峰笜宸ュ叿澶勭悊鍣ㄥ疄锟?
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
    // Pipeline: pixelX 鈫?timeline 鈫?output(content) 鈫?tauInverse 鈫?source.
    // invalid projection 鎰忓懗鐫€娌℃湁 edit target锛岃繑鍥?nullopt銆?
    // 一旦 projection valid，TimeGrid 必须参与 output/source 转换，
    // identity TimeGrid 的 tauInverse 本身就是 identity，无需特判。
    const auto projection = ctx_.getContentProjection();
    if (!projection.isValid())
        return std::nullopt;

    const auto grid = ctx_.getActiveContentTimeGrid();
    jassert(grid != nullptr);

    const double timelineSeconds = ctx_.getViewMapper().xToTime(pixelX);
    const double outputSeconds = projection.projectTimelineTimeToContent(timelineSeconds);
    return grid->tauInverse(outputSeconds);
}

double PianoRollToolHandler::sourceTimeToTimelineTime(double sourceSeconds) const
{
    // Pipeline: source 鈫?tauForward 鈫?output(content) 鈫?timeline.
    // projection valid 是调用方契约（入口已检查）。
    const auto projection = ctx_.getContentProjection();
    jassert(projection.isValid());

    const auto grid = ctx_.getActiveContentTimeGrid();
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
    const auto grid = ctx_.getActiveContentTimeGrid();
    jassert(grid != nullptr);
    return SourceEditRange::fromTimeGrid(*grid, minDurationSeconds);
}

void PianoRollToolHandler::mouseMove(const juce::MouseEvent& e)
// 鼠标移动处理：更新光标形状（音符边缘调整、线锚点预览）
{
    if (e.mods.isCtrlDown()) {
        ctx_.setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }

    if (currentTool_ == ToolId::LineAnchor && ctx_.getState().drawing.isPlacingAnchors) {
        const auto dirtyBefore = ctx_.getLineAnchorPreviewBounds();
        ctx_.getState().drawing.currentMousePos = e.position;
        if (ctx_.invalidateInteractionPreview) ctx_.invalidateInteractionPreview(dirtyBefore.getUnion(ctx_.getLineAnchorPreviewBounds()));
        return;
    }

    if (currentTool_ == ToolId::TimeTool) {
        handleTimeToolMouseMove(e);
        return;
    }

    if (currentTool_ == ToolId::Scissors) {
        updateScissorsPreview(e);
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
    float mousePitch = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));
    float mouseMidiVal = ctx_.getViewMapper().freqToMidi(mousePitch);

    for (const auto& note : displayNotes(ctx_)) {
        const int x1 = sourceTimeToScreenX(note.startTime);
        const int x2 = sourceTimeToScreenX(note.endTime);
        
        bool nearLeft = std::abs(e.x - x1) <= edgeThreshold;
        bool nearRight = std::abs(e.x - x2) <= edgeThreshold;
        
        float noteMidi = ctx_.getViewMapper().freqToMidi(note.getAdjustedPitch());
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
            if (ctx_.invalidateInteractionPreview) ctx_.invalidateInteractionPreview(dirtyBefore.getUnion(ctx_.getLineAnchorPreviewBounds()));
            return;
        }
        showToolContextMenu(e);
        return;
    }

    const int rulerHeight = ctx_.contentOriginY;
    constexpr int timelineExtendedHitArea = 20;
    const int timelineBottomExtended = rulerHeight + timelineExtendedHitArea;
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
            // OpenDyne（NotesPrimary）：DrawNote 新建音符路径不进入。
            if (!AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme())) {
                handleDrawNoteMouseDown(e);
            }
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
        case ToolId::Pitch:
        case ToolId::PitchModulation:
        case ToolId::PitchDrift:
            handlePitchToolMouseDown(e);
            break;
        case ToolId::VolumeEnvelope:
            handleVolumeEnvelopeToolMouseDown(e);
            break;
        case ToolId::Scissors:
            handleScissorsToolMouseDown(e);
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

    // 框选进行中（含 Pitch 工具空区拖拽启动的框选），统一走 Select drag 处理，
    // 确保框选终点持续跟随鼠标。
    if (ctx_.getState().selection.isSelectingArea) {
        handleSelectDrag(e);
        return;
    }

    switch (currentTool_) {
        case ToolId::Select:
            handleSelectDrag(e);
            break;
        case ToolId::DrawNote:
            // OpenDyne（NotesPrimary）：DrawNote 新建音符路径不进入。
            if (!AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme())) {
                handleDrawNoteDrag(e);
            }
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
        case ToolId::Pitch:
        case ToolId::PitchModulation:
        case ToolId::PitchDrift:
            dragNotePitch(e);
            break;
        case ToolId::VolumeEnvelope:
            handleVolumeEnvelopeToolDrag(e);
            break;
        case ToolId::Scissors:
            updateScissorsPreview(e);
            break;
        default:
            break;
    }
}

void PianoRollToolHandler::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (currentTool_ == ToolId::TimeTool) {
        handleTimeToolMouseDoubleClick(e);
    } else if ((currentTool_ == ToolId::Pitch || currentTool_ == ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift)
               && AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme())) {
        handlePitchToolDoubleClick(e);
    } else if (currentTool_ == ToolId::VolumeEnvelope
               && AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme())) {
        handleVolumeEnvelopeToolDoubleClick(e);
    } else if (currentTool_ == ToolId::Scissors) {
        // 双击切割：复用 mouseUp 的提交逻辑，与 Melodyne 双击交互一致。
        handleScissorsToolUp(e);
    }
    // Other tools: no-op (could be extended later for note resize / etc.)
}

void PianoRollToolHandler::mouseUp(const juce::MouseEvent& e)
{
    if (consumeEmptySpaceIntentUp(e)) {
        return;
    }

    // 框选完成时（含 Pitch 工具空区拖拽启动的框选），统一走 Select up 处理提交选择。
    if (ctx_.getState().selection.isSelectingArea) {
        handleSelectUp(e);
        return;
    }

    switch (currentTool_) {
        case ToolId::Select:
            handleSelectUp(e);
            break;
        case ToolId::HandDraw:
            handleDrawCurveUp(e);
            break;
        case ToolId::DrawNote:
            // OpenDyne（NotesPrimary）：DrawNote 新建音符路径不进入。
            if (!AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme())) {
                handleDrawNoteUp(e);
            }
            break;
        case ToolId::TimeTool:
            handleTimeToolMouseUp(e);
            break;
        case ToolId::Pitch:
        case ToolId::PitchModulation:
        case ToolId::PitchDrift:
            handlePitchToolMouseUp(e);
            break;
        case ToolId::VolumeEnvelope:
            handleVolumeEnvelopeToolUp(e);
            break;
        case ToolId::Scissors:
            handleScissorsToolUp(e);
            break;
        default:
            ctx_.getState().noteDrag.draggedNoteIndex = -1;
            break;
    }
}

bool PianoRollToolHandler::keyPressed(const juce::KeyPress& key)
{
    const auto& shortcutSettings = ctx_.getShortcutSettings();

    // OpenDyne（NotesPrimary）F 键固定映射：F1=Select、F2=Pitch、F4=VolumeEnvelope、
    // F6=Scissors。不新增 ShortcutId、不进 KeyShortcutConfig。T 继续走现有 ToolTimeTool。
    const bool isOpenDyne = AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme());
    if (isOpenDyne) {
        if (key.getKeyCode() == juce::KeyPress::F1Key) {
            ctx_.setCurrentTool(ToolId::Select);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::F2Key) {
            // Melodyne-style F2 cycling: F2×1=Pitch, F2×2=Modulation, F2×3=Drift
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastF2PressTime_).count();
            if (elapsed < kF2DoubleClickMs && f2PressCount_ > 0) {
                f2PressCount_ = (f2PressCount_ % 3) + 1;
            } else {
                f2PressCount_ = 1;
            }
            lastF2PressTime_ = now;
            switch (f2PressCount_) {
                case 1: ctx_.setCurrentTool(ToolId::Pitch); break;
                case 2: ctx_.setCurrentTool(ToolId::PitchModulation); break;
                case 3: ctx_.setCurrentTool(ToolId::PitchDrift); break;
            }
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::F4Key) {
            ctx_.setCurrentTool(ToolId::VolumeEnvelope);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::F6Key) {
            ctx_.setCurrentTool(ToolId::Scissors);
            return true;
        }
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::SelectAll, key)) {
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
        if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
        return true;
    }

    // ==== Tool switching (via configurable shortcuts) ====
    // OpenTune（PitchPrimary）工具切换走可配置快捷键；OpenDyne（NotesPrimary）用固定 F1/F2/F4/F6，
    // 这四个可配置入口（ToolDrawNote/ToolSelect/ToolLineAnchor/ToolHandDraw）只在 OpenTune 分支生效，
    // 绝不穿透到 OpenDyne。ToolTimeTool/ToolAutoTune 与通用 SelectAll/Delete/CancelSelection 两 scheme 共用。
    if (!isOpenDyne) {
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

    // TimeTool 的 mouseDown 自己处理 hit-test handle / 空区两种语义。
    // 不能被空区意图捕获，否则 handleTimeToolMouseDown 永不触发，handle 无法拖动。
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

    const float clickedPitch = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));
    const float mouseMidi = ctx_.getViewMapper().freqToMidi(clickedPitch);
    constexpr int edgeThreshold = 6;

    for (const auto& note : displayNotes(ctx_)) {
        const float noteMidi = ctx_.getViewMapper().freqToMidi(note.getAdjustedPitch());
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
    if (snapshot->isEmpty()) {
        return false;
    }
    const auto contentSnapshot = ctx_.getEditableContentSnapshot();
    if (contentSnapshot == nullptr)
        return false;

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

    constexpr float kHitTolerancePx = 7.0f;
    float bestDistanceSquared = kHitTolerancePx * kHitTolerancePx;
    int bestFrame = -1;

    const auto& viewMapper = ctx_.getViewMapper();

    auto testCandidate = [&](int frame, float frequency) {
        if (frequency <= 0.0f) {
            return;
        }

        const int x = sourceTimeToScreenX(f0tl.timeAtFrame(frame));
        const float y = viewMapper.freqToY(frequency);
        const float dx = static_cast<float>(e.x - x);
        const float contentY = static_cast<float>(e.y - ctx_.contentOriginY);
        const float dy = contentY - y;
        const float distanceSquared = dx * dx + dy * dy;
        if (distanceSquared <= bestDistanceSquared) {
            bestDistanceSquared = distanceSquared;
            bestFrame = frame;
        }
    };

    contentSnapshot->forEachEffectiveF0Span(startFrame, endFrameExclusive,
        [&](int spanStart, const float* data, int length, float gain) {
            for (int i = 0; i < length; ++i) {
                const int frame = spanStart + i;
                testCandidate(frame, originalF0[static_cast<size_t>(frame)]);
                testCandidate(frame, data[i] * gain);
            }
        });

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
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
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

    const int frame = juce::jlimit(0, f0tl.endFrameExclusive() - 1, f0tl.frameAtOrBefore(*sourceTime));
    const int startFrame = std::min(selection.f0SelectionAnchorFrame, frame);
    const int endFrameExclusive = std::max(selection.f0SelectionAnchorFrame, frame) + 1;
    selection.setF0Range(startFrame, endFrameExclusive);
    selection.isSelectingF0 = true;
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
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
    const int threshold = ctx_.getDragThreshold();
    if (dx * dx + dy * dy <= threshold * threshold) {
        return true;
    }

    const auto startEvent = eventAtEmptySpaceMouseDown(e);
    const ToolId tool = intent.tool;
    intent.clear();

    switch (tool) {
        case ToolId::Select:
        case ToolId::Pitch:
        case ToolId::PitchModulation:
        case ToolId::PitchDrift:
            beginAreaSelection(startEvent);
            handleSelectDrag(e);
            return true;
        case ToolId::DrawNote:
            // OpenDyne（NotesPrimary）：DrawNote 新建音符路径不进入。
            if (!AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme())) {
                handleDrawNoteMouseDown(startEvent);
                handleDrawNoteDrag(e);
            }
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
    const int threshold = ctx_.getDragThreshold();
    if (dx * dx + dy * dy > threshold * threshold) {
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
                // OpenDyne（NotesPrimary）：DrawNote 新建音符路径不进入。
                if (!AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme())) {
                    handleDrawNoteUp(e);
                }
                break;
            case ToolId::Pitch:
            case ToolId::PitchModulation:
            case ToolId::PitchDrift:
                handleSelectUp(e);
                break;
            default:
                break;
        }
        return true;
    }

    // 纯单击空白：取消当前选中 + 移动播放头
    deselectAllNotes();
    updateF0SelectionFromNotes(committedNotes(ctx_));

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
    state.resetTransient();
    ctx_.clearNoteDraft();
}

void PianoRollToolHandler::handleDeleteKey()
// 删除键处理：只删除选中的音符并清除对应音高修正；无选中则不删除
{
    const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));
    ctx_.beginNoteDraft();
    auto& notes = workingDraftNotes(ctx_);
    const auto selectedIndices = collectSelectedNoteIndices(notes);

    if (selectedIndices.empty()) {
        ctx_.clearNoteDraft();
        return;
    }

    int globalDirtyStartFrame = INT_MAX;
    int globalDirtyEndFrame = INT_MIN;

    auto curve = ctx_.getPitchCurve();
    const auto f0tl = ctx_.getF0Timeline();

    // 每个选中音符独立计算清除范围：不连续选中时，中间未选中音符的修正必须保留
    // 本地累积需要清除的修正范围，不立即提交，最后一次性与音符原子提交
    std::vector<F0FrameRange> correctionClearRanges;
    if (curve) {
        for (int noteIndex : selectedIndices) {
            const auto& note = notes[static_cast<size_t>(noteIndex)];
            const auto noteRange = f0tl.rangeForTimes(note.startTime, note.endTime);
            if (noteRange.isEmpty())
                continue;
            correctionClearRanges.push_back(noteRange);
            globalDirtyStartFrame = std::min(globalDirtyStartFrame, noteRange.startFrame);
            globalDirtyEndFrame = std::max(globalDirtyEndFrame, noteRange.endFrameExclusive - 1);
        }

        // 按 startFrame 排序并合并重叠/相邻范围，避免同一帧重复清除
        std::sort(correctionClearRanges.begin(), correctionClearRanges.end(),
                  [](const F0FrameRange& a, const F0FrameRange& b) { return a.startFrame < b.startFrame; });
        std::vector<F0FrameRange> mergedRanges;
        for (const auto& range : correctionClearRanges) {
            if (!mergedRanges.empty() && range.startFrame <= mergedRanges.back().endFrameExclusive) {
                mergedRanges.back().endFrameExclusive =
                    std::max(mergedRanges.back().endFrameExclusive, range.endFrameExclusive);
            } else {
                mergedRanges.push_back(range);
            }
        }
        correctionClearRanges = std::move(mergedRanges);
    }

    deleteSelectedNotes(notes);
    // 删除后清除残留的框选区域标记（与旧区域删除行为一致）
    ctx_.getState().selection.hasSelectionArea = false;

    ctx_.getNoteDraft().contentDirty = true;
    ctx_.getNoteDraft().workingNotes = notes;
    ctx_.setUndoDescription(juce::String::fromUTF8(u8"删除音符"));

    // 同步计算清除修正 + 删除音符，一次性原子提交
    bool committed = false;
    if (curve && !correctionClearRanges.empty()) {
        auto clonedCurve = curve->clone();
        for (const auto& range : correctionClearRanges) {
            clonedCurve->clearCorrectionRange(range.startFrame, range.endFrameExclusive);
        }
        auto snap = clonedCurve->getSnapshot();
        // delete 路径：affectedRange = globalDirty*Frame 的覆盖范围（含端点）。
        // F0FrameRange 的 endFrameExclusive 语义。
        const F0FrameRange affectedRange{globalDirtyStartFrame, globalDirtyEndFrame + 1};

        // Extract segments overlapping the affected range (range-scoped, not full)
        auto allSegments = snap->getCorrectionSegments();
        std::vector<PitchCorrectionSegment> segmentsInRange;
        for (const auto& seg : allSegments) {
            if (seg.startFrame < affectedRange.endFrameExclusive && seg.endFrame > affectedRange.startFrame)
                segmentsInRange.push_back(seg);
        }

        auto commitSnap = ctx_.commitNotesAndSegments(notes, segmentsInRange, affectedRange);
        committed = (commitSnap != nullptr);
        if (committed) {
            ctx_.notifyPitchCurveEdited(globalDirtyStartFrame, globalDirtyEndFrame);
        }
    } else {
        committed = ctx_.commitNoteDraft();
    }

    if (committed) {
        if (ctx_.invalidateLiveNotes) ctx_.invalidateLiveNotes(beforeNotes, committedNotes(ctx_));
        return;
    }

    ctx_.clearNoteDraft();
}

void PianoRollToolHandler::handleSelectTool(const juce::MouseEvent& e)
// 选择工具鼠标按下处理：检测音符边缘调整、音符选中/取消选中、框选区域开始
{
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

    float clickedPitch = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));

    const int clickedNoteIndex = findNoteIndexAt(notes, *sourceTime, clickedPitch, 1.0f);

    bool isCtrlDown = e.mods.isCtrlDown() || e.mods.isCommandDown();

    bool isOpenDyne = AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme());
    int edgeThreshold = 6;
    float mouseMidi = ctx_.getViewMapper().freqToMidi(clickedPitch);
    bool isShiftDown = e.mods.isShiftDown();

    for (int noteIndex = 0; noteIndex < static_cast<int>(notes.size()); ++noteIndex) {
        const auto& note = notes[static_cast<size_t>(noteIndex)];
        const int x1 = sourceTimeToScreenX(note.startTime);
        const int x2 = sourceTimeToScreenX(note.endTime);

        bool nearLeft = std::abs(e.x - x1) <= edgeThreshold;
        bool nearRight = std::abs(e.x - x2) <= edgeThreshold;

        if (nearLeft || nearRight) {
            float noteMidi = ctx_.getViewMapper().freqToMidi(note.getAdjustedPitch());
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
                if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
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
        } else if (!noteSelection.isSelected(clickedNoteIndex) || (isOpenDyne && noteSelection.isAllSelected(noteCount))) {
            noteSelection.setSingle(clickedNoteIndex, noteCount);
        }

        updateF0SelectionFromNotes(notes);

        if (noteSelection.isSelected(clickedNoteIndex)) {
            // OpenDyne Main/Select 与普通模式一致：允许对选中音符进行 pitch 拖拽。
            beginNotePitchDrag(clickedNoteIndex, notes);
        } else {
            // Note was not selected - start selection area or clear
            if (!isCtrlDown) {
                ctx_.getState().noteDrag.draggedNoteIndex = -1;
                ctx_.getState().noteDrag.draggedNoteIndices.clear();
                ctx_.getState().noteDrag.previewSnapshot.reset();
            }
        }

        if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
    } else {
        int f0Frame = -1;
        if (hitTestF0Curve(e, f0Frame)) {
            beginF0SelectionAt(e, f0Frame);
            return;
        }
        beginAreaSelection(e);
    }
}

void PianoRollToolHandler::beginAreaSelection(const juce::MouseEvent& e)
{
    if (e.mods.isCtrlDown() || e.mods.isCommandDown()) {
        ctx_.clearNoteDraft();
        return;
    }

    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return;

    deselectAllNotes();
    updateF0SelectionFromNotes(committedNotes(ctx_));
    ctx_.getState().noteDrag.draggedNoteIndex = -1;
    ctx_.getState().noteDrag.draggedNoteIndices.clear();
    ctx_.getState().noteDrag.previewSnapshot.reset();
    ctx_.getState().noteResize.isResizing = false;
    ctx_.getState().noteResize.noteIndex = -1;
    ctx_.getState().noteResize.edge = NoteResizeEdge::None;

    if (e.x > ctx_.getPianoKeyWidth()) {
        ctx_.getState().selection.isSelectingArea = true;
        ctx_.getState().selection.hasSelectionArea = true;
        ctx_.getState().selection.selectionStartTime = std::max(0.0, *sourceTime);
        ctx_.getState().selection.selectionEndTime = ctx_.getState().selection.selectionStartTime;
        float midiVal = ctx_.getViewMapper().freqToMidi(ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY)));
        ctx_.getState().selection.selectionStartMidi = midiVal;
        ctx_.getState().selection.selectionEndMidi = midiVal;
    } else {
        ctx_.getState().selection.isSelectingArea = false;
        ctx_.getState().selection.hasSelectionArea = false;
    }
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
}

void PianoRollToolHandler::handleDrawCurveTool(const juce::MouseEvent& e)
// 手绘曲线工具处理：将鼠标位置转换为F0值，在帧间进行对数插值，记录脏区
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
    const double curveTime = juce::jlimit(editRange.startSeconds,
                                           editRange.endSeconds,
                                           *sourceTime);

    float targetF0 = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));

    const auto& originalF0 = ctx_.getOriginalF0();
    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) return;
    const int frameIndex = juce::jlimit(0, f0tl.endFrameExclusive() - 1, f0tl.frameAtOrBefore(curveTime));

    const auto scheme = ctx_.getAudioEditingScheme();

    if (!ctx_.getState().drawing.isDrawingF0) {
        ctx_.getState().drawing.isDrawingF0 = true;
        ctx_.setDirtyStartTime(-1.0);
        ctx_.setDirtyEndTime(-1.0);
        lastDrawTime_ = curveTime;
        lastDrawF0_ = targetF0;

        auto& handDrawBuffer = ctx_.getState().drawing.handDrawBuffer;
        handDrawBuffer.clear();
        handDrawBuffer.resize(originalF0.size(), -1.0f);
        
    }

    auto& handDrawBuffer = ctx_.getState().drawing.handDrawBuffer;
    
    const double lastTime = lastDrawTime_;

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

    const int lastFrame = juce::jlimit(0, f0tl.endFrameExclusive() - 1, f0tl.frameAtOrBefore(lastTime));
    const float lastF0 = lastDrawF0_;
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

    lastDrawTime_ = curveTime;
    lastDrawF0_ = targetF0;
    if (ctx_.invalidateInteractionPreview)
        ctx_.invalidateInteractionPreview(dirtyBefore.getUnion(ctx_.getHandDrawPreviewBounds()));
}

void PianoRollToolHandler::handleDrawNoteMouseDown(const juce::MouseEvent& e)
// 绘制音符工具鼠标按下处理：检测是否点击已有音符进行选择，设置待拖拽状态
{
    const auto projection = ctx_.getContentProjection();
    if (!projection.isValid())
        return;

    // Clicking an existing note changes only the editor-local selection model.
    const auto& committedNotes = ctx_.getCommittedNotes();
    float clickedPitch = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));
    float mouseMidi = ctx_.getViewMapper().freqToMidi(clickedPitch);

    int existingNoteIndex = -1;
    for (int noteIndex = 0; noteIndex < static_cast<int>(committedNotes.size()); ++noteIndex) {
        const auto& note = committedNotes[static_cast<size_t>(noteIndex)];
        int x1 = sourceTimeToScreenX(note.startTime);
        int x2 = sourceTimeToScreenX(note.endTime);
        float noteMidi = ctx_.getViewMapper().freqToMidi(note.getAdjustedPitch());
        
        if (e.x >= x1 && e.x <= x2 && std::abs(mouseMidi - noteMidi) < 1.0f) {
            existingNoteIndex = noteIndex;
            break;
        }
    }
    
    if (existingNoteIndex >= 0) {
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
        if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
    }
    
    ctx_.setDrawNoteToolPendingDrag(true);
    ctx_.setDrawNoteToolMouseDownPos(e.getPosition());
}

void PianoRollToolHandler::handleDrawNoteTool(const juce::MouseEvent& e)
// 绘制音符工具处理：更新 DrawingState 预览状态（不创建 noteDraft），overlay 负责渲染
{
    const auto projection = ctx_.getContentProjection();
    if (!projection.isValid())
        return;

    const auto currentTime = pixelXToSourceTime(e.x);
    if (!currentTime)
        return;

    const auto editRange = sourceEditRange();
    const double clampedTime = juce::jlimit(editRange.startSeconds,
                                             editRange.endSeconds,
                                             *currentTime);

    float targetF0 = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));
    // DrawNote 是 OpenTune（CorrectedF0Primary）专属工具：绘制音符允许全部半音
    // （chromatic），直接半音量化，不读活动调式/顶栏 scale。
    int roundedMidi = static_cast<int>(std::lround(PitchUtils::freqToMidi(targetF0)));
    float snappedF0 = PitchUtils::midiToFreq(static_cast<float>(roundedMidi));

    // Compute before bounds from current drawing state
    juce::Rectangle<int> beforeBounds;
    {
        const auto& dr = ctx_.getState().drawing;
        if (dr.isDrawingNote && dr.drawingNotePitch > 0.0f) {
            auto vm = ctx_.getViewMapper();
            int sx1 = sourceTimeToScreenX(dr.drawingNoteStartTime);
            int sx2 = sourceTimeToScreenX(dr.drawingNoteEndTime);
            if (sx1 > sx2) std::swap(sx1, sx2);
            float ps = vm.midiToY(vm.freqToMidi(dr.drawingNotePitch)) - vm.midiToY(vm.freqToMidi(dr.drawingNotePitch) + 1.0f);
            float y = ctx_.contentOriginY + vm.freqToY(dr.drawingNotePitch) - ps * 0.5f;
            beforeBounds = juce::Rectangle<int>(sx1, static_cast<int>(y), std::max(1, sx2 - sx1), static_cast<int>(std::ceil(ps))).expanded(2);
        }
    }

    if (!ctx_.getState().drawing.isDrawingNote) {
        // First drag frame: initialize drawing state
        ctx_.getState().drawing.isDrawingNote = true;
        ctx_.setDrawingNoteStartTime(clampedTime);
        ctx_.setDrawingNoteEndTime(clampedTime);
        ctx_.setDrawingNotePitch(snappedF0);
    } else {
        // Subsequent drag frames: update end time
        ctx_.setDrawingNoteEndTime(clampedTime);
    }

    // Compute after bounds and invalidate
    if (ctx_.invalidateInteractionPreview) {
        const auto& dr = ctx_.getState().drawing;
        if (dr.isDrawingNote && dr.drawingNotePitch > 0.0f) {
            auto vm = ctx_.getViewMapper();
            int sx1 = sourceTimeToScreenX(dr.drawingNoteStartTime);
            int sx2 = sourceTimeToScreenX(dr.drawingNoteEndTime);
            if (sx1 > sx2) std::swap(sx1, sx2);
            float ps = vm.midiToY(vm.freqToMidi(dr.drawingNotePitch)) - vm.midiToY(vm.freqToMidi(dr.drawingNotePitch) + 1.0f);
            float y = ctx_.contentOriginY + vm.freqToY(dr.drawingNotePitch) - ps * 0.5f;
            juce::Rectangle<int> afterBounds(sx1, static_cast<int>(y), std::max(1, sx2 - sx1), static_cast<int>(std::ceil(ps)));
            ctx_.invalidateInteractionPreview(beforeBounds.getUnion(afterBounds.expanded(2)));
        }
    }
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
        // 搂8.5 鈥?selection box bounds compared against note.startTime (source time).
        const auto currentTime = pixelXToSourceTime(e.x);
        if (!currentTime)
            return;

        const auto editRange = sourceEditRange();
        const double clampedTime = juce::jlimit(editRange.startSeconds,
                                                 editRange.endSeconds,
                                                 *currentTime);

        ctx_.getState().selection.selectionEndTime = std::max(0.0, clampedTime);

        float currentMidi = ctx_.getViewMapper().freqToMidi(
            ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY)));
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
            float noteMidi = ctx_.getViewMapper().freqToMidi(note.getAdjustedPitch());
            bool timeOverlap = (note.endTime > selStartTime && note.startTime < selEndTime);
            bool pitchOverlap = (noteMidi >= selMinMidi - 0.5f && noteMidi <= selMaxMidi + 0.5f);
            if (timeOverlap && pitchOverlap) {
                selectedIndices.push_back(noteIndex);
            }
        }
        ctx_.getState().noteSelection.setFromIndices(std::move(selectedIndices),
                                                     static_cast<int>(notes.size()));
        updateF0SelectionFromNotes(notes);
        if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
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

        // 搂8.5 锟?Note resize edge writes startTime/endTime in SOURCE time.
        const auto currentTime = pixelXToSourceTime(e.x);
        if (!currentTime)
            return;

        const auto editRange = sourceEditRange();
        const double clampedTime = juce::jlimit(editRange.startSeconds,
                                                 editRange.endSeconds,
                                                 *currentTime);

        double minDuration = 0.02;
        const int resizeIdx = static_cast<int>(ctx_.getState().noteResize.noteIndex);

        // notes 已按 startTime 全局排序且互不重叠：右边缘只可能推让 resizeIdx+1，
        // 左边缘只可能推让 resizeIdx-1。目标先进入时间间隙；仅当实际侵入邻居时
        // 才移动邻居边界，且邻居保持最小长度 minDuration，目标扩展受其限制。
        // 不按 pitch 过滤，不链式压缩多个音符。
        if (ctx_.getState().noteResize.edge == NoteResizeEdge::Left) {
            double newStart = std::min(clampedTime, notes[static_cast<size_t>(resizeIdx)].endTime - minDuration);
            newStart = std::max(0.0, newStart);
            if (resizeIdx > 0) {
                const auto& prev = notes[static_cast<size_t>(resizeIdx - 1)];
                if (newStart < prev.endTime) {
                    // 短前驱（时长 < minDuration）不可再压缩：目标最小 start 至多到 prev.endTime，
                    // 不会越过其 end；正常前驱则以 prev.startTime + minDuration 为下界。
                    newStart = std::max(newStart, std::min(prev.endTime, prev.startTime + minDuration));
                    if (newStart < prev.endTime) {
                        notes[static_cast<size_t>(resizeIdx - 1)].endTime = newStart;
                        notes[static_cast<size_t>(resizeIdx - 1)].dirty = true;
                    }
                }
            }
            notes[static_cast<size_t>(resizeIdx)].startTime = newStart;
        } else if (ctx_.getState().noteResize.edge == NoteResizeEdge::Right) {
            double newEnd = std::max(clampedTime, notes[static_cast<size_t>(resizeIdx)].startTime + minDuration);
            if (resizeIdx + 1 < static_cast<int>(notes.size())) {
                const auto& next = notes[static_cast<size_t>(resizeIdx + 1)];
                if (newEnd > next.startTime) {
                    // 短后继（时长 < minDuration）不可再压缩：目标最大 end 至少到 next.startTime，
                    // 不会越过其 start；正常后继则以 next.endTime - minDuration 为上界。
                    newEnd = std::min(newEnd, std::max(next.startTime, next.endTime - minDuration));
                    if (newEnd > next.startTime) {
                        notes[static_cast<size_t>(resizeIdx + 1)].startTime = newEnd;
                        notes[static_cast<size_t>(resizeIdx + 1)].dirty = true;
                    }
                }
            }
            notes[static_cast<size_t>(resizeIdx)].endTime = newEnd;
        }

        notes[static_cast<size_t>(resizeIdx)].dirty = true;
        if (ctx_.invalidateLiveNotes) ctx_.invalidateLiveNotes(beforeNotes, notes);
        return;
    }

    if (ctx_.getState().noteDrag.draggedNoteIndex >= 0) {
        dragNotePitch(e);
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
// 选择工具鼠标释放处理：完成音符拖动/调整/框选，提交音高修正
{
    bool suppressFinalNoteDraftCommit = false;

    // 只要 mouseDown 落在音符上（draggedNoteIndex >= 0）就进入 endNotePitchDrag：
    // 无实际拖拽的点击也在其内部走统一终止路径（noteDrag.clear + draft 清理）。
    if (ctx_.getState().noteDrag.draggedNoteIndex >= 0) {
        suppressFinalNoteDraftCommit = endNotePitchDrag(e);
    }

    auto notes = std::vector<Note>(displayNotes(ctx_));
    const auto f0tl = ctx_.getF0Timeline();

    bool resizeMoved = false;
    double resizedStartTime = 0.0;
    double resizedEndTime = 0.0;
    const int resizeIdx = static_cast<int>(ctx_.getState().noteResize.noteIndex);
    if (ctx_.getState().noteResize.isResizing
        && resizeIdx >= 0
        && ctx_.getState().noteResize.isDirty
        && resizeIdx < static_cast<int>(notes.size())) {
        const auto& baselineTarget = draftBaselineNotes(ctx_)[static_cast<size_t>(resizeIdx)];
        const auto& resizedNote = notes[static_cast<size_t>(resizeIdx)];
        // Note::dirty 是历史编辑保留的渲染标记，不是本次 resize 的临时集合；
        // 以 baseline 与当前边界对比判定 resize 目标是否实际移动。
        resizeMoved = baselineTarget.startTime != resizedNote.startTime
            || baselineTarget.endTime != resizedNote.endTime;
        resizedStartTime = resizedNote.startTime;
        resizedEndTime = resizedNote.endTime;
    }

    notes.erase(
        std::remove_if(notes.begin(), notes.end(), [](const Note& n) {
            return n.startTime >= n.endTime;
        }),
        notes.end());

    if (ctx_.getState().noteResize.isResizing && resizeMoved) {
        const auto& baseline = draftBaselineNotes(ctx_);
        double dirtyStartTime = std::min(baseline[static_cast<size_t>(resizeIdx)].startTime, resizedStartTime);
        double dirtyEndTime = std::max(baseline[static_cast<size_t>(resizeIdx)].endTime, resizedEndTime);
        // 只合并 resize 目标与本次实际退让的唯一时间邻居：notes 按 startTime 全局
        // 排序且互不重叠，左边缘只可能推让 resizeIdx-1，右边缘只可能推让 resizeIdx+1。
        // 用 baseline 与当前 notes 的旧/新边界扩展脏区，覆盖目标和被推让邻居的
        // 完整旧/新区间，保证 commitNotesAndSegments 的范围过滤、F0 重算与 undo
        // 都包含邻居变化。不扫描 notes[i].dirty：dirty 是历史编辑保留的渲染标记，
        // 会把无关音符纳入全局范围。
        const int neighborIdx = ctx_.getState().noteResize.edge == NoteResizeEdge::Left
            ? resizeIdx - 1
            : resizeIdx + 1;
        if (neighborIdx >= 0 && neighborIdx < static_cast<int>(notes.size())) {
            const auto& oldNeighbor = baseline[static_cast<size_t>(neighborIdx)];
            const auto& newNeighbor = notes[static_cast<size_t>(neighborIdx)];
            const bool neighborYielded = ctx_.getState().noteResize.edge == NoteResizeEdge::Left
                ? oldNeighbor.endTime != newNeighbor.endTime
                : oldNeighbor.startTime != newNeighbor.startTime;
            if (neighborYielded) {
                dirtyStartTime = std::min(dirtyStartTime, std::min(oldNeighbor.startTime, newNeighbor.startTime));
                dirtyEndTime = std::max(dirtyEndTime, std::max(oldNeighbor.endTime, newNeighbor.endTime));
            }
        }
        auto pitchCurve = ctx_.getPitchCurve();
        F0FrameRange editRange;
        if (pitchCurve) {
            editRange = f0tl.rangeForTimes(dirtyStartTime, dirtyEndTime);
        }

        ctx_.getNoteDraft().workingNotes = notes;
        ctx_.setUndoDescription(juce::String::fromUTF8(u8"调整音符长度"));

        // 同步计算修正并一次性提交音符和F0
        if (pitchCurve && !editRange.isEmpty()) {
            commitNoteBasedCorrection(ctx_, notes, pitchCurve, editRange);
            suppressFinalNoteDraftCommit = true;
        } else {
            ctx_.commitNoteDraft();
        }
    }

    if (ctx_.getNoteDraft().active && !suppressFinalNoteDraftCommit) {
        ctx_.getNoteDraft().workingNotes = notes;
        ctx_.setUndoDescription(juce::String::fromUTF8(u8"编辑音符"));
        ctx_.commitNoteDraft();
    }

    ctx_.getState().noteResize.isResizing = false;
    ctx_.getState().noteResize.isDirty = false;
    ctx_.getState().noteResize.noteIndex = -1;
    ctx_.getState().noteResize.edge = NoteResizeEdge::None;

    if (ctx_.getState().selection.isSelectingArea) {
        ctx_.getState().selection.isSelectingArea = false;
        updateF0SelectionFromNotes(notes);
    }

    ctx_.getState().selection.isSelectingF0 = false;
    ctx_.getState().selection.f0SelectionAnchorFrame = -1;
    // note drag 终止状态由 endNotePitchDrag 唯一清理（noteDrag.clear），不再局部写
    ctx_.clearNoteDraft();
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
}

// ============================================================================
// OpenDyne（NotesPrimary）工具
//
// 唯一 pitch-drag 内部流程：OpenTune Select 与 OpenDyne Pitch Tool 共用。
// OpenTune（CorrectedF0Primary）固定 Chromatic 半音吸附，不读活动调式；
// OpenDyne 由 Pitch Grid 三态决定（KeyScale 走 quantizeMidiToActiveScale）。
// ============================================================================

void PianoRollToolHandler::beginNotePitchDrag(int clickedNoteIndex, const std::vector<Note>& notes)
{
    auto& state = ctx_.getState();
    state.noteDrag.draggedNoteIndex = clickedNoteIndex;
    state.noteDrag.draggedNoteIndices = collectSelectedNoteIndices(notes);
    state.noteDrag.previewSnapshot.reset();
}

F0FrameRange PianoRollToolHandler::noteDragEditRange(const std::vector<Note>& notes) const
{
    auto& state = ctx_.getState();
    const auto& baselineNotes = draftBaselineNotes(ctx_);

    double rangeStart = 1e30;
    double rangeEnd = -1e30;
    for (int noteIndex : state.noteDrag.draggedNoteIndices) {
        if (noteIndex < 0
            || noteIndex >= static_cast<int>(notes.size())
            || noteIndex >= static_cast<int>(baselineNotes.size())) {
            continue;
        }
        const auto& note = notes[static_cast<size_t>(noteIndex)];
        const auto& baseline = baselineNotes[static_cast<size_t>(noteIndex)];

        bool changed = false;
        if (currentTool_ == ToolId::PitchModulation) {
            // 两侧 effective：raw retuneSpeed >=0 取自身，否则都回退全局
            // retuneSpeed；禁止用 raw -1 与 target 直接对比。
            const float baselineEffective = baseline.retuneSpeed >= 0.0f
                ? baseline.retuneSpeed
                : ctx_.getRetuneSpeed();
            const float currentEffective = note.retuneSpeed >= 0.0f
                ? note.retuneSpeed
                : ctx_.getRetuneSpeed();
            changed = std::abs(currentEffective - baselineEffective) > 0.001f;
        } else if (currentTool_ == ToolId::PitchDrift) {
            changed = std::abs(note.pitchDriftScale - baseline.pitchDriftScale) > 0.001f;
        } else {
            changed = std::abs(note.pitchOffset - baseline.pitchOffset) > 0.001f;
        }
        if (!changed)
            continue;

        rangeStart = std::min(rangeStart, note.startTime);
        rangeEnd = std::max(rangeEnd, note.endTime);
    }

    if (rangeEnd <= rangeStart)
        return {};

    return ctx_.getF0Timeline().rangeForTimes(rangeStart, rangeEnd);
}

void PianoRollToolHandler::updateNoteBasedCorrectionPreview(const std::vector<Note>& notes)
{
    auto& state = ctx_.getState();
    state.noteDrag.previewSnapshot.reset();

    const F0FrameRange editRange = noteDragEditRange(notes);
    if (editRange.isEmpty())
        return;

    auto pitchCurve = ctx_.getPitchCurve();
    if (pitchCurve == nullptr)
        return;

    state.noteDrag.previewSnapshot =
        buildNoteBasedCorrectionState(ctx_, notes, pitchCurve, editRange);
}

void PianoRollToolHandler::dragNotePitch(const juce::MouseEvent& e)
{
    auto& state = ctx_.getState();

    // PitchModulation / PitchDrift：垂直拖拽直接修改标量参数
    if (currentTool_ == ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift) {
        if (!ctx_.getNoteDraft().active)
            ctx_.beginNoteDraft();
        ctx_.getNoteDraft().contentDirty = true;

        // 设置 Modulation/Drift 拖拽预览状态（必须在 invalidateLiveNotes 之前，
        // 组件侧据此扩展 dirty 区域，否则 F0 预览曲线超出 note 高度的部分残留）
        state.isModDriftDragging = true;
        state.modDriftTool = currentTool_;

        auto& notes = workingDraftNotes(ctx_);
        resetDraftNotesToBaseline(ctx_);

        const float deltaY = static_cast<float>(e.y - dragStartPos_.y);
        // 1 半音像素高度 ≈ 25% 参数变化（与 VolumeEnvelope 的 12dB/半音对齐视觉）
        const float paramDelta = -deltaY * 0.25f / 12.0f;

        for (int noteIndex : state.noteDrag.draggedNoteIndices) {
            auto& note = notes[static_cast<size_t>(noteIndex)];
            // 相对变化：每个 note 以 baseline 为唯一输入起点（Melodyne 行为）
            const auto& baseline = draftBaselineNotes(ctx_)[static_cast<size_t>(noteIndex)];
            if (currentTool_ == ToolId::PitchModulation) {
                // baseline 有效值：raw >=0 取自身，否则回退全局 retuneSpeed
                const float baselineEffective = baseline.retuneSpeed >= 0.0f
                    ? baseline.retuneSpeed
                    : ctx_.getRetuneSpeed();
                const float target = juce::jlimit(0.0f, 1.0f, baselineEffective + paramDelta);
                state.modDriftPreviewValue = target;
                // 仅实际变化才写字段；否则保持 baseline raw（尤其 -1 = 跟随全局）
                if (std::abs(target - baselineEffective) > 0.001f) {
                    note.retuneSpeed = target;
                    note.dirty = true;
                }
            } else {
                const float target = juce::jlimit(-1.0f, 1.0f, baseline.pitchDriftScale + paramDelta);
                state.modDriftPreviewValue = target;
                if (std::abs(target - baseline.pitchDriftScale) > 0.001f) {
                    note.pitchDriftScale = target;
                    note.dirty = true;
                }
            }
        }

        // 预览：以拖拽后的 retuneSpeed/pitchDriftScale 经唯一
        // updateNoteBasedCorrectionPreview（内部走权威 applyCorrectionToRange）
        // 构建 noteDrag.previewSnapshot，与 mouseUp 提交链完全一致。
        updateNoteBasedCorrectionPreview(notes);

        if (ctx_.invalidateLiveNotes) {
            const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));
            ctx_.invalidateLiveNotes(beforeNotes, notes);
        }

        if (ctx_.invalidateInteractionPreview) {
            ctx_.invalidateInteractionPreview(juce::Rectangle<int>());
        }
        return;
    }

    if (state.noteDrag.draggedNoteIndex < 0 || state.noteDrag.draggedNoteIndices.empty()) {
        return;
    }

    const auto beforeNotes = std::vector<Note>(displayNotes(ctx_));

    if (!state.noteDrag.isDraggingNotes) {
        state.noteDrag.isDraggingNotes = true;
    }
    if (!ctx_.getNoteDraft().active) {
        ctx_.beginNoteDraft();
    }
    ctx_.getNoteDraft().contentDirty = true;

    const float startF0 = ctx_.getViewMapper().yToFreq(static_cast<float>(dragStartPos_.y - ctx_.contentOriginY));
    const float currentF0 = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));

    float deltaSemitones = 0.0f;
    if (startF0 > 0.0f && currentF0 > 0.0f) {
        deltaSemitones = 12.0f * std::log2(currentF0 / startF0);
    }

    // OpenDyne Pitch Tool：Alt 拖拽期间临时解除吸附（保留连续 cents）；
    // OpenTune Select：固定 Chromatic 半音吸附，允许全部音高，不读活动调式。
    const bool openDyne = AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme());
    const bool altBypass = openDyne && e.mods.isAltDown();

    auto& notes = workingDraftNotes(ctx_);
    resetDraftNotesToBaseline(ctx_);
    for (int noteIndex : state.noteDrag.draggedNoteIndices) {
        auto& note = notes[static_cast<size_t>(noteIndex)];
        const float initialOffset = draftBaselineNotes(ctx_)[static_cast<size_t>(noteIndex)].pitchOffset;
        // 连续基准 MIDI（不提前取整）：SNAP 目标全程保持连续语义
        const float baseMidi = PitchUtils::freqToMidi(note.pitch);
        const float targetMidi = baseMidi + initialOffset + deltaSemitones;
        // OpenDyne：Pitch Grid 三态决定吸附方式；Alt 拖拽临时解除吸附（保留连续 cents）；OpenTune 固定半音
        float snappedMidi = targetMidi;
        if (!altBypass) {
            if (!openDyne) {
                // OpenTune：固定 Chromatic，吸附到最近半音，允许全部音高
                snappedMidi = std::round(targetMidi);
            } else {
                switch (pitchGridMode_) {
                    case PitchGridMode::NoSnap:
                        // 自由模式：不吸附，保留连续 cents
                        break;
                    case PitchGridMode::Chromatic:
                        snappedMidi = std::round(targetMidi);  // 吸附到最近半音
                        break;
                    case PitchGridMode::KeyScale: {
                        // 吸附到活动音阶；无配置时用默认 Chromatic（quantize 内部 round 半音）
                        const auto scaleSnap = ctx_.getActiveScaleSnap ? ctx_.getActiveScaleSnap() : std::nullopt;
                        const ScaleSnapConfig snap = scaleSnap.value_or(ScaleSnapConfig{});
                        snappedMidi = snap.quantizeMidiToActiveScale(targetMidi);
                        break;
                    }
                }
            }
        }
        // pitchOffset = snappedMidi - baseMidi ⇒ getAdjustedPitch() 精确等于目标 MIDI 频率；
        // 仅实际变化才写 pitchOffset/dirty，否则保留 baseline
        const float finalOffset = snappedMidi - baseMidi;
        if (std::abs(finalOffset - initialOffset) > 0.001f) {
            note.pitchOffset = finalOffset;
            note.dirty = true;
        }
    }

    updateNoteBasedCorrectionPreview(notes);
    if (ctx_.invalidateLiveNotes) ctx_.invalidateLiveNotes(beforeNotes, notes);
}

bool PianoRollToolHandler::endNotePitchDrag(const juce::MouseEvent& e)
{
    auto& state = ctx_.getState();

    // PitchModulation / PitchDrift：提交标量参数变更
    if (currentTool_ == ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift) {
        // 无实际拖拽（未产生 mouseDrag）：统一终止拖拽事务，不创建编辑
        if (!state.isModDriftDragging) {
            state.noteDrag.clear();
            ctx_.clearNoteDraft();
            state.isModDriftDragging = false;
            return true;
        }

        // 以 mouseUp 时刻位置应用最终参数
        dragNotePitch(e);

        auto baselineNotes = std::vector<Note>(draftBaselineNotes(ctx_));
        auto notes = std::vector<Note>(displayNotes(ctx_));
        auto pitchCurve = ctx_.getPitchCurve();

        // 唯一 editRange：由实际变化的音符派生（noteDragEditRange），与预览一致
        const F0FrameRange editRange = noteDragEditRange(notes);

        // 最终采样后、任何提交前复位预览：mouseUp 时刻 build 的 previewSnapshot
        // 只供拖拽期间渲染；无提交时也必须清掉，由下方 invalidation 把
        // retained raster 抹回 committed 状态。
        state.noteDrag.previewSnapshot.reset();

        bool committed = false;
        if (!editRange.isEmpty() && pitchCurve != nullptr) {
            ctx_.setUndoDescription(currentTool_ == ToolId::PitchModulation
                ? juce::String::fromUTF8(u8"调制深度")
                : juce::String::fromUTF8(u8"漂移修正"));
            // applyCorrectionToRange + commitNotesAndSegments 唯一提交链
            committed = commitNoteBasedCorrection(ctx_, notes, pitchCurve, editRange);
        }

        // 终止事务：draft 清理必须先于未提交 invalidation，使 rasterize 消费
        // committed display notes；无提交/提交失败时把拖拽期间 preview raster
        // 重绘回 committed 状态（isModDriftDragging 仍为 true、noteDrag 未 clear，
        // 保证 invalidation 走全高扩展）。
        ctx_.clearNoteDraft();
        if (!committed && ctx_.invalidateLiveNotes) {
            ctx_.invalidateLiveNotes(baselineNotes, notes);
        }

        state.noteDrag.clear();
        state.isModDriftDragging = false;
        return true;
    }

    if (state.noteDrag.draggedNoteIndex < 0) {
        return false;
    }

    // 有 index 但未进入实际拖拽（mouseDown 后直接 mouseUp）：无编辑产生，
    // 统一终止拖拽事务。
    if (!state.noteDrag.isDraggingNotes) {
        state.noteDrag.clear();
        ctx_.clearNoteDraft();
        return true;
    }

    // 以 mouseUp 时刻的 Alt 状态重算最终吸附（Alt 状态变化瞬间生效）。
    dragNotePitch(e);

    auto baselineNotes = std::vector<Note>(draftBaselineNotes(ctx_));
    auto notes = std::vector<Note>(displayNotes(ctx_));

    for (int noteIndex : state.noteDrag.draggedNoteIndices) {
        float initialOffset = draftBaselineNotes(ctx_)[static_cast<size_t>(noteIndex)].pitchOffset;
        float finalOffset = notes[static_cast<size_t>(noteIndex)].pitchOffset;

        if (std::abs(finalOffset - initialOffset) > 0.001f) {
            ctx_.notifyNoteOffsetChanged(static_cast<size_t>(noteIndex), initialOffset, finalOffset);
        }
    }

    // 唯一 editRange：由实际变化的音符派生（noteDragEditRange），与预览一致
    const F0FrameRange editRange = noteDragEditRange(notes);

    // 最终采样后、任何提交前复位预览：mouseUp 时刻 build 的 previewSnapshot
    // 只供拖拽期间渲染；无提交时也必须清掉，由下方 invalidation 把
    // retained raster 抹回 committed 状态。
    state.noteDrag.previewSnapshot.reset();

    bool committed = false;
    if (!editRange.isEmpty()) {
        auto pitchCurve = ctx_.getPitchCurve();
        ctx_.getNoteDraft().workingNotes = notes;
        ctx_.setUndoDescription(juce::String::fromUTF8(u8"移动音符"));

        // 音高编辑失败（无 PitchCurve）不允许 note-only 提交。
        if (pitchCurve != nullptr) {
            committed = commitNoteBasedCorrection(ctx_, notes, pitchCurve, editRange);
        }
    }

    // 终止事务：draft 清理必须先于未提交 invalidation，使 rasterize 消费
    // committed display notes；无提交/提交失败时把拖拽期间 preview raster
    // 重绘回 committed 状态（noteDrag 未 clear，isDraggingNotes 仍为 true，
    // 保证 invalidation 走全高扩展）。
    ctx_.clearNoteDraft();
    if (!committed && ctx_.invalidateLiveNotes) {
        ctx_.invalidateLiveNotes(baselineNotes, notes);
    }

    state.noteDrag.clear();
    // 始终 true：Select 路径必须禁止落入通用 commitNoteDraft note-only 旁路
    return true;
}

void PianoRollToolHandler::handlePitchToolMouseDown(const juce::MouseEvent& e)
{
    if (!AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme()))
        return;

    const auto& notes = committedNotes(ctx_);
    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return;
    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime))
        return;

    const float clickedPitch = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));
    const int clickedNoteIndex = findNoteIndexAt(notes, *sourceTime, clickedPitch, 1.0f);
    if (clickedNoteIndex < 0)
        return;

    auto& noteSelection = ctx_.getState().noteSelection;
    const int noteCount = static_cast<int>(notes.size());
    if (noteSelection.isSelected(clickedNoteIndex) && !noteSelection.isAllSelected(noteCount)) {
        // 点击已选中音符：保留多选，准备批量拖拽
    } else {
        // 点击未选中音符或全选状态：单选该音符
        noteSelection.setSingle(clickedNoteIndex, noteCount);
    }
    // 清除旧框选矩形，F0 编辑范围由实际 draggedNoteIndices 计算
    ctx_.getState().selection.hasSelectionArea = false;
    updateF0SelectionFromNotes(notes);
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();

    if (!noteSelection.isSelected(clickedNoteIndex))
        return;

    // PitchModulation / PitchDrift：记录拖拽起点，初始化选中音符索引
    if (currentTool_ == ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift) {
        dragStartPos_ = e.position.toInt();
        beginNotePitchDrag(clickedNoteIndex, notes);
        return;
    }

    // Pitch Tool (F2×1)：启动 pitch drag
    beginNotePitchDrag(clickedNoteIndex, notes);
}

void PianoRollToolHandler::handlePitchToolMouseUp(const juce::MouseEvent& e)
{
    // 终止状态只有 endNotePitchDrag 一个所有者（含 noteDrag.clear 与 draft 清理）
    endNotePitchDrag(e);
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
}

void PianoRollToolHandler::handlePitchToolDoubleClick(const juce::MouseEvent& e)
{
    const auto& notes = committedNotes(ctx_);
    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return;
    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime))
        return;

    const float clickedPitch = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));
    const int clickedNoteIndex = findNoteIndexAt(notes, *sourceTime, clickedPitch, 1.0f);
    if (clickedNoteIndex < 0)
        return;

    // PitchModulation / PitchDrift 双击：切换 100% ↔ 0%
    if (currentTool_ == ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift) {
        const auto& note = notes[static_cast<size_t>(clickedNoteIndex)];
        float currentVal = (currentTool_ == ToolId::PitchModulation)
            ? (note.retuneSpeed >= 0.0f ? note.retuneSpeed : ctx_.getRetuneSpeed())
            : note.pitchDriftScale;
        float newVal = (currentVal > 0.5f) ? 0.0f : 1.0f;

        ctx_.beginNoteDraft();
        auto& draft = ctx_.getNoteDraft();
        draft.contentDirty = true;
        auto work = draft.workingNotes;
        if (currentTool_ == ToolId::PitchModulation)
            work[static_cast<size_t>(clickedNoteIndex)].retuneSpeed = newVal;
        else
            work[static_cast<size_t>(clickedNoteIndex)].pitchDriftScale = newVal;
        work[static_cast<size_t>(clickedNoteIndex)].dirty = true;
        draft.workingNotes = work;
        ctx_.setUndoDescription(currentTool_ == ToolId::PitchModulation
            ? juce::String::fromUTF8(u8"调制深度")
            : juce::String::fromUTF8(u8"漂移修正"));

        auto pitchCurve = ctx_.getPitchCurve();
        const auto f0tl = ctx_.getF0Timeline();
        const F0FrameRange noteRange = f0tl.rangeForTimes(note.startTime, note.endTime);
        if (pitchCurve != nullptr && !noteRange.isEmpty()) {
            commitNoteBasedCorrection(ctx_, work, pitchCurve, noteRange);
        } else {
            ctx_.clearNoteDraft();
        }
        return;
    }

    // Pitch Tool (F2×1) 双击：按 Pitch Grid 模式吸附（与拖拽吸附同一三态语义）
    // NoSnap：不吸附；Chromatic：吸附到最近半音；KeyScale：吸附到活动音阶（无配置时按半音）
    auto scaleSnap = ctx_.getActiveScaleSnap ? ctx_.getActiveScaleSnap() : std::nullopt;

    const Note& original = notes[static_cast<size_t>(clickedNoteIndex)];
    // SNAP 按基准音高重投影：pitchOffset 不进入量化输入（与按钮入口一致）
    const float baseMidi = PitchUtils::freqToMidi(original.pitch);
    float snappedMidi = baseMidi;
    switch (pitchGridMode_) {
        case PitchGridMode::NoSnap:
            return;   // 自由模式：不吸附
        case PitchGridMode::Chromatic:
            snappedMidi = std::round(baseMidi);  // 吸附到最近半音
            break;
        case PitchGridMode::KeyScale: {
            // 吸附到活动音阶；无配置时用默认 Chromatic（quantize 内部 round 半音）
            const ScaleSnapConfig snap = scaleSnap.value_or(ScaleSnapConfig{});
            snappedMidi = snap.quantizeMidiToActiveScale(baseMidi);
            break;
        }
    }
    const float snappedOffset = snappedMidi - baseMidi;
    if (std::abs(snappedOffset - original.pitchOffset) < 0.001f)
        return;

    ctx_.beginNoteDraft();
    auto& draft = ctx_.getNoteDraft();
    draft.contentDirty = true;
    auto work = draft.workingNotes;
    work[static_cast<size_t>(clickedNoteIndex)].pitchOffset = snappedOffset;
    work[static_cast<size_t>(clickedNoteIndex)].dirty = true;
    draft.workingNotes = work;
    ctx_.setUndoDescription(juce::String::fromUTF8(u8"音高吸附"));

    auto pitchCurve = ctx_.getPitchCurve();
    const auto f0tl = ctx_.getF0Timeline();
    const F0FrameRange noteRange = f0tl.rangeForTimes(original.startTime, original.endTime);
    if (pitchCurve != nullptr && !noteRange.isEmpty()) {
        commitNoteBasedCorrection(ctx_, work, pitchCurve, noteRange);
    } else {
        ctx_.clearNoteDraft();   // 音高编辑失败不退化提交
    }
}

void PianoRollToolHandler::handleVolumeEnvelopeToolMouseDown(const juce::MouseEvent& e)
{
    if (!AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme()))
        return;

    const auto& notes = committedNotes(ctx_);
    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return;
    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime))
        return;

    const float clickedPitch = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));
    const int clickedNoteIndex = findNoteIndexAt(notes, *sourceTime, clickedPitch, 1.0f);
    if (clickedNoteIndex < 0)
        return;

    auto& state = ctx_.getState();
    if (!state.noteSelection.isSelected(clickedNoteIndex) || state.noteSelection.isAllSelected(static_cast<int>(notes.size())))
        state.noteSelection.setSingle(clickedNoteIndex, static_cast<int>(notes.size()));
    updateF0SelectionFromNotes(notes);

    state.isVolumeDragging = true;
    const auto snap = ctx_.getEditableContentSnapshot();
    volumeDragBaselineEnvelope_ = snap->volumeEnvelope;
    state.volumePreviewEnvelope = volumeDragBaselineEnvelope_;
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
}

AutomationLane PianoRollToolHandler::buildVolumeEnvelopeDragPreview(const std::vector<Note>& notes,
                                                                     float deltaGainDb)
{
    const auto selected = collectSelectedNoteIndices(notes);
    AutomationLane envelope = volumeDragBaselineEnvelope_;
    for (int index : selected) {
        const auto& note = notes[static_cast<size_t>(index)];
        envelope.setRegionGain(
            note.startTime,
            note.endTime,
            volumeDragBaselineEnvelope_.evalAt(note.startTime) + deltaGainDb);
    }
    return envelope;
}

void PianoRollToolHandler::handleVolumeEnvelopeToolDrag(const juce::MouseEvent& e)
{
    auto& state = ctx_.getState();
    if (!state.isVolumeDragging)
        return;

    const float dbPerPixel = 12.0f / ctx_.getViewMapper().pixelsPerSemitone;
    const float deltaGainDb = static_cast<float>(dragStartPos_.y - e.y) * dbPerPixel;
    const auto& notes = committedNotes(ctx_);
    state.volumePreviewEnvelope = buildVolumeEnvelopeDragPreview(notes, deltaGainDb);
    // blob 大小变化绘制在 content surface，需触发 rasterizeContent 重绘
    if (ctx_.invalidateLiveNotes) ctx_.invalidateLiveNotes(notes, notes);
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
}

void PianoRollToolHandler::handleVolumeEnvelopeToolUp(const juce::MouseEvent& e)
{
    auto& state = ctx_.getState();
    if (!state.isVolumeDragging)
        return;
    state.isVolumeDragging = false;

    const float dbPerPixel = 12.0f / ctx_.getViewMapper().pixelsPerSemitone;
    const float deltaGainDb = static_cast<float>(dragStartPos_.y - e.y) * dbPerPixel;

    if (std::abs(deltaGainDb) < 0.001f) {
        state.volumePreviewEnvelope.clear();
        return;
    }

    const auto& notes = committedNotes(ctx_);
    AutomationLane envelope = buildVolumeEnvelopeDragPreview(notes, deltaGainDb);
    state.volumePreviewEnvelope.clear();
    if (ctx_.commitVolumeEnvelope)
        ctx_.commitVolumeEnvelope(volumeDragBaselineEnvelope_, std::move(envelope));
}

void PianoRollToolHandler::handleVolumeEnvelopeToolDoubleClick(const juce::MouseEvent& e)
{
    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return;

    const auto& notes = committedNotes(ctx_);
    const float clickedPitch = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));
    const int noteIndex = findNoteIndexAt(notes, *sourceTime, clickedPitch, 1.0f);
    if (noteIndex < 0)
        return;

    auto& state = ctx_.getState();
    state.isVolumeDragging = false;
    state.volumePreviewEnvelope.clear();
    state.noteSelection.setSingle(noteIndex, static_cast<int>(notes.size()));
    updateF0SelectionFromNotes(notes);
    if (ctx_.invalidateSelectionFeedback)
        ctx_.invalidateSelectionFeedback();

    const auto snap = ctx_.getEditableContentSnapshot();
    const auto& note = notes[static_cast<size_t>(noteIndex)];
    AutomationLane envelope = snap->volumeEnvelope;
    envelope.setRegionGain(note.startTime, note.endTime, 0.0f);
    if (envelope == snap->volumeEnvelope)
        return;
    if (ctx_.commitVolumeEnvelope)
        ctx_.commitVolumeEnvelope(snap->volumeEnvelope, std::move(envelope));
}

void PianoRollToolHandler::updateScissorsPreview(const juce::MouseEvent& e)
{
    auto& state = ctx_.getState();
    double newPreview = -1.0;
    const auto sourceTime = pixelXToSourceTime(e.x);
    if (sourceTime) {
        const auto editRange = sourceEditRange();
        if (editRange.contains(*sourceTime)) {
            // 预览与提交语义一致：切点时间穿过任意音符即显示预览（不检查音高），
            // 具体命中音符由 drawScissorsPreview 过滤。
            for (const auto& note : committedNotes(ctx_)) {
                if (note.startTime < *sourceTime && *sourceTime < note.endTime) {
                    newPreview = *sourceTime;
                    break;
                }
            }
        }
    }
    if (state.scissorsPreviewTime != newPreview) {
        state.scissorsPreviewTime = newPreview;
        if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
    }
}

void PianoRollToolHandler::handleScissorsToolMouseDown(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    // 切点以 mouseUp 时刻为准提交；此处只清除旧预览。
    ctx_.getState().scissorsPreviewTime = -1.0;
}

void PianoRollToolHandler::handleScissorsToolUp(const juce::MouseEvent& e)
{
    auto& state = ctx_.getState();
    state.scissorsPreviewTime = -1.0;

    const auto beforeNotes = committedNotes(ctx_);
    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return;
    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime))
        return;

    const double splitTime = *sourceTime;

    // 切点时间竖线穿过的所有音符一律切割，与选中状态无关（Melodyne 行为）。
    auto isCutBySplitTime = [&](const Note& note) {
        return note.startTime < splitTime && splitTime < note.endTime;
    };
    size_t cutCount = 0;
    for (const auto& note : beforeNotes) {
        if (isCutBySplitTime(note))
            ++cutCount;
    }

    if (cutCount == 0)
        return;

    // 获取 effective (corrected) F0 数据用于 pitch center 重算；
    // F0 数据不可用时保持原 pitch（不重算）。
    std::vector<float> effectiveF0;
    const auto contentSnapshot = ctx_.getEditableContentSnapshot();
    const auto f0tl = ctx_.getF0Timeline();
    const bool hasF0 = contentSnapshot && !f0tl.isEmpty();
    if (hasF0) {
        effectiveF0.assign(static_cast<size_t>(f0tl.endFrameExclusive()), 0.0f);
        contentSnapshot->forEachEffectiveF0Span(0, f0tl.endFrameExclusive(),
            [&](int frameIndex, const float* data, int length, float gain) {
                if (data == nullptr || frameIndex < 0)
                    return;
                for (int i = 0; i < length; ++i) {
                    const int idx = frameIndex + i;
                    if (idx < static_cast<int>(effectiveF0.size()))
                        effectiveF0[static_cast<size_t>(idx)] = data[i] * gain;
                }
            });
    }

    // 构建新 notes 向量：每个待切割 note 生成左右两段，其余原样保留。
    std::vector<Note> newNotes;
    newNotes.reserve(beforeNotes.size() + cutCount);

    for (size_t i = 0; i < beforeNotes.size(); ++i) {
        const Note& original = beforeNotes[i];
        if (!isCutBySplitTime(original)) {
            newNotes.push_back(original);
            continue;
        }

        Note left = original;
        Note right = original;
        left.endTime = splitTime;
        left.dirty = true;
        right.startTime = splitTime;
        right.dirty = true;

        // pitch center 重算：用 effective F0 在各自时间范围内的平均值，
        // pitch 设为均值、pitchOffset=0，使 getAdjustedPitch() 直接返回该频率。
        if (hasF0 && !effectiveF0.empty()) {
            auto computeAvgF0 = [&](double tStart, double tEnd) -> float {
                const int startFrame = f0tl.frameAtOrBefore(tStart);
                const int endFrame = f0tl.exclusiveFrameAt(tEnd);
                float sum = 0.0f;
                int count = 0;
                for (int f = startFrame; f < endFrame; ++f) {
                    if (f < 0 || f >= static_cast<int>(effectiveF0.size()))
                        continue;
                    const float f0 = effectiveF0[static_cast<size_t>(f)];
                    if (f0 > 0.0f) {
                        sum += f0;
                        ++count;
                    }
                }
                return count > 0 ? sum / static_cast<float>(count) : 0.0f;
            };

            const float leftAvgF0 = computeAvgF0(original.startTime, splitTime);
            if (leftAvgF0 > 0.0f) {
                left.pitch = leftAvgF0;
                left.pitchOffset = 0.0f;
            }

            const float rightAvgF0 = computeAvgF0(splitTime, original.endTime);
            if (rightAvgF0 > 0.0f) {
                right.pitch = rightAvgF0;
                right.pitchOffset = 0.0f;
            }
        }

        newNotes.push_back(left);
        newNotes.push_back(right);
    }

    if (!ctx_.replaceContentNotesForFullMutation || !ctx_.replaceContentNotesForFullMutation(newNotes)) {
        return;
    }
    if (ctx_.republishPlaybackSource)
        ctx_.republishPlaybackSource();

    // 选中切割后的右侧段（最后一个被切割 note 的右段），避免下次拖拽同时移动两侧。
    int lastRightIdx = -1;
    int newIdx = 0;
    for (size_t i = 0; i < beforeNotes.size(); ++i) {
        const bool isCut = isCutBySplitTime(beforeNotes[i]);
        if (isCut)
            lastRightIdx = newIdx + 1;
        newIdx += isCut ? 2 : 1;
    }
    if (lastRightIdx >= 0 && lastRightIdx < static_cast<int>(newNotes.size())) {
        state.noteSelection.setSingle(lastRightIdx, static_cast<int>(newNotes.size()));
        updateF0SelectionFromNotes(newNotes);
    }
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();

    // 不切换工具 —— 保持 Scissors，与 Melodyne 一致。

    if (ctx_.pushUndoAction) {
        auto action = std::make_unique<ScissorsUndoAction>(
            juce::String::fromUTF8(u8"音符分割"),
            beforeNotes,
            newNotes,
            ctx_.replaceContentNotesForFullMutation,
            ctx_.republishPlaybackSource);
        ctx_.pushUndoAction(std::move(action));
    }
}

void PianoRollToolHandler::handleDrawCurveUp(const juce::MouseEvent& e)
// 手绘曲线宸ュ叿榧犳爣閲婃斁澶勭悊锛氬皢缁樺埗鐨凢0鏁版嵁鎻愪氦鍒伴煶楂樹慨姝ｉ槦锟?
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
            if (ctx_.invalidateInteractionPreview) ctx_.invalidateInteractionPreview(dirtyBefore.getUnion(ctx_.getHandDrawPreviewBounds()));
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
            ctx_.setUndoDescription(juce::String::fromUTF8(u8"手绘曲线"));
            ctx_.applyManualCorrection(std::move(ops), editedStartFrame, editedEndFrameExclusive - 1, false);
            ctx_.notifyPitchCurveEdited(editedStartFrame, editedEndFrameExclusive - 1);
            selectNotesForEditedFrameRange(ctx_, editedStartFrame, editedEndFrameExclusive);
        }
    }


    ctx_.getState().drawing.isDrawingF0 = false;
    ctx_.setDirtyStartTime(-1.0);
    ctx_.setDirtyEndTime(-1.0);
    ctx_.getState().drawing.handDrawBuffer.clear();
    if (ctx_.invalidateInteractionPreview) ctx_.invalidateInteractionPreview(dirtyBefore.getUnion(ctx_.getHandDrawPreviewBounds()));
}

void PianoRollToolHandler::handleDrawNoteUp(const juce::MouseEvent& e)
// 绘制音符工具鼠标释放处理：完成音符绘制，分割重叠音符，应用最小时间
// Option B: noteDraft 浠呭湪 mouseUp 鏃朵竴娆℃€у垱寤哄苟鎻愪氦
{
    const auto beforeNotes = std::vector<Note>(committedNotes(ctx_));

    if (ctx_.getDrawNoteToolPendingDrag()) {
        // Capture stale preview bounds before clearing state
        juce::Rectangle<int> staleBounds;
        {
            const auto& dr = ctx_.getState().drawing;
            if (dr.isDrawingNote && dr.drawingNotePitch > 0.0f) {
                auto vm = ctx_.getViewMapper();
                int sx1 = sourceTimeToScreenX(dr.drawingNoteStartTime);
                int sx2 = sourceTimeToScreenX(dr.drawingNoteEndTime);
                if (sx1 > sx2) std::swap(sx1, sx2);
                float ps = vm.midiToY(vm.freqToMidi(dr.drawingNotePitch)) - vm.midiToY(vm.freqToMidi(dr.drawingNotePitch) + 1.0f);
                float y = ctx_.contentOriginY + vm.freqToY(dr.drawingNotePitch) - ps * 0.5f;
                staleBounds = juce::Rectangle<int>(sx1, static_cast<int>(y), std::max(1, sx2 - sx1), static_cast<int>(std::ceil(ps))).expanded(2);
            }
        }
        ctx_.setDrawNoteToolPendingDrag(false);
        if (ctx_.invalidateInteractionPreview && !staleBounds.isEmpty())
            ctx_.invalidateInteractionPreview(staleBounds);
        return;
    }
    
    if (!ctx_.getState().drawing.isDrawingNote) {
        return;
    }
    
    ctx_.getState().drawing.isDrawingNote = false;

    const auto editRange = sourceEditRange(0.02);
    // 搂8.5 鈥?DrawNote release writes endTime in SOURCE time.
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
        // 不烘焙 retuneSpeed/vibratoDepth/vibratoRate：保持 -1 跟随全局，
        // 音符级显式参数只由用户编辑（applyNoteParameterToSelectedNotes）写入。
        finalNote.dirty = true;

        float newPip = ctx_.calculateEffectivePIP(finalNote);
        if (newPip > 0.0f) {
            const float sourceMidi = PitchUtils::freqToMidi(newPip);
            finalNote.pitch = PitchUtils::midiToFreq(std::round(sourceMidi));
            finalNote.originalPitch = newPip;

            const float targetMidi = PitchUtils::freqToMidi(ctx_.getDrawingNotePitch());
            finalNote.pitchOffset = targetMidi - std::round(sourceMidi);
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
        int newSelectedIndex = findNoteIndexAt(notes, midTime, finalNote.getAdjustedPitch(), 1.0f);
        if (newSelectedIndex >= 0) {
            ctx_.getState().noteSelection.setSingle(newSelectedIndex,
                                                    static_cast<int>(notes.size()));
            updateF0SelectionFromNotes(notes);
        }
    }

    ctx_.getNoteDraft().contentDirty = true;
    ctx_.getNoteDraft().workingNotes = notes;
    ctx_.setUndoDescription(juce::String::fromUTF8(u8"绘制音符"));

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
    if (ctx_.invalidateLiveNotes) ctx_.invalidateLiveNotes(beforeNotes, committedNotes(ctx_));
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
    // 搂8.5 鈥?LineAnchor places anchors at SOURCE time (PitchCurve indexing).
    const auto clickTime = pixelXToSourceTime(e.x);
    if (!clickTime)
        return;

    if (!editRange.contains(*clickTime))
        return;

    float clickFreq = ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));
    clickFreq = std::max(20.0f, clickFreq);
    const auto scheme = ctx_.getAudioEditingScheme();

    auto pitchCurve = ctx_.getPitchCurve();
    if (!pitchCurve) return;
    const auto& originalF0 = ctx_.getOriginalF0();
    if (originalF0.empty()) return;
    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) return;

    const float midiNote = PitchUtils::freqToMidi(clickFreq);
    const bool openDyne = AudioEditingScheme::usesNotesPrimaryScheme(scheme);
    float snappedFreq;
    if (openDyne && e.mods.isAltDown()) {
        snappedFreq = clickFreq;  // OpenDyne Alt：旁路量化，直接使用点击频率
    } else if (openDyne) {
        // OpenDyne：按活动调式音阶吸附
        const auto scaleSnap = ctx_.getActiveScaleSnap ? ctx_.getActiveScaleSnap() : std::nullopt;
        const ScaleSnapConfig snap = scaleSnap.value_or(ScaleSnapConfig{});
        const int roundedMidi = static_cast<int>(std::lround(snap.quantizeMidiToActiveScale(midiNote)));
        snappedFreq = PitchUtils::midiToFreq(static_cast<float>(roundedMidi));
    } else {
        // OpenTune：允许全部半音（chromatic），不读活动调式
        snappedFreq = PitchUtils::midiToFreq(static_cast<float>(std::lround(midiNote)));
    }

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
            int segmentIdx = ctx_.findLineAnchorSegmentNear(e.x, e.y - ctx_.contentOriginY);
            if (segmentIdx >= 0) {
                if (e.mods.isCtrlDown() || e.mods.isCommandDown()) {
                    ctx_.toggleLineAnchorSegmentSelection(segmentIdx);
                } else {
                    ctx_.selectLineAnchorSegment(segmentIdx);
                }
                if (ctx_.invalidateInteractionPreview) ctx_.invalidateInteractionPreview(ctx_.getLineAnchorPreviewBounds());
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
        if (ctx_.invalidateInteractionPreview) ctx_.invalidateInteractionPreview(ctx_.getLineAnchorPreviewBounds());
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
    ctx_.setUndoDescription(juce::String::fromUTF8(u8"锚点修正"));
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
    if (ctx_.invalidateInteractionPreview) ctx_.invalidateInteractionPreview(ctx_.getLineAnchorPreviewBounds());
}

void PianoRollToolHandler::handleLineAnchorMouseDrag(const juce::MouseEvent& e) {
    if (!ctx_.getState().drawing.isPlacingAnchors) return;
    const auto dirtyBefore = ctx_.getLineAnchorPreviewBounds();
    ctx_.getState().drawing.currentMousePos = e.position;
    if (ctx_.invalidateInteractionPreview)
        ctx_.invalidateInteractionPreview(dirtyBefore.getUnion(ctx_.getLineAnchorPreviewBounds()));
}

void PianoRollToolHandler::clearLineAnchorPreview()
{
    const auto dirtyBefore = ctx_.getLineAnchorPreviewBounds();
    ctx_.getState().drawing.isPlacingAnchors = false;
    ctx_.getState().drawing.pendingAnchors.clear();
    if (ctx_.invalidateInteractionPreview)
        ctx_.invalidateInteractionPreview(dirtyBefore.getUnion(ctx_.getLineAnchorPreviewBounds()));
}

int PianoRollToolHandler::findNoteIndexAt(const std::vector<Note>& notes,
                                          double time,
                                          float targetPitchHz,
                                          float pitchToleranceSemitones)
{
    const float targetMidi = ctx_.getViewMapper().freqToMidi(targetPitchHz);
    for (int i = 0; i < static_cast<int>(notes.size()); ++i) {
        const auto& note = notes[static_cast<size_t>(i)];
        if (time >= note.startTime && time < note.endTime) {
            const float adjustedPitch = note.getAdjustedPitch();
            const float noteMidi = ctx_.getViewMapper().freqToMidi(adjustedPitch);
            if (std::abs(noteMidi - targetMidi) <= pitchToleranceSemitones) {
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
// vocal-time-stretch §8.4 — Time tool handlers
// ============================================================================

uint64_t PianoRollToolHandler::hitTestTimeGridHandle(const juce::MouseEvent& e) const
{
    if (!ctx_.getActiveContentTimeGrid) return 0;
    auto snap = ctx_.getActiveContentTimeGrid();
    if (snap == nullptr) return 0;

    constexpr int kHitToleranceX = 5;  // pixels

    // Use uint64_t throughout; handle ids are stable opaque values.
    uint64_t closestId = 0;
    int closestDistance = std::numeric_limits<int>::max();
    for (const auto& h : snap->handles()) {
        if (h.isEndpoint()) continue;   // endpoints not selectable

        // output_seconds 锟?timeline via projectContentTimeToTimeline 锟?screen X via timeToX.
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
        // Clicked empty space 锟?seek playhead + clear selection.
        // §8.4: TimeTool 下主区空白点击现在也会重定位播放头，
        // 涓庢爣灏哄尯鐐瑰嚮琛屼负涓€鑷达紝娑堥櫎"鐐瑰嚮鏃犲搷锟?鐨勭敤鎴峰洶鎯戯拷?
        // 搂8.4 (bugfix): playhead seek must use TIMELINE time
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

    // Found a handle 锟?select + arm drag.
    auto snap = ctx_.getActiveContentTimeGrid ? ctx_.getActiveContentTimeGrid() : nullptr;
    if (snap == nullptr) {
        AppLogger::warn("[TimeTool] mouseDown: no TimeGridSnapshot available");
        return;
    }

    const TimeHandle* hitHandle = nullptr;
    for (const auto& h : snap->handles()) {
        if (h.id == hitId) { hitHandle = &h; break; }
    }
    if (hitHandle == nullptr || hitHandle->isEndpoint()) return;

    // 鈿★笍 搂8.4 锟?Shift+click toggles in additionalSelectedIds
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

    // 搂8.4: 鍛戒腑 handle 鍚庡厛杩涘叆 pending 鐘舵€侊拷?
    // mouseDrag 越过阈值后才转为真正拖拽，防止轻微抖动触发 undo。
    tt.dragPending = true;
    tt.isDraggingHandle = false;
    tt.draggedHandleId = hitId;
    tt.dragSnapDisabled = e.mods.isAltDown();   // Alt disables clamp
    tt.dragOriginalSnapshot = snap;
    tt.dragWorkingSnapshot = snap;   // identity at drag start
    tt.dragStartOutputSeconds = hitHandle->output_seconds;
    tt.dragStartPixel = e.getPosition();

    if (ctx_.repaintTimeGridHandles) ctx_.repaintTimeGridHandles();
}

void PianoRollToolHandler::handleTimeToolMouseDrag(const juce::MouseEvent& e)
{
    auto& tt = ctx_.getState().timeTool;

    // 搂8.4: 妫€锟?dragPending 闃堝€硷拷?
    // Time handles only move horizontally; use X-axis-only threshold so
    // vertical jitter does not start a drag that produces an identical output.
    if (tt.dragPending) {
        const int dx = std::abs(e.x - tt.dragStartPixel.x);
        constexpr int kHandleDragThreshold = 6;
        if (dx <= kHandleDragThreshold) {
            return;   // 灏氭湭瓒婅繃闃堝€硷紝淇濇寔 pending
        }
        tt.dragPending = false;
        tt.isDraggingHandle = true;
    }

    if (!tt.isDraggingHandle || tt.dragOriginalSnapshot == nullptr) return;

    // pixel 锟?timeline time 锟?content-local output time
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

    // 鈿★笍 搂8.4 锟?group-drag detection.
    // If the user has multi-selected handles AND the dragged handle is part
    // of that selection, every selected handle moves by the same delta
    // (uniformDelta).  Otherwise only the dragged handle moves.
    const bool isGroupDrag = (!tt.additionalSelectedIds.empty())
                              && tt.isSelected(tt.draggedHandleId);

    // Spacing rule between adjacent outputs.
    // Alt held at drag start (`dragSnapDisabled`) bypasses the clamp for
    // power-user nudging into tight regions.
    const double kMinSpacingSec = tt.dragSnapDisabled ? 0.000 : TimeGridSnapshot::kMinOutputSpacingSeconds;
    const double minOutput = origHandles[static_cast<size_t>(draggedIdx - 1)].output_seconds + kMinSpacingSec;
    const double maxOutput = origHandles[static_cast<size_t>(draggedIdx + 1)].output_seconds - kMinSpacingSec;
    const double clampedOutput = juce::jlimit(minOutput, maxOutput, newOutputTime);
    const double uniformDelta = clampedOutput - tt.dragStartOutputSeconds;

    // Build new handle vector.  For group drag we shift every selected,
    // non-endpoint handle by uniformDelta and clamp each individually to its
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
            if (newHandles[i].isEndpoint()) continue;
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
                // Region too tight 锟?abort the drag step (keep last valid
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

    // 搂8.4: 濡傛灉浠庢湭瓒婅繃鎷栧姩闃堝€硷紝浠呬繚锟?selection 涓嶆彁浜わ拷?
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
                             juce::String::fromUTF8(u8"鎷栧姩鏃堕棿鎵嬫焺"));
    }

    tt.isDraggingHandle = false;
    tt.draggedHandleId = 0;
    tt.dragOriginalSnapshot.reset();
    tt.dragWorkingSnapshot.reset();
}

// ============================================================================
// 搂8.4 锟?Double-click to insert UserAdded handle
//
// Constraints (per spec time-tool-interaction.md):
//   - Click must be on empty area (no existing handle within 卤5 px)
//   - Click position must satisfy TimeGrid output/source spacing
//   - source_seconds initially equals output_seconds (identity insertion);
//     subsequent drag operations modify only output_seconds
// ============================================================================
void PianoRollToolHandler::handleTimeToolMouseDoubleClick(const juce::MouseEvent& e)
{
    if (!ctx_.getActiveContentTimeGrid || !ctx_.commitTimeGrid) return;
    auto snap = ctx_.getActiveContentTimeGrid();
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
    // 搂8.4 bugfix: clickedTime is output/content time.
    // Source spacing check must compare against source_seconds, so
    // compute clickedOutput 锟?clickedSource via tauInverse.
    // Identity grid 锟?tauInverse is identity 锟?same value.
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
    // time via tauInverse.  Identity grid 锟?tauInverse is identity.
    newHandle.source_seconds = snap->tauInverse(clickedTime);
    newHandle.output_seconds = clickedTime;
    newHandle.kind = HandleKind::UserAdded;
    newHandles.insert(newHandles.begin() + insertIdx, newHandle);

    auto newSnap = TimeGridSnapshot::makeFromHandles(
        std::move(newHandles), /*revision=*/snap->revision() + 1);
    if (newSnap == nullptr) {
        AppLogger::warn("[TimeTool] insert: makeFromHandles validation failed");
        return;
    }

    ctx_.commitTimeGrid(newSnap, snap, juce::String::fromUTF8(u8"鎻掑叆鏃堕棿鎵嬫焺"));

    // Auto-select the newly-inserted handle so user can immediately drag.
    // commitTimeGrid already triggers cache dirty internally.
    auto& tt = ctx_.getState().timeTool;
    tt.selectedHandleId = newHandle.id;
}

// ============================================================================
// 搂8.4 锟?Delete key removes selected handle (non-endpoint only)
//
// Returns true when a handle was deleted (caller should not fall through to
// note-delete logic).  Returns false when nothing was selected or the only
// selected handle is an endpoint.
// ============================================================================
bool PianoRollToolHandler::handleTimeToolDeleteSelected()
{
    if (!ctx_.getActiveContentTimeGrid || !ctx_.commitTimeGrid) return false;
    auto& tt = ctx_.getState().timeTool;
    if (tt.selectedHandleId == 0) return false;

    auto snap = ctx_.getActiveContentTimeGrid();
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
        // Endpoint or not found 锟?cannot delete
        AppLogger::log("[TimeTool] delete rejected: cannot delete endpoint or unknown handle");
        return false;
    }
    if (handles[static_cast<size_t>(targetIdx)].isEndpoint()) {
        AppLogger::log("[TimeTool] delete rejected: handle is endpoint");
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

    ctx_.commitTimeGrid(newSnap, snap, juce::String::fromUTF8(u8"鍒犻櫎鏃堕棿鎵嬫焺"));

    tt.selectedHandleId = 0;
    tt.hoveredHandleId  = 0;
    // commitTimeGrid already triggers cache dirty internally.
    return true;
}

} // namespace OpenTune
