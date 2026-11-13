#include "PianoRollToolHandler.h"
#include "PianoRollTimeMap.h"
#include "../../../Utils/AudioEditingScheme.h"
#include "../../../Utils/AppLogger.h"
#include "../../../Utils/KeyShortcutConfig.h"
#include "../../../Utils/ScissorsTopologyEdit.h"
#include "../../../Utils/PitchUtils.h"
#include "../../../Utils/TimeCoordinate.h"
#include "../../../Utils/TimeGridEdit.h"
#include "../../../Utils/LocalizationManager.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace OpenTune {

namespace {

constexpr int kNoteResizeEdgeThresholdPx = 6;
constexpr float kNotePitchHitToleranceSemitones = 1.0f;
constexpr double kNoteResizeMinDuration = 0.02;

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
// 共用：复制当前 EditableContentSnapshot，替换 notes 与 scratch curve 后经权威
// applyCorrectionToRange（含全局 pitch-shift 比例）烘焙的 pitchCurve。
// ============================================================================
std::shared_ptr<const EditableContentSnapshot> buildNoteBasedCorrectionState(
    PianoRollToolHandler::Context& ctx,
    const std::vector<Note>& notes,
    F0FrameRange editRange)
{
    const auto contentSnapshot = ctx.getEditableContentSnapshot();
    if (contentSnapshot == nullptr || contentSnapshot->pitchCurve == nullptr)
        return nullptr;

    auto snap = std::make_shared<EditableContentSnapshot>(*contentSnapshot);
    snap->notes = notes;

    auto scratchCurve = PitchCurve::fromSnapshot(contentSnapshot->pitchCurve);
    snap->pitchCurve = scratchCurve->applyCorrectionToRange(
        notes,
        editRange.startFrame,
        editRange.endFrameExclusive,
        static_cast<float>(contentSnapshot->pitchShiftSettings.getPitchRatio()),
        ctx.getRetuneSpeed(),
        ctx.getVibratoDepth(),
        ctx.getVibratoRate());
    return snap;
}

bool commitNoteBasedCorrection(PianoRollToolHandler::Context& ctx,
                               const std::vector<Note>& notes,
                               F0FrameRange editRange,
                               const std::function<bool(const std::vector<PitchCorrectionSegment>&,
                                                        F0FrameRange)>& commit = {})
{
    const auto f0tl = ctx.getF0Timeline();
    const auto snap = buildNoteBasedCorrectionState(ctx, notes, editRange);
    if (snap == nullptr)
        return false;

    const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(editRange.startFrame,
                                                                          editRange.endFrameExclusive,
                                                                          f0tl.endFrameExclusive());

    // Extract segments overlapping the affected range (range-scoped, not full)
    auto allSegments = snap->pitchCurve->getCorrectionSegments();
    std::vector<PitchCorrectionSegment> segmentsInRange;
    for (const auto& seg : allSegments) {
        if (seg.startFrame < affectedRange.endFrameExclusive && seg.endFrame > affectedRange.startFrame)
            segmentsInRange.push_back(seg);
    }

    const bool committed = commit
        ? commit(segmentsInRange, affectedRange)
        : ctx.commitNotesAndSegments(notes, segmentsInRange, affectedRange) != nullptr;
    if (!committed) {
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

PianoRollToolHandler::ParameterTargetSelection PianoRollToolHandler::beginParameterTargetChange()
{
    // Input dispatch commits pending parameters before reading notes or planning edits.
    const auto& state = ctx_.getState();
    return { state.noteSelection.selectedIndices, state.frameSelection.ranges };
}

void PianoRollToolHandler::finishParameterTargetChange(const ParameterTargetSelection& before, bool forceSync)
{
    const auto& state = ctx_.getState();
    if (forceSync || before.noteIndices != state.noteSelection.selectedIndices
        || before.frameRanges != state.frameSelection.ranges) {
        if (ctx_.parameterTargetDidChange)
            ctx_.parameterTargetDidChange();
    }
}

void PianoRollToolHandler::beginMouseParameterTargetChange()
{
    if (!mouseParameterTargetBefore_.has_value()) {
        const auto& state = ctx_.getState();
        mouseParameterTargetBefore_ = {
            state.noteSelection.selectedIndices,
            state.frameSelection.ranges
        };
    }
}

void PianoRollToolHandler::finishMouseParameterTargetChange()
{
    if (!mouseParameterTargetBefore_.has_value())
        return;
    const auto before = std::move(*mouseParameterTargetBefore_);
    mouseParameterTargetBefore_.reset();
    finishParameterTargetChange(before);
}

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
    if (!grid)
        return std::nullopt;

    const PianoRollTimeMap map(projection, *grid, ctx_.getViewMapper());
    return map.xToSource(pixelX);
}

int PianoRollToolHandler::sourceTimeToScreenX(double sourceSeconds) const
{
    // Pipeline: source 鈫?tauForward 鈫?output(content) 鈫?timeline 鈫?screen X.
    // projection valid 是调用方契约（入口已检查）。
    const auto projection = ctx_.getContentProjection();
    jassert(projection.isValid());

    const auto grid = ctx_.getActiveContentTimeGrid();
    if (!grid)
        return ctx_.getViewMapper().timeToX(0.0);

    const PianoRollTimeMap map(projection, *grid, ctx_.getViewMapper());
    return map.sourceToX(sourceSeconds);
}

SourceEditRange PianoRollToolHandler::sourceEditRange(double minDurationSeconds) const
{
    const auto grid = ctx_.getActiveContentTimeGrid();
    if (!grid)
        return { 0.0, 0.0, minDurationSeconds };
    return SourceEditRange::fromTimeGrid(*grid, minDurationSeconds);
}

PianoRollToolHandler::NoteHitResult PianoRollToolHandler::hitTestNoteAt(
    const std::vector<Note>& notes,
    const juce::MouseEvent& e) const
{
    const auto mapper = ctx_.getViewMapper();
    const float mousePitch = mapper.yToFreq(static_cast<float>(e.y - ctx_.contentOriginY));
    const float mouseMidi = mapper.freqToMidi(mousePitch);

    for (int noteIndex = 0; noteIndex < static_cast<int>(notes.size()); ++noteIndex) {
        const auto& note = notes[static_cast<size_t>(noteIndex)];
        const float noteMidi = mapper.freqToMidi(note.getAdjustedPitch());
        if (std::abs(mouseMidi - noteMidi) > kNotePitchHitToleranceSemitones)
            continue;

        const int x1 = sourceTimeToScreenX(note.startTime);
        const int x2 = sourceTimeToScreenX(note.endTime);
        const int left = std::min(x1, x2);
        const int right = std::max(x1, x2);
        if (e.x < left || e.x > right)
            continue;

        const int width = right - left;
        // x1..x2 is an inclusive pixel range; width 13 still has no interior pixel.
        if (width <= 2 * kNoteResizeEdgeThresholdPx + 1)
            return { noteIndex, NoteResizeEdge::None };

        if (e.x - left <= kNoteResizeEdgeThresholdPx)
            return { noteIndex, x1 <= x2 ? NoteResizeEdge::Left : NoteResizeEdge::Right };
        if (right - e.x <= kNoteResizeEdgeThresholdPx)
            return { noteIndex, x1 <= x2 ? NoteResizeEdge::Right : NoteResizeEdge::Left };

        return { noteIndex, NoteResizeEdge::None };
    }

    return {};
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
        ctx_.setMouseCursor(juce::MouseCursor::CrosshairCursor);
        return;
    }

    if (currentTool_ == ToolId::TimeTool) {
        handleTimeToolMouseMove(e);
        return;
    }

    if (currentTool_ == ToolId::Scissors) {
        updateScissorsPreview(e);
        ctx_.setMouseCursor(juce::MouseCursor::CrosshairCursor);
        return;
    }

    if (currentTool_ == ToolId::Eq) {
        ctx_.setMouseCursor(ctx_.eqCursor);
        return;
    }

    if (currentTool_ == ToolId::Eraser) {
        updateEraserCursor(e);
        ctx_.setMouseCursor(juce::MouseCursor::CrosshairCursor);
        return;
    }

    if (currentTool_ == ToolId::AutoTune) {
        ctx_.setMouseCursor(juce::MouseCursor::PointingHandCursor);
        return;
    }

    if (currentTool_ != ToolId::Select) {
        // DrawNote / HandDraw / Pitch / PitchModulation / PitchDrift / VolumeEnvelope
        ctx_.setMouseCursor(juce::MouseCursor::CrosshairCursor);
        return;
    }

    const auto projection = ctx_.getContentProjection();
    if (!projection.isValid())
    {
        ctx_.setMouseCursor(juce::MouseCursor::NormalCursor);
        return;
    }

    const auto hit = hitTestNoteAt(displayNotes(ctx_), e);
    if (hit.noteIndex >= 0 && hit.edge != NoteResizeEdge::None) {
        ctx_.setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    } else if (hit.noteIndex >= 0) {
        ctx_.setMouseCursor(juce::MouseCursor::UpDownLeftRightResizeCursor);
    } else {
        ctx_.setMouseCursor(juce::MouseCursor::NormalCursor);
    }
}

void PianoRollToolHandler::armTimelineSeek(const juce::MouseEvent& e, bool commit)
{
    if (!ctx_.notifyPlayheadChange)
        return;

    const double time = ctx_.getViewMapper().xToTime(e.x);
    const double resolved = time >= 0.0 ? time : timelineSeekTime_;
    if (resolved < 0.0)
        return;

    timelineSeekTime_ = resolved;
    ctx_.notifyPlayheadChange(resolved, commit);
}

void PianoRollToolHandler::mouseDown(const juce::MouseEvent& e)
{
    if (ctx_.parameterTargetWillChange)
        ctx_.parameterTargetWillChange();
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
    if (e.y < timelineBottomExtended && e.x >= ctx_.getPianoKeyWidth()) {
        timelineSeekActive_ = true;
        timelineSeekTime_ = -1.0;
        armTimelineSeek(e, false);
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
        case ToolId::Eraser:
            handleEraserTool(e);
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
        case ToolId::Eq:
            handleEqToolMouseDown(e);
            break;
        default:
            AppLogger::warn("[PianoRollToolHandler] mouseDown: unknown tool " + juce::String(static_cast<int>(currentTool_)));
            break;
    }
}

void PianoRollToolHandler::mouseDrag(const juce::MouseEvent& e)
{
    if (timelineSeekActive_) {
        armTimelineSeek(e, false);
        return;
    }

    // 空区拖拽→框选：超过阈值直接启动框选（消除 EmptySpaceMouseIntent 平行路径）
    if (ctx_.getState().emptySpaceIntent.active) {
        const auto& intent = ctx_.getState().emptySpaceIntent;
        const int dx = e.x - intent.mouseDownPos.x;
        const int dy = e.y - intent.mouseDownPos.y;
        const int threshold = ctx_.getDragThreshold();
        if (dx * dx + dy * dy > threshold * threshold) {
            const auto tool = intent.tool;
            const auto mouseDownPos = intent.mouseDownPos;
            ctx_.getState().emptySpaceIntent.clear();

            if (tool == ToolId::Select || tool == ToolId::Eq
                || tool == ToolId::Pitch || tool == ToolId::PitchModulation
                || tool == ToolId::PitchDrift) {
                beginAreaSelection(e.withNewPosition(mouseDownPos.toFloat()));
                handleSelectDrag(e);
                return;
            }
            // DrawNote / HandDraw：保持旧路径
            auto startEvent = e.withNewPosition(mouseDownPos.toFloat());
            if (tool == ToolId::DrawNote
                && !AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme())) {
                handleDrawNoteMouseDown(startEvent);
                handleDrawNoteDrag(e);
            } else if (tool == ToolId::HandDraw) {
                handleDrawCurveTool(startEvent);
                handleDrawCurveTool(e);
            }
            return;
        }
        return; // 仍在死区，消费事件
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
        case ToolId::Eraser:
            handleEraserTool(e);
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
        case ToolId::Eq:
            handleEqToolMouseDrag(e);
            break;
        default:
            break;
    }
}

void PianoRollToolHandler::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (ctx_.parameterTargetWillChange)
        ctx_.parameterTargetWillChange();
    if (currentTool_ == ToolId::TimeTool) {
        handleTimeToolMouseDoubleClick(e);
    } else if ((currentTool_ == ToolId::Pitch || currentTool_ == ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift)
               && AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme())) {
        handlePitchToolDoubleClick(e);
    } else if (currentTool_ == ToolId::VolumeEnvelope
               && AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme())) {
        handleVolumeEnvelopeToolDoubleClick(e);
    }
    // Scissors 合并检测已移至 handleScissorsToolUp，通过自定义时间/距离检测
    // （800ms / 30px），不依赖 JUCE mouseDoubleClick 回调。
    // Other tools: no-op (could be extended later for note resize / etc.)
}

void PianoRollToolHandler::mouseUp(const juce::MouseEvent& e)
{
    if (timelineSeekActive_) {
        timelineSeekActive_ = false;
        if (!e.mods.isPopupMenu())
            armTimelineSeek(e, true);
        return;
    }

    if (e.mods.isPopupMenu())
    {
        finishMouseParameterTargetChange();
        return;
    }

    // 空区点击/拖拽完成（消除 EmptySpaceMouseIntent 平行路径）
    if (ctx_.getState().emptySpaceIntent.active) {
        const auto& intent = ctx_.getState().emptySpaceIntent;
        const int dx = e.x - intent.mouseDownPos.x;
        const int dy = e.y - intent.mouseDownPos.y;
        const int threshold = ctx_.getDragThreshold();

        if (dx * dx + dy * dy > threshold * threshold) {
            // 拖拽完成：根据工具类型调用对应 up 处理
            const auto tool = intent.tool;
            ctx_.getState().emptySpaceIntent.clear();

            if (tool == ToolId::Select || tool == ToolId::Eq
                || tool == ToolId::Pitch || tool == ToolId::PitchModulation
                || tool == ToolId::PitchDrift) {
                handleSelectUp(e);
            } else if (tool == ToolId::HandDraw) {
                handleDrawCurveUp(e);
            } else if (tool == ToolId::DrawNote
                && !AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme())) {
                handleDrawNoteUp(e);
            }
            finishMouseParameterTargetChange();
            return;
        }

        // 纯单击空白：取消当前选中 + 清除框选残留状态 + 移动播放头
        beginMouseParameterTargetChange();
        deselectAllNotes();
        ctx_.getState().frameSelection.clear();
        auto& sel = ctx_.getState().selection;
        sel.isSelectingArea = false;
        sel.isSelectingF0 = false;
        sel.f0SelectionAnchorFrame = -1;
        sel.f0SelectionCurrentFrame = -1;
        if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
        finishMouseParameterTargetChange();

        if (intent.mouseDownTime >= 0.0 && ctx_.notifyPlayheadChange) {
            ctx_.notifyPlayheadChange(intent.mouseDownTime, true);
        }

        ctx_.getState().emptySpaceIntent.clear();
        return;
    }

    // 框选完成时（含 Pitch 工具空区拖拽启动的框选），统一走 Select up 处理提交选择。
    if (ctx_.getState().selection.isSelectingArea) {
        handleSelectUp(e);
        finishMouseParameterTargetChange();
        return;
    }

    switch (currentTool_) {
        case ToolId::Select:
            handleSelectUp(e);
            break;
        case ToolId::HandDraw:
            handleDrawCurveUp(e);
            break;
        case ToolId::Eraser:
            handleEraserUp();
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
        case ToolId::Eq:
            handleEqToolMouseUp(e);
            break;
        default:
            break;
    }
    finishMouseParameterTargetChange();
}

bool PianoRollToolHandler::keyPressed(const juce::KeyPress& key,
                                      const KeyShortcutConfig::KeyBinding* pointerShortcut)
{
    if (ctx_.parameterTargetWillChange)
        ctx_.parameterTargetWillChange();
    const bool accessibilityMode = ctx_.accessibilityModeEnabled
        && ctx_.accessibilityModeEnabled();
    if (accessibilityMode ? handleNoteArrowKey(key)
                          : handleNoteArrowKeyPreAccessibility(key))
        return true;

    const auto& shortcutSettings = ctx_.getShortcutSettings();
    const auto matches = [&](KeyShortcutConfig::ShortcutId id) {
        return pointerShortcut != nullptr
            ? KeyShortcutConfig::matchesShortcut(shortcutSettings, id, *pointerShortcut)
            : KeyShortcutConfig::matchesShortcut(shortcutSettings, id, key);
    };

    // OpenDyne（NotesPrimary）工具切换：通过 KeyShortcutConfig 可配置
    const bool isOpenDyne = AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme());
    if (isOpenDyne) {
        if (matches(KeyShortcutConfig::ShortcutId::ToolODSelect)) {
            switchToolFromKeyboard(ToolId::Select);
            return true;
        }
        if (matches(KeyShortcutConfig::ShortcutId::ToolODPitch)) {
            switchToolFromKeyboard(ToolId::Pitch);
            return true;
        }
        if (matches(KeyShortcutConfig::ShortcutId::ToolODPitchModulation)) {
            switchToolFromKeyboard(ToolId::PitchModulation);
            return true;
        }
        if (matches(KeyShortcutConfig::ShortcutId::ToolODPitchDrift)) {
            switchToolFromKeyboard(ToolId::PitchDrift);
            return true;
        }
        if (matches(KeyShortcutConfig::ShortcutId::ToolODVolumeEnvelope)) {
            switchToolFromKeyboard(ToolId::VolumeEnvelope);
            return true;
        }
        if (matches(KeyShortcutConfig::ShortcutId::ToolODScissors)) {
            switchToolFromKeyboard(ToolId::Scissors);
            return true;
        }
    }

    if (matches(KeyShortcutConfig::ShortcutId::SelectAll)) {
        const auto& notes = committedNotes(ctx_);
        const auto contentSnapshot = ctx_.getEditableContentSnapshot();
        bool hasNotes = !notes.empty();
        bool hasCurve = contentSnapshot != nullptr
            && contentSnapshot->pitchCurve != nullptr
            && !contentSnapshot->pitchCurve->isEmpty();
        
        if (!hasNotes && !hasCurve) {
            notifyKeyboardNote(KeyboardNoteEvent::NoNotes, notes, -1);
            return true;
        }

        if (hasNotes) {
            const auto before = beginParameterTargetChange();
            selectAllNotes(notes);
            finishParameterTargetChange(before);
        }

        // F0 范围从 noteSelection 按需派生，无需同步。
        if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
        // Accessibility: without this, selecting all from the keyboard gives no feedback.
        notifyKeyboardNote(hasNotes ? KeyboardNoteEvent::AllSelected : KeyboardNoteEvent::NoNotes, notes, -1);
        return true;
    }

    // ==== Tool switching (via configurable shortcuts) ====
    // OpenTune（PitchPrimary）工具切换走可配置快捷键；OpenDyne（NotesPrimary）用固定 F1/F2/F4/F6，
    // DrawNote/LineAnchor 只在 OpenTune 分支生效，绝不穿透到 OpenDyne。
    // HandDraw 两种模式共用（OpenDyne 下自由绘制音高曲线）。
    // ToolTimeTool/ToolAutoTune 与通用 SelectAll/Delete/CancelSelection 两 scheme 共用。
    if (!isOpenDyne) {
        if (matches(KeyShortcutConfig::ShortcutId::ToolDrawNote)) {
            switchToolFromKeyboard(ToolId::DrawNote);
            return true;
        }

        if (matches(KeyShortcutConfig::ShortcutId::ToolSelect)) {
            switchToolFromKeyboard(ToolId::Select);
            return true;
        }

        if (matches(KeyShortcutConfig::ShortcutId::ToolLineAnchor)) {
            switchToolFromKeyboard(ToolId::LineAnchor);
            return true;
        }
    }

    // EQ 工具两种模式共用（ShortcutId::Eq 可配置，默认 E）
    if (matches(KeyShortcutConfig::ShortcutId::Eq)) {
        switchToolFromKeyboard(ToolId::Eq);
        return true;
    }

    // HandDraw 两种模式共用
    if (matches(KeyShortcutConfig::ShortcutId::ToolHandDraw)) {
        switchToolFromKeyboard(ToolId::HandDraw);
        return true;
    }

    if (matches(KeyShortcutConfig::ShortcutId::ToolTimeTool)) {
        switchToolFromKeyboard(ToolId::TimeTool);
        return true;
    }

    if (matches(KeyShortcutConfig::ShortcutId::ToolAutoTune)) {
        ctx_.notifyAutoTuneRequested();
        return true;
    }
    // ==== End tool switching ====

    if (ctx_.transportShortcutsEnabled) {
        if (matches(KeyShortcutConfig::ShortcutId::PlayPause)) {
            ctx_.notifyPlayPauseToggle();
            return true;
        }

        if (matches(KeyShortcutConfig::ShortcutId::Stop)) {
            ctx_.notifyStopPlayback();
            return true;
        }
    }

    if (matches(KeyShortcutConfig::ShortcutId::Delete)) {
        // Time tool always consumes Delete to avoid accidentally deleting notes
        // when a handle isn't selected. No-op when nothing's selected.
        if (currentTool_ == ToolId::TimeTool) {
            handleTimeToolDeleteSelected();
            return true;
        }
        handleDeleteKey();
        return true;
    }

    if (matches(KeyShortcutConfig::ShortcutId::CancelSelection))
        return ctx_.notifyEscapeKey == nullptr || ctx_.notifyEscapeKey();

    return false;
}

bool PianoRollToolHandler::handleNoteArrowKeyPreAccessibility(const juce::KeyPress& key)
{
    if (key.getModifiers().isAnyModifierKeyDown())
        return false;

    const int keyCode = key.getKeyCode();
    if (keyCode == juce::KeyPress::upKey || keyCode == juce::KeyPress::downKey) {
        moveSelectedNotesPitchByKeyboard(keyCode == juce::KeyPress::upKey ? 1 : -1);
        return true;
    }
    if (keyCode != juce::KeyPress::leftKey && keyCode != juce::KeyPress::rightKey)
        return false;

    const auto& notes = committedNotes(ctx_);
    const auto selectedIndices = collectSelectedNoteIndices(notes);
    if (selectedIndices.empty())
        return true;

    const bool moveRight = keyCode == juce::KeyPress::rightKey;
    double boundaryTime = notes[static_cast<size_t>(selectedIndices.front())].startTime;
    for (int index : selectedIndices) {
        const double startTime = notes[static_cast<size_t>(index)].startTime;
        boundaryTime = moveRight ? std::max(boundaryTime, startTime)
                                 : std::min(boundaryTime, startTime);
    }

    int adjacentIndex = -1;
    for (int index = 0; index < static_cast<int>(notes.size()); ++index) {
        const double startTime = notes[static_cast<size_t>(index)].startTime;
        if (moveRight && startTime > boundaryTime
            && (adjacentIndex < 0 || startTime < notes[static_cast<size_t>(adjacentIndex)].startTime)) {
            adjacentIndex = index;
        } else if (!moveRight && startTime < boundaryTime
                   && (adjacentIndex < 0 || startTime > notes[static_cast<size_t>(adjacentIndex)].startTime)) {
            adjacentIndex = index;
        }
    }

    if (adjacentIndex >= 0) {
        const auto before = beginParameterTargetChange();
        ctx_.getState().noteSelection.setSingle(adjacentIndex, static_cast<int>(notes.size()));
        if (ctx_.invalidateSelectionFeedback)
            ctx_.invalidateSelectionFeedback();
        finishParameterTargetChange(before);
    }
    return true;
}

bool PianoRollToolHandler::handleNoteArrowKey(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const int keyCode = key.getKeyCode();
    const bool plain = !mods.isAnyModifierKeyDown();
    const bool shiftOnly = mods.isShiftDown() && !mods.isCtrlDown() && !mods.isAltDown() && !mods.isCommandDown();
    const bool altOnly = mods.isAltDown() && !mods.isShiftDown() && !mods.isCtrlDown() && !mods.isCommandDown();
    // Values change with Ctrl+Up/Down (as in Melodyne), Ctrl+Shift for a fine step, so a plain
    // arrow pressed while exploring changes nothing. Ctrl+Alt+Up/Down stays the vertical zoom.
    const bool ctrlOnly = (mods.isCtrlDown() || mods.isCommandDown()) && !mods.isShiftDown() && !mods.isAltDown();
    const bool ctrlShift = (mods.isCtrlDown() || mods.isCommandDown()) && mods.isShiftDown() && !mods.isAltDown();
    const bool upOrDown = keyCode == juce::KeyPress::upKey || keyCode == juce::KeyPress::downKey;
    const bool isInfoKey = keyCode == 'I' || keyCode == 'i';
    const bool isResetKey = keyCode == 'R' || keyCode == 'r';
    const bool isSplitKey = keyCode == 'X' || keyCode == 'x';
    const bool isMergeKey = keyCode == 'J' || keyCode == 'j';
    const bool isHomeOrEnd = keyCode == juce::KeyPress::homeKey || keyCode == juce::KeyPress::endKey;
    const bool openDyne = AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme());

    // Enter mirrors the OpenDyne double-click on a note: with the modulation or drift tool
    // it toggles 100% / 0%, otherwise it snaps the note to the pitch grid. Without a selected
    // note Enter stays "Stop" as documented, so it is not consumed here.
    if (plain && keyCode == juce::KeyPress::returnKey && openDyne)
        return applyNoteDefaultActionByKeyboard();

    // Extra mouse-free keys (Ctrl/Alt+Up/Down, Home, End, I, R, X, J) do not override a binding
    // the user assigned to something else in the shortcut settings.
    const bool leftOrRight = keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::rightKey;
    // Ctrl+Left/Right move the end of the note, Ctrl+Shift+Left/Right its start.
    const bool resizeKey = leftOrRight && mods.isCtrlDown() && !mods.isAltDown();
    const bool extraKey = ((altOnly || ctrlOnly || ctrlShift) && upOrDown) || (shiftOnly && leftOrRight) || resizeKey
        || (shiftOnly && (isSplitKey || isInfoKey))
        || (plain && (isInfoKey || isResetKey || isSplitKey || isMergeKey || isHomeOrEnd));
    if (extraKey
        && KeyShortcutConfig::findShortcutForBinding(ctx_.getShortcutSettings(),
                                                     KeyShortcutConfig::KeyBinding(keyCode, mods),
                                                     openDyne ? KeyShortcutConfig::kPianoRollOpenDyneScope
                                                              : KeyShortcutConfig::kPianoRollOpenTuneScope)
               != KeyShortcutConfig::ShortcutId::Count)
        return false;

    if (upOrDown && (plain || altOnly || ctrlOnly || ctrlShift)) {
        const int direction = keyCode == juce::KeyPress::upKey ? 1 : -1;
        const bool modDrift = openDyne && (currentTool_ == ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift);
        const bool volume = openDyne && currentTool_ == ToolId::VolumeEnvelope;
        // A plain arrow, or Alt where there are no semitones, only says which keys edit.
        if (plain || (altOnly && (modDrift || volume))) {
            notifyKeyboardNote(KeyboardNoteEvent::ArrowHint, committedNotes(ctx_), -1);
            return true;
        }
        // Like mouse dragging: the arrows change whatever the current tool edits.
        if (modDrift)
            adjustSelectedNotesModDriftByKeyboard(direction, ctrlShift);   // 5%, fine 1%
        else if (volume)
            adjustSelectedNotesVolumeByKeyboard(direction, ctrlShift);     // 1 dB, fine 0.1 dB
        else if (altOnly)
            // Alt: a semitone even when the key/scale grid would skip notes outside the scale.
            moveSelectedNotesPitchByKeyboard(direction, false, false, true);
        else
            // Ctrl: next step of the pitch grid; Ctrl+Shift: 1 cent without the grid.
            moveSelectedNotesPitchByKeyboard(direction, ctrlShift);
        return true;
    }
    if (resizeKey) {
        resizeSelectedNoteByKeyboard(mods.isShiftDown(), keyCode == juce::KeyPress::rightKey ? 1 : -1);
        return true;
    }
    if (shiftOnly && isSplitKey) {
        splitSelectedNoteByKeyboard(true);
        return true;
    }
    if (shiftOnly && isInfoKey) {
        notifyKeyboardNote(KeyboardNoteEvent::PositionInfo, committedNotes(ctx_), -1);
        return true;
    }
    // Shift+Left/Right extend the selection, like Shift+click on a note.
    if (shiftOnly && leftOrRight) {
        extendSelectionByKeyboard(keyCode == juce::KeyPress::rightKey);
        return true;
    }
    if (!plain)
        return false;

    if (isSplitKey) {
        splitSelectedNoteByKeyboard();
        return true;
    }
    if (isMergeKey) {
        mergeSelectedNoteWithNext();
        return true;
    }

    const auto& notes = committedNotes(ctx_);
    const int noteCount = static_cast<int>(notes.size());

    if (isInfoKey) {
        const auto selected = collectSelectedNoteIndices(notes);
        if (noteCount == 0)
            notifyKeyboardNote(KeyboardNoteEvent::NoNotes, notes, -1);
        else if (selected.empty())
            notifyKeyboardNote(KeyboardNoteEvent::NoSelection, notes, -1);
        else
            notifyKeyboardNote(KeyboardNoteEvent::Info, notes, selected.front());
        return true;
    }

    // R: back to the sung pitch. Ctrl+Z only goes back step by step, so after many edits
    // there was no quick way to return one note to how it was sung.
    if (isResetKey) {
        moveSelectedNotesPitchByKeyboard(0, false, true);
        return true;
    }

    if (!isHomeOrEnd && keyCode != juce::KeyPress::leftKey && keyCode != juce::KeyPress::rightKey)
        return false;

    if (noteCount == 0) {
        notifyKeyboardNote(KeyboardNoteEvent::NoNotes, notes, -1);
        return true;
    }

    // First / last note by start time; array order is not treated as time order.
    const auto edgeNote = [&notes, noteCount](bool last) {
        int edge = 0;
        for (int index = 1; index < noteCount; ++index) {
            const double startTime = notes[static_cast<size_t>(index)].startTime;
            const double edgeTime = notes[static_cast<size_t>(edge)].startTime;
            if (last ? startTime > edgeTime : startTime < edgeTime)
                edge = index;
        }
        return edge;
    };

    if (isHomeOrEnd) {
        selectNoteFromKeyboard(notes, edgeNote(keyCode == juce::KeyPress::endKey));
        return true;
    }

    const auto selectedIndices = collectSelectedNoteIndices(notes);
    // The place in the song moved since the last arrow (playback paused somewhere else, or the
    // edit cursor was moved in REAPER): start from there, as without a selection. Unknown
    // positions (during playback) are not remembered, so a pause after them still counts.
    const auto cursor = ctx_.getEditCursorSourceTime ? ctx_.getEditCursorSourceTime() : std::nullopt;
    // Nothing remembered yet (the window was just opened): follow the cursor too.
    const bool cursorMoved = cursor && (!lastArrowCursor_ || std::abs(*cursor - *lastArrowCursor_) > 1.0e-6);
    if (cursor)
        lastArrowCursor_ = cursor;
    // The note at the cursor is already the selected one: move on to its neighbour as usual,
    // otherwise the arrow would only repeat the same note and seem to do nothing.
    const auto cursorIsInSelection = [&] {
        for (int index : selectedIndices) {
            const auto& note = notes[static_cast<size_t>(index)];
            if (note.startTime <= *cursor && *cursor < note.endTime)
                return true;
        }
        return false;
    };
    if (selectedIndices.empty() || (cursorMoved && !cursorIsInSelection())) {
        // Start from REAPER's edit cursor when it is known, so the user lands where they are
        // in the song: Right takes the note under the cursor or the next one, Left the note
        // under the cursor or the previous one. Past the last or before the first note, the
        // nearest note. Without a cursor: the first note.
        if (cursor) {
            const bool right = keyCode == juce::KeyPress::rightKey;
            int target = -1;
            for (int index = 0; index < noteCount; ++index) {
                const auto& note = notes[static_cast<size_t>(index)];
                const bool candidate = right ? note.endTime > *cursor : note.startTime < *cursor;
                if (!candidate)
                    continue;
                const double startTime = note.startTime;
                if (target < 0 || (right ? startTime < notes[static_cast<size_t>(target)].startTime
                                         : startTime > notes[static_cast<size_t>(target)].startTime))
                    target = index;
            }
            selectNoteFromKeyboard(notes, target >= 0 ? target : edgeNote(right));
            return true;
        }
        // Without a selection the arrows used to do nothing, and there was no mouse-free way to select.
        selectNoteFromKeyboard(notes, edgeNote(false));
        return true;
    }

    const bool moveRight = keyCode == juce::KeyPress::rightKey;
    // All notes selected (Ctrl+A): the selection has no neighbours, so the arrows would be silent.
    // Right goes to the first note, Left to the last one.
    if (noteCount > 1 && static_cast<int>(selectedIndices.size()) == noteCount) {
        selectNoteFromKeyboard(notes, edgeNote(!moveRight));
        return true;
    }

    double boundaryTime = notes[static_cast<size_t>(selectedIndices.front())].startTime;
    for (int index : selectedIndices) {
        const double startTime = notes[static_cast<size_t>(index)].startTime;
        boundaryTime = moveRight ? std::max(boundaryTime, startTime)
                                 : std::min(boundaryTime, startTime);
    }

    int adjacentIndex = -1;
    for (int index = 0; index < static_cast<int>(notes.size()); ++index) {
        const double startTime = notes[static_cast<size_t>(index)].startTime;
        if (moveRight && startTime > boundaryTime
            && (adjacentIndex < 0 || startTime < notes[static_cast<size_t>(adjacentIndex)].startTime)) {
            adjacentIndex = index;
        } else if (!moveRight && startTime < boundaryTime
                   && (adjacentIndex < 0 || startTime > notes[static_cast<size_t>(adjacentIndex)].startTime)) {
            adjacentIndex = index;
        }
    }

    // Stay silent at the ends of the list.
    if (adjacentIndex >= 0)
        selectNoteFromKeyboard(notes, adjacentIndex);
    return true;
}

// Keyboard version of dragging a note edge with the Select tool (handleSelectDrag/Up):
// 10 ms per press, the same minimum length and the same rule for the touching neighbour,
// which yields its boundary but keeps its own minimum length.
void PianoRollToolHandler::resizeSelectedNoteByKeyboard(bool startEdge, int direction)
{
    const auto& notes = committedNotes(ctx_);
    const auto selected = collectSelectedNoteIndices(notes);
    if (selected.empty()) {
        notifyKeyboardNote(notes.empty() ? KeyboardNoteEvent::NoNotes : KeyboardNoteEvent::NoSelection, notes, -1);
        return;
    }
    if (selected.size() != 1) {
        notifyKeyboardNote(KeyboardNoteEvent::SingleNoteNeeded, notes, -1);
        return;
    }

    constexpr double step = 0.01;
    const int idx = selected.front();
    const auto beforeNotes = notes;
    const auto& old = beforeNotes[static_cast<size_t>(idx)];
    const auto editRange = sourceEditRange();
    const auto edge = startEdge ? NoteResizeEdge::Left : NoteResizeEdge::Right;
    const double clampedTime = juce::jlimit(editRange.startSeconds, editRange.endSeconds,
                                            (startEdge ? old.startTime : old.endTime) + direction * step);

    // The same planner as the mouse: minimum length, the neighbour yields down to its own minimum.
    const auto plan = planNoteResizeEdit(beforeNotes, idx, edge, clampedTime, kNoteResizeMinDuration);
    const auto& changed = plan.afterNotes[static_cast<size_t>(idx)];
    // Limit reached (minimum length, neighbour or clip edge): stay silent, as at list ends.
    if (changed.startTime == old.startTime && changed.endTime == old.endTime)
        return;

    // Same transaction as a mouse resize (revision check, one undo step).
    const auto expectedRevision = ctx_.getNotesRevision ? ctx_.getNotesRevision() : 0;
    const auto commitResize = [this, expectedRevision, idx, edge, clampedTime]
        (const std::vector<PitchCorrectionSegment>& segments, F0FrameRange affectedRange) {
        return ctx_.commitNoteResize
            && ctx_.commitNoteResize(expectedRevision, idx, edge, clampedTime, kNoteResizeMinDuration,
                                     segments, affectedRange, LOC_RAW("Resize note"));
    };
    const auto contentSnapshot = ctx_.getEditableContentSnapshot();
    const F0FrameRange f0Range = ctx_.getF0Timeline().rangeForTimes(plan.dirtyStartTime, plan.dirtyEndTime);
    const bool committed = (contentSnapshot != nullptr && contentSnapshot->pitchCurve != nullptr && !f0Range.isEmpty())
                               ? commitNoteBasedCorrection(ctx_, plan.afterNotes, f0Range, commitResize)
                               : commitResize({}, {});
    if (!committed) {
        notifyKeyboardNote(KeyboardNoteEvent::EditFailed, committedNotes(ctx_), -1);
        return;
    }
    if (ctx_.invalidateLiveNotes) ctx_.invalidateLiveNotes(beforeNotes, committedNotes(ctx_));
    notifyKeyboardNote(KeyboardNoteEvent::NoteResized, committedNotes(ctx_), idx);
}

// Shift+Left/Right, as in Windows lists: the anchor stays, the other end moves one note.
// Moving back towards the anchor shrinks the range.
void PianoRollToolHandler::extendSelectionByKeyboard(bool moveRight)
{
    const auto& notes = committedNotes(ctx_);
    const int noteCount = static_cast<int>(notes.size());
    if (noteCount == 0) {
        notifyKeyboardNote(KeyboardNoteEvent::NoNotes, notes, -1);
        return;
    }

    // Time order: array order is not treated as time order.
    std::vector<int> order(static_cast<size_t>(noteCount));
    for (int i = 0; i < noteCount; ++i)
        order[static_cast<size_t>(i)] = i;
    std::stable_sort(order.begin(), order.end(), [&notes](int a, int b) {
        return notes[static_cast<size_t>(a)].startTime < notes[static_cast<size_t>(b)].startTime;
    });
    std::vector<int> position(static_cast<size_t>(noteCount));
    for (int i = 0; i < noteCount; ++i)
        position[static_cast<size_t>(order[static_cast<size_t>(i)])] = i;
    const auto posOf = [&position](int index) { return position[static_cast<size_t>(index)]; };

    const auto selected = collectSelectedNoteIndices(notes);
    if (selected.empty()) {
        const int first = order[static_cast<size_t>(moveRight ? 0 : noteCount - 1)];
        keyboardRangeAnchor_ = keyboardRangeActive_ = first;
        const auto before = beginParameterTargetChange();
        ctx_.getState().noteSelection.setSingle(first, noteCount);
        if (ctx_.invalidateSelectionFeedback)
            ctx_.invalidateSelectionFeedback();
        notifyKeyboardNote(KeyboardNoteEvent::Selected, notes, first);
        finishParameterTargetChange(before);
        return;
    }

    // Keep the anchor only while the selection is still exactly the range made with Shift;
    // after a click, an edit or plain arrows start again from the current selection.
    bool rangeStillValid = keyboardRangeAnchor_ >= 0 && keyboardRangeAnchor_ < noteCount
                        && keyboardRangeActive_ >= 0 && keyboardRangeActive_ < noteCount;
    if (rangeStillValid) {
        const int lo = std::min(posOf(keyboardRangeAnchor_), posOf(keyboardRangeActive_));
        const int hi = std::max(posOf(keyboardRangeAnchor_), posOf(keyboardRangeActive_));
        rangeStillValid = static_cast<int>(selected.size()) == hi - lo + 1;
        for (int index : selected)
            rangeStillValid = rangeStillValid && posOf(index) >= lo && posOf(index) <= hi;
    }
    if (!rangeStillValid) {
        int minPos = noteCount, maxPos = -1;
        for (int index : selected) {
            minPos = std::min(minPos, posOf(index));
            maxPos = std::max(maxPos, posOf(index));
        }
        keyboardRangeAnchor_ = order[static_cast<size_t>(moveRight ? minPos : maxPos)];
        keyboardRangeActive_ = order[static_cast<size_t>(moveRight ? maxPos : minPos)];
    }

    // Stay silent at the ends of the list.
    const int next = posOf(keyboardRangeActive_) + (moveRight ? 1 : -1);
    if (next < 0 || next >= noteCount)
        return;
    keyboardRangeActive_ = order[static_cast<size_t>(next)];

    const int lo = std::min(posOf(keyboardRangeAnchor_), next);
    const int hi = std::max(posOf(keyboardRangeAnchor_), next);
    std::vector<int> range;
    for (int p = lo; p <= hi; ++p)
        range.push_back(order[static_cast<size_t>(p)]);
    const auto before = beginParameterTargetChange();
    ctx_.getState().noteSelection.setFromIndices(std::move(range), noteCount);
    if (ctx_.invalidateSelectionFeedback)
        ctx_.invalidateSelectionFeedback();
    finishParameterTargetChange(before);
    notifyKeyboardNote(KeyboardNoteEvent::RangeSelected, notes, keyboardRangeActive_);
}

void PianoRollToolHandler::selectNoteFromKeyboard(const std::vector<Note>& notes, int index)
{
    const auto before = beginParameterTargetChange();
    ctx_.getState().noteSelection.setSingle(index, static_cast<int>(notes.size()));
    if (ctx_.invalidateSelectionFeedback)
        ctx_.invalidateSelectionFeedback();
    notifyKeyboardNote(KeyboardNoteEvent::Selected, notes, index);
    finishParameterTargetChange(before);
}

void PianoRollToolHandler::notifyKeyboardNote(KeyboardNoteEvent event, const std::vector<Note>& notes, int index)
{
    if (ctx_.accessibilityModeEnabled && !ctx_.accessibilityModeEnabled())
        return;
    if (ctx_.notifyKeyboardNote)
        ctx_.notifyKeyboardNote(event, notes, index);
}

// X: split the selected note where its sung pitch changes the most - usually where two sung
// notes were merged into one. Without a clear pitch change the note is split in half.
// Shift+X splits at REAPER's edit cursor instead, when it is inside the note (the cursor
// is read from REAPER while stopped, see ReaperHostBridge). The split itself uses the
// scissors planner (planScissorsSplit).
void PianoRollToolHandler::splitSelectedNoteByKeyboard(bool atEditCursor)
{
    const auto beforeNotes = committedNotes(ctx_);
    if (beforeNotes.empty()) {
        notifyKeyboardNote(KeyboardNoteEvent::NoNotes, beforeNotes, -1);
        return;
    }
    const auto selected = collectSelectedNoteIndices(beforeNotes);
    if (selected.empty()) {
        notifyKeyboardNote(KeyboardNoteEvent::NoSelection, beforeNotes, -1);
        return;
    }

    const int index = selected.front();
    const Note& original = beforeNotes[static_cast<size_t>(index)];

    // Parts shorter than 20 ms are useless.
    static constexpr double kMinPartSeconds = 0.02;
    if (original.getDuration() < 2.0 * kMinPartSeconds) {
        notifyKeyboardNote(KeyboardNoteEvent::NoteTooShort, beforeNotes, index);
        return;
    }

    double splitTime = 0.5 * (original.startTime + original.endTime);
    bool foundPitchChange = false;
    const auto f0tl = ctx_.getF0Timeline();
    const auto originalF0 = ctx_.getOriginalF0 ? ctx_.getOriginalF0() : std::vector<float>{};
    if (atEditCursor) {
        const auto cursor = ctx_.getEditCursorSourceTime ? ctx_.getEditCursorSourceTime() : std::nullopt;
        if (!cursor) {
            notifyKeyboardNote(KeyboardNoteEvent::CursorUnknown, beforeNotes, index);
            return;
        }
        if (*cursor <= original.startTime + kMinPartSeconds || *cursor >= original.endTime - kMinPartSeconds) {
            notifyKeyboardNote(KeyboardNoteEvent::CursorOutsideNote, beforeNotes, index);
            return;
        }
        splitTime = *cursor;
    } else if (!originalF0.empty() && !f0tl.isEmpty()) {
        const auto range = f0tl.rangeForTimes(original.startTime, original.endTime);
        const int frameCount = static_cast<int>(originalF0.size());
        // Mean sung pitch (MIDI) of the voiced frames in [from, to) inside the note.
        const auto meanMidi = [&](int from, int to) -> std::optional<float> {
            float sum = 0.0f;
            int count = 0;
            for (int frame = std::max(from, range.startFrame);
                 frame < std::min({ to, range.endFrameExclusive, frameCount }); ++frame) {
                const float hz = originalF0[static_cast<size_t>(frame)];
                if (hz > 0.0f) {
                    sum += PitchUtils::freqToMidi(hz);
                    ++count;
                }
            }
            if (count < 2)
                return std::nullopt;
            return sum / static_cast<float>(count);
        };

        // Compare the mean pitch just before and just after each frame (~45 ms each side
        // at the usual hop) and pick the frame with the largest jump.
        constexpr int kWindowFrames = 4;
        float bestJump = 0.0f;
        int bestFrame = -1;
        for (int frame = range.startFrame + 1; frame < range.endFrameExclusive; ++frame) {
            const double time = f0tl.timeAtFrame(frame);
            if (time <= original.startTime + kMinPartSeconds || time >= original.endTime - kMinPartSeconds)
                continue;
            const auto before = meanMidi(frame - kWindowFrames, frame);
            const auto after = meanMidi(frame, frame + kWindowFrames);
            if (!before || !after)
                continue;
            const float jump = std::abs(*after - *before);
            if (jump > bestJump) {
                bestJump = jump;
                bestFrame = frame;
            }
        }
        // Less than half a semitone is vibrato or drift, not a second note.
        if (bestFrame >= 0 && bestJump >= 0.5f) {
            splitTime = f0tl.timeAtFrame(bestFrame);
            foundPitchChange = true;
        }
    }

    // The parts and their pitch centres come from the scissors planner, applied to the
    // selected note only (the scissors cut every note under the line).
    const auto plan = planScissorsSplit({ original }, splitTime, loadEffectiveF0(), f0tl);
    if (plan.cutCount == 0 || plan.afterNotes.size() != 2) {
        notifyKeyboardNote(KeyboardNoteEvent::EditFailed, beforeNotes, -1);
        return;
    }

    std::vector<Note> newNotes;
    newNotes.reserve(beforeNotes.size() + 1);
    for (int i = 0; i < static_cast<int>(beforeNotes.size()); ++i) {
        if (i == index)
            newNotes.insert(newNotes.end(), plan.afterNotes.begin(), plan.afterNotes.end());
        else
            newNotes.push_back(beforeNotes[static_cast<size_t>(i)]);
    }

    // Same transaction as the scissors: revision check, notes replaced, one undo step.
    const auto before = beginParameterTargetChange();
    if (!ctx_.commitScissorsNotes || !ctx_.commitScissorsNotes(newNotes, LOC_RAW("Split note"))) {
        notifyKeyboardNote(KeyboardNoteEvent::EditFailed, committedNotes(ctx_), -1);
        return;
    }

    // Select the left part so Right arrow leads straight to the right part.
    ctx_.getState().noteSelection.setSingle(index, static_cast<int>(newNotes.size()));
    finishParameterTargetChange(before, true);
    if (ctx_.invalidateSelectionFeedback)
        ctx_.invalidateSelectionFeedback();
    notifyKeyboardNote(atEditCursor       ? KeyboardNoteEvent::SplitAtCursor
                       : foundPitchChange ? KeyboardNoteEvent::Split
                                          : KeyboardNoteEvent::SplitInHalf,
                       newNotes, index);
}

// J: merge the selected note with the next one if they touch. Same as double-clicking
// the boundary with the scissors (handleScissorsToolMerge).
void PianoRollToolHandler::mergeSelectedNoteWithNext()
{
    const auto beforeNotes = committedNotes(ctx_);
    if (beforeNotes.empty()) {
        notifyKeyboardNote(KeyboardNoteEvent::NoNotes, beforeNotes, -1);
        return;
    }
    const auto selected = collectSelectedNoteIndices(beforeNotes);
    if (selected.empty()) {
        notifyKeyboardNote(KeyboardNoteEvent::NoSelection, beforeNotes, -1);
        return;
    }

    const int index = selected.front();
    const Note& left = beforeNotes[static_cast<size_t>(index)];
    int nextIndex = -1;
    for (int i = 0; i < static_cast<int>(beforeNotes.size()); ++i) {
        const double startTime = beforeNotes[static_cast<size_t>(i)].startTime;
        if (startTime > left.startTime
            && (nextIndex < 0 || startTime < beforeNotes[static_cast<size_t>(nextIndex)].startTime))
            nextIndex = i;
    }

    // "Touching" means a gap of at most 20 ms; notes across a pause are not merged, as with the mouse.
    constexpr double kTouchToleranceSeconds = 0.02;
    if (nextIndex < 0
        || beforeNotes[static_cast<size_t>(nextIndex)].startTime - left.endTime > kTouchToleranceSeconds) {
        notifyKeyboardNote(KeyboardNoteEvent::NoAdjacentNote, beforeNotes, index);
        return;
    }

    const Note& right = beforeNotes[static_cast<size_t>(nextIndex)];
    // The scissors planner merges at a boundary. The gap may be up to the touch tolerance,
    // so the boundary is its middle and the planner's tolerance covers both note edges.
    const double boundary = 0.5 * (left.endTime + right.startTime);
    const double tolerance = 0.5 * std::abs(right.startTime - left.endTime) + 1.0e-6;
    const auto plan = planScissorsMerge({ left, right }, boundary, tolerance,
                                        loadEffectiveF0(), ctx_.getF0Timeline());
    if (plan.mergeCount == 0 || plan.afterNotes.size() != 1) {
        notifyKeyboardNote(KeyboardNoteEvent::NoAdjacentNote, beforeNotes, index);
        return;
    }

    std::vector<Note> newNotes;
    newNotes.reserve(beforeNotes.size());
    int mergedIndex = -1;
    for (int i = 0; i < static_cast<int>(beforeNotes.size()); ++i) {
        if (i == nextIndex)
            continue;
        if (i == index) {
            mergedIndex = static_cast<int>(newNotes.size());
            newNotes.push_back(plan.afterNotes.front());
        } else {
            newNotes.push_back(beforeNotes[static_cast<size_t>(i)]);
        }
    }

    // Same transaction as the scissors: revision check, notes replaced, one undo step.
    const auto before = beginParameterTargetChange();
    if (!ctx_.commitScissorsNotes || !ctx_.commitScissorsNotes(newNotes, LOC_RAW("Merge notes"))) {
        notifyKeyboardNote(KeyboardNoteEvent::EditFailed, committedNotes(ctx_), -1);
        return;
    }

    ctx_.getState().noteSelection.setSingle(mergedIndex, static_cast<int>(newNotes.size()));
    finishParameterTargetChange(before, true);
    if (ctx_.invalidateSelectionFeedback)
        ctx_.invalidateSelectionFeedback();
    notifyKeyboardNote(KeyboardNoteEvent::Merged, newNotes, mergedIndex);
}

// Shared commit path for keyboard note edits: the same commitNoteBasedCorrection
// as the mouse, over the range of the changed notes.
bool PianoRollToolHandler::commitKeyboardNoteEdit(const std::vector<Note>& work,
                                                  const std::vector<int>& changedIndices,
                                                  const juce::String& undoDescription)
{
    double rangeStart = std::numeric_limits<double>::max();
    double rangeEnd = std::numeric_limits<double>::lowest();
    for (int index : changedIndices) {
        rangeStart = std::min(rangeStart, work[static_cast<size_t>(index)].startTime);
        rangeEnd = std::max(rangeEnd, work[static_cast<size_t>(index)].endTime);
    }

    const F0FrameRange editRange = ctx_.getF0Timeline().rangeForTimes(rangeStart, rangeEnd);
    const auto contentSnapshot = ctx_.getEditableContentSnapshot();
    if (changedIndices.empty() || editRange.isEmpty()
        || contentSnapshot == nullptr || contentSnapshot->pitchCurve == nullptr) {
        notifyKeyboardNote(KeyboardNoteEvent::EditFailed, committedNotes(ctx_), -1);
        return false;
    }

    ctx_.beginNoteDraft();
    auto& draft = ctx_.getNoteDraft();
    draft.contentDirty = true;
    draft.workingNotes = work;
    ctx_.setUndoDescription(undoDescription);
    const bool committed = commitNoteBasedCorrection(ctx_, work, editRange);
    ctx_.clearNoteDraft();
    if (!committed) {
        ctx_.setUndoDescription({});
        // Every caller is a keyboard edit, so the failure is said here once.
        notifyKeyboardNote(KeyboardNoteEvent::EditFailed, committedNotes(ctx_), -1);
    }
    return committed;
}

// Up/Down with the Pitch Modulation or Pitch Drift tool: 5% steps, 1% with Alt.
// Values and their meaning match mouse dragging (dragNotePitch).
void PianoRollToolHandler::adjustSelectedNotesModDriftByKeyboard(int direction, bool fine)
{
    const auto& notes = committedNotes(ctx_);
    const auto selected = collectSelectedNoteIndices(notes);
    if (selected.empty()) {
        notifyKeyboardNote(notes.empty() ? KeyboardNoteEvent::NoNotes : KeyboardNoteEvent::NoSelection,
                           notes, -1);
        return;
    }

    const bool modulation = currentTool_ == ToolId::PitchModulation;
    const auto event = modulation ? KeyboardNoteEvent::ModulationChanged : KeyboardNoteEvent::DriftChanged;
    const float step = static_cast<float>(direction) * (fine ? 0.01f : 0.05f);

    std::vector<Note> work = notes;
    std::vector<int> changed;
    for (int index : selected) {
        auto& note = work[static_cast<size_t>(index)];
        if (modulation) {
            // A negative value means "follow the global Retune Speed knob".
            const float effective = note.retuneSpeed >= 0.0f ? note.retuneSpeed : ctx_.getRetuneSpeed();
            const float target = juce::jlimit(0.0f, 1.0f, effective + step);
            if (std::abs(target - effective) <= 0.001f)
                continue;
            note.retuneSpeed = target;
        } else {
            // Drift is not clamped to +/-100%, as with the mouse (Melodyne allows 224%, -112%, etc.).
            // Up increases the number that the screen reader announces.
            note.pitchDriftScale += step;
        }
        note.dirty = true;
        changed.push_back(index);
    }

    if (changed.empty()) {
        // At the end of the range (0% or 100% modulation) just announce the current value.
        notifyKeyboardNote(event, notes, selected.front());
        return;
    }
    if (commitKeyboardNoteEdit(work, changed,
                               modulation ? LOC_RAW("Modulation depth") : LOC_RAW("Drift correction")))
        notifyKeyboardNote(event, work, changed.front());
}

// Enter: like double-clicking a note (handlePitchToolDoubleClick) but for the selected notes.
// Returns false when nothing is selected so Enter keeps working as "Stop".
bool PianoRollToolHandler::applyNoteDefaultActionByKeyboard()
{
    const auto& notes = committedNotes(ctx_);
    const auto selected = collectSelectedNoteIndices(notes);
    if (selected.empty())
        return false;

    // EQ: a click on a note opens the EQ window, so Enter does the same instead of snapping the pitch.
    if (currentTool_ == ToolId::Eq) {
        const int anchor = ctx_.getState().noteSelection.anchorIndex;
        const bool anchorSelected = std::find(selected.begin(), selected.end(), anchor) != selected.end();
        if (ctx_.openEqFromKeyboard)
            ctx_.openEqFromKeyboard(anchorSelected ? anchor : selected.front());
        return true;
    }

    // Volume Envelope: the double-click resets the note to 0 dB.
    if (currentTool_ == ToolId::VolumeEnvelope) {
        resetSelectedNotesVolumeByKeyboard();
        return true;
    }

    std::vector<Note> work = notes;
    std::vector<int> changed;

    if (currentTool_ == ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift) {
        const bool modulation = currentTool_ == ToolId::PitchModulation;
        for (int index : selected) {
            auto& note = work[static_cast<size_t>(index)];
            float& value = modulation ? note.retuneSpeed : note.pitchDriftScale;
            value = value > 0.5f ? 0.0f : 1.0f;   // toggle 100% / 0%, as the double-click does
            note.dirty = true;
            changed.push_back(index);
        }
        const auto event = modulation ? KeyboardNoteEvent::ModulationChanged : KeyboardNoteEvent::DriftChanged;
        if (commitKeyboardNoteEdit(work, changed,
                                   modulation ? LOC_RAW("Modulation depth") : LOC_RAW("Drift correction")))
            notifyKeyboardNote(event, work, changed.front());
        return true;
    }

    // Other tools: snap the note to the pitch grid (Pitch Snap). As with the double-click,
    // snapping starts from the note's base pitch, ignoring the current offset.
    const auto scaleSnap = ctx_.getActiveScaleSnap ? ctx_.getActiveScaleSnap() : std::nullopt;
    for (int index : selected) {
        auto& note = work[static_cast<size_t>(index)];
        const float baseMidi = PitchUtils::freqToMidi(note.pitch);
        float snappedMidi = baseMidi;
        switch (pitchGridMode_) {
            case PitchGridMode::NoSnap:
                continue;   // nothing to snap to without a grid
            case PitchGridMode::Chromatic:
                snappedMidi = std::round(baseMidi);
                break;
            case PitchGridMode::KeyScale:
                snappedMidi = scaleSnap.value_or(ScaleSnapConfig{}).quantizeMidiToActiveScale(baseMidi);
                break;
        }
        const float snappedOffset = snappedMidi - baseMidi;
        if (std::abs(snappedOffset - note.pitchOffset) < 0.001f)
            continue;
        note.pitchOffset = snappedOffset;
        note.dirty = true;
        changed.push_back(index);
    }

    if (changed.empty()) {
        // Already on the grid (or snapping is off): announce the current pitch.
        notifyKeyboardNote(KeyboardNoteEvent::PitchMoved, notes, selected.front());
        return true;
    }
    if (commitKeyboardNoteEdit(work, changed, LOC_RAW("Pitch snap")))
        notifyKeyboardNote(KeyboardNoteEvent::PitchMoved, work, changed.front());
    return true;
}

void PianoRollToolHandler::switchToolFromKeyboard(ToolId tool)
{
    ctx_.setCurrentTool(tool);
    if (ctx_.notifyToolChangedFromKeyboard)
        ctx_.notifyToolChangedFromKeyboard(tool);
}

void PianoRollToolHandler::moveSelectedNotesPitchByKeyboard(int direction, bool fineCents, bool resetToSung,
                                                             bool semitoneIgnoringScale)
{
    const auto& notes = committedNotes(ctx_);
    const auto selectedIndices = collectSelectedNoteIndices(notes);
    if (selectedIndices.empty()) {
        notifyKeyboardNote(notes.empty() ? KeyboardNoteEvent::NoNotes : KeyboardNoteEvent::NoSelection,
                           notes, -1);
        return;
    }

    const bool openDyne = AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme());
    const bool standardPitch = !openDyne
        && ctx_.shouldSnapWholeNoteMoveToStandardPitch
        && ctx_.shouldSnapWholeNoteMoveToStandardPitch();
    const bool noSnap = openDyne && pitchGridMode_ == PitchGridMode::NoSnap;
    const bool scaleSnapEnabled = openDyne
        ? pitchGridMode_ == PitchGridMode::KeyScale
        : !standardPitch;
    const auto scaleSnap = ctx_.getActiveScaleSnap
        ? ctx_.getActiveScaleSnap().value_or(ScaleSnapConfig{})
        : ScaleSnapConfig{};

    std::vector<Note> workingNotes = notes;
    double rangeStart = std::numeric_limits<double>::max();
    double rangeEnd = std::numeric_limits<double>::lowest();
    std::vector<std::pair<int, float>> changedOffsets;
    for (int noteIndex : selectedIndices) {
        auto& note = workingNotes[static_cast<size_t>(noteIndex)];
        const float baseMidi = PitchUtils::freqToMidi(note.pitch);
        const float initialOffset = note.pitchOffset;
        const float currentMidi = baseMidi + initialOffset;
        float targetMidi = currentMidi;
        if (resetToSung) {
            // originalPitch is the sung pitch; notes without it have nothing to return to.
            if (note.originalPitch > 0.0f)
                targetMidi = PitchUtils::freqToMidi(note.originalPitch);
        } else if (fineCents) {
            targetMidi += static_cast<float>(direction) * 0.01f;
        } else if (semitoneIgnoringScale) {
            // To the next whole semitone, whatever the grid mode.
            targetMidi = direction > 0
                ? std::floor(currentMidi) + 1.0f
                : std::ceil(currentMidi) - 1.0f;
        } else if (noSnap) {
            targetMidi += static_cast<float>(direction) * 0.01f;
        } else if (scaleSnapEnabled) {
            const float firstCandidate = direction > 0
                ? std::floor(currentMidi) + 1.0f
                : std::ceil(currentMidi) - 1.0f;
            for (int distance = 0; distance < 12; ++distance) {
                const float candidate = firstCandidate + static_cast<float>(direction * distance);
                if (std::abs(scaleSnap.quantizeMidiToActiveScale(candidate) - candidate) <= 0.001f) {
                    targetMidi = candidate;
                    break;
                }
            }
        } else {
            targetMidi = direction > 0
                ? std::floor(currentMidi) + 1.0f
                : std::ceil(currentMidi) - 1.0f;
        }
        const float finalOffset = targetMidi - baseMidi;
        if (std::abs(finalOffset - initialOffset) <= 0.001f)
            continue;

        note.pitchOffset = finalOffset;
        note.dirty = true;
        changedOffsets.emplace_back(noteIndex, initialOffset);
        rangeStart = std::min(rangeStart, note.startTime);
        rangeEnd = std::max(rangeEnd, note.endTime);
    }
    if (changedOffsets.empty()) {
        // Resetting a note that is already at its sung pitch: still say the pitch, so R is not silent.
        if (resetToSung)
            notifyKeyboardNote(KeyboardNoteEvent::PitchMoved, notes, selectedIndices.front());
        return;
    }

    const F0FrameRange editRange = ctx_.getF0Timeline().rangeForTimes(rangeStart, rangeEnd);
    const auto contentSnapshot = ctx_.getEditableContentSnapshot();
    if (editRange.isEmpty() || contentSnapshot == nullptr || contentSnapshot->pitchCurve == nullptr) {
        notifyKeyboardNote(KeyboardNoteEvent::EditFailed, notes, -1);
        return;
    }

    ctx_.beginNoteDraft();
    auto& draft = ctx_.getNoteDraft();
    draft.contentDirty = true;
    draft.workingNotes = workingNotes;
    ctx_.setUndoDescription(LOC_RAW("Move note"));
    const bool committed = commitNoteBasedCorrection(ctx_, workingNotes, editRange);
    ctx_.clearNoteDraft();
    if (!committed) {
        ctx_.setUndoDescription({});
        notifyKeyboardNote(KeyboardNoteEvent::EditFailed, committedNotes(ctx_), -1);
        return;
    }

    for (const auto& [noteIndex, oldOffset] : changedOffsets) {
        ctx_.notifyNoteOffsetChanged(static_cast<size_t>(noteIndex), oldOffset,
                                     workingNotes[static_cast<size_t>(noteIndex)].pitchOffset);
    }
    // workingNotes rather than committedNotes: it is guaranteed to hold the new pitch.
    notifyKeyboardNote(KeyboardNoteEvent::PitchMoved, workingNotes, changedOffsets.front().first);
}

bool PianoRollToolHandler::isEmptySpaceMouseDown(const juce::MouseEvent& e)
{
    if (e.x < ctx_.getPianoKeyWidth()) {
        return false;
    }

    if (currentTool_ == ToolId::LineAnchor) {
        return false;
    }

    if (currentTool_ == ToolId::Eraser) {
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

    return hitTestNoteAt(displayNotes(ctx_), e).noteIndex >= 0;
}

bool PianoRollToolHandler::hitTestF0Curve(const juce::MouseEvent& e, int& frameIndex) const
{
    frameIndex = -1;
    if (e.x < ctx_.getPianoKeyWidth()) {
        return false;
    }

    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) {
        return false;
    }

    const auto contentSnapshot = ctx_.getEditableContentSnapshot();
    if (contentSnapshot == nullptr || contentSnapshot->pitchCurve == nullptr) {
        return false;
    }

    const auto& curveSnapshot = *contentSnapshot->pitchCurve;
    if (curveSnapshot.isEmpty()) {
        return false;
    }

    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return false;
    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime)) {
        return false;
    }

    const int frameCount = static_cast<int>(curveSnapshot.size());
    const int centerFrame = juce::jlimit(0, frameCount - 1, f0tl.frameAtOrBefore(*sourceTime));
    const int startFrame = std::max(0, centerFrame - 2);
    const int endFrameExclusive = std::min(frameCount, centerFrame + 3);
    const auto& originalF0 = curveSnapshot.getOriginalF0();

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

void PianoRollToolHandler::updateF0SelectionDrag(const juce::MouseEvent& e)
{
    auto& selection = ctx_.getState().selection;
    if (!selection.isSelectingF0 || selection.f0SelectionAnchorFrame < 0) {
        return;
    }

    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) {
        selection.isSelectingF0 = false;
        selection.f0SelectionAnchorFrame = -1;
        selection.f0SelectionCurrentFrame = -1;
        return;
    }

    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return;

    const int frame = juce::jlimit(0, f0tl.endFrameExclusive() - 1, f0tl.frameAtOrBefore(*sourceTime));
    selection.f0SelectionCurrentFrame = frame;

    // 拖拽中：收集锚点到当前位置之间所有有效 F0 帧
    const double anchorTime = f0tl.timeAtFrame(selection.f0SelectionAnchorFrame);
    const double currentTime = f0tl.timeAtFrame(frame);
    const double t0 = std::min(anchorTime, currentTime);
    const double t1 = std::max(anchorTime, currentTime);
    ctx_.getState().frameSelection.clear();
    collectF0StretchesInRange(t0, t1, ctx_.getState().frameSelection);

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

void PianoRollToolHandler::cancelActiveMouseGesture()
{
    if (timelineSeekActive_) {
        timelineSeekActive_ = false;
        if (timelineSeekTime_ >= 0.0 && ctx_.notifyPlayheadChange)
            ctx_.notifyPlayheadChange(timelineSeekTime_, true);
    }

    auto& state = ctx_.getState();
    state.resetTransient();
    ctx_.clearNoteDraft();
    pendingEqPrimaryIndex_ = -1;
    finishMouseParameterTargetChange();
}

void PianoRollToolHandler::handleDeleteKey()
// 删除键处理：只删除选中的音符并清除对应音高修正；无选中则不删除
{
    const auto& notes = committedNotes(ctx_);
    const auto selectedIndices = collectSelectedNoteIndices(notes);
    if (selectedIndices.empty()) {
        // Otherwise Delete did nothing and said nothing.
        notifyKeyboardNote(notes.empty() ? KeyboardNoteEvent::NoNotes : KeyboardNoteEvent::NoSelection,
                           notes, -1);
        return;
    }

    const auto before = beginParameterTargetChange();
    auto result = ctx_.deleteSelectedPianoRollNotes(
        selectedIndices, ctx_.getNotesRevision(), LOC_RAW("Delete note"));
    if (result.status != SelectedNoteDeleteStatus::Applied) {
        // Delete also sits on Backspace and "1": a press that did nothing must be heard too.
        notifyKeyboardNote(KeyboardNoteEvent::EditFailed, committedNotes(ctx_), -1);
        if (result.status != SelectedNoteDeleteStatus::StaleRevision)
            finishParameterTargetChange(before);
        return;
    }

    if (result.pitchCurveEdited) {
        ctx_.notifyPitchCurveEdited(result.affectedRange.startFrame,
                                    result.affectedRange.endFrameExclusive - 1);
    }
    if (ctx_.invalidateLiveNotes)
        ctx_.invalidateLiveNotes(result.beforeNotes, result.contentSnapshot->notes);

    // Accessibility: keep the place in the note list. The note after the removed ones
    // (or the last note) becomes selected, so the arrows go on from here.
    const bool accessibilityMode = ctx_.accessibilityModeEnabled
        && ctx_.accessibilityModeEnabled();
    const auto& afterNotes = committedNotes(ctx_);
    if (accessibilityMode && !afterNotes.empty()) {
        const int firstRemoved = *std::min_element(selectedIndices.begin(), selectedIndices.end());
        const int neighbour = juce::jmin(firstRemoved, static_cast<int>(afterNotes.size()) - 1);
        ctx_.getState().noteSelection.setSingle(neighbour, static_cast<int>(afterNotes.size()));
        if (ctx_.invalidateSelectionFeedback)
            ctx_.invalidateSelectionFeedback();
    }
    finishParameterTargetChange(before, true);
    // Delete also sits on Backspace and "1", so an accidental press must be heard.
    notifyKeyboardNote(KeyboardNoteEvent::Removed, afterNotes, static_cast<int>(selectedIndices.size()));
}

void PianoRollToolHandler::handleSelectTool(const juce::MouseEvent& e)
// 选择工具鼠标按下处理：检测音符边缘调整、音符选中/取消选中、框选区域开始
{
    const auto& notes = committedNotes(ctx_);

    ctx_.getState().noteResize.clear();

    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return;

    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime))
        return;

    const auto hit = hitTestNoteAt(notes, e);
    const int clickedNoteIndex = hit.noteIndex >= 0 && hit.edge == NoteResizeEdge::None
        ? hit.noteIndex
        : -1;

    if (hit.noteIndex >= 0)
        beginMouseParameterTargetChange();

    bool isCtrlDown = e.mods.isCtrlDown() || e.mods.isCommandDown();

    bool isOpenDyne = AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme());
    bool isShiftDown = e.mods.isShiftDown();

    if (hit.noteIndex >= 0 && hit.edge != NoteResizeEdge::None) {
        ctx_.getState().noteResize.isResizing = true;
        ctx_.getState().noteResize.isDirty = false;
        ctx_.getState().noteResize.noteIndex = hit.noteIndex;
        ctx_.getState().noteResize.edge = hit.edge;
        ctx_.getState().noteResize.expectedNotesRevision = ctx_.getNotesRevision();

        auto& noteSelection = ctx_.getState().noteSelection;
        const int noteCount = static_cast<int>(notes.size());
        if (!noteSelection.isSelected(hit.noteIndex) && !isCtrlDown && !isShiftDown) {
            noteSelection.setSingle(hit.noteIndex, noteCount);
            ctx_.getState().frameSelection.clear();   // 纯点击替换全部选择
        } else {
            noteSelection.add(hit.noteIndex, noteCount);
        }

        if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
        finishMouseParameterTargetChange();
        return;
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
            ctx_.getState().frameSelection.clear();   // 纯点击替换全部选择
        }

        finishMouseParameterTargetChange();
        if (noteSelection.isSelected(clickedNoteIndex)) {
            // OpenDyne Main/Select 与普通模式一致：允许对选中音符进行 pitch 拖拽。
            beginNotePitchDrag(notes, clickedNoteIndex);
        } else {
            // Note was not selected - start selection area or clear
            if (!isCtrlDown) {
                ctx_.getState().noteDrag.draggedNoteIndices.clear();
                ctx_.getState().noteDrag.previewSnapshot.reset();
            }
        }

        if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
    } else {
        int f0Frame = -1;
        if (hitTestF0Curve(e, f0Frame)) {
            int segStart = 0, segEnd = 0;
            if (findF0ContiguousStretch(f0Frame, segStart, segEnd)) {
                auto& sel = ctx_.getState().selection;
                auto& fs = ctx_.getState().frameSelection;
                beginMouseParameterTargetChange();
                if (isCtrlDown) {
                    // Ctrl：复选追加该段，不进入拖拽
                    fs.addRange(segStart, segEnd);
                } else {
                    ctx_.getState().noteSelection.clear();
                    ctx_.getState().noteDrag.clear();
                    ctx_.getState().noteResize.clear();
                    fs.clear();
                    fs.addRange(segStart, segEnd);
                    // 接通拖拽状态机：后续拖拽经 updateF0SelectionDrag 扩展选择
                    sel.isSelectingArea = false;
                    sel.isSelectingF0 = true;
                    sel.f0SelectionAnchorFrame = f0Frame;
                    sel.f0SelectionCurrentFrame = f0Frame;
                }
                if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
            }
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

    beginMouseParameterTargetChange();
    deselectAllNotes();
    ctx_.getState().frameSelection.clear();
    ctx_.getState().noteDrag.draggedNoteIndices.clear();
    ctx_.getState().noteDrag.previewSnapshot.reset();
    ctx_.getState().noteResize.clear();

    if (e.x >= ctx_.getPianoKeyWidth()) {
        ctx_.getState().selection.isSelectingArea = true;
        ctx_.getState().selection.selectionStartTime = std::max(0.0, *sourceTime);
        ctx_.getState().selection.selectionEndTime = ctx_.getState().selection.selectionStartTime;
        float midiVal = juce::jlimit(ctx_.getMinMidi(), ctx_.getMaxMidi(),
            ctx_.getViewMapper().freqToMidi(ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY))));
        ctx_.getState().selection.selectionStartMidi = midiVal;
        ctx_.getState().selection.selectionEndMidi = midiVal;
    } else {
        ctx_.getState().selection.isSelectingArea = false;
    }
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
}

void PianoRollToolHandler::handleDrawCurveTool(const juce::MouseEvent& e)
// 手绘曲线工具处理：将鼠标位置转换为F0值，在帧间进行对数插值，记录脏区
{
    const auto contentSnapshot = ctx_.getEditableContentSnapshot();
    if (contentSnapshot == nullptr || contentSnapshot->pitchCurve == nullptr) {
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

namespace {

bool eraserTouchesRectangle(const juce::Point<float>& point,
                            const juce::Rectangle<float>& rectangle,
                            float radius) noexcept
{
    const float closestX = juce::jlimit(rectangle.getX(), rectangle.getRight(), point.x);
    const float closestY = juce::jlimit(rectangle.getY(), rectangle.getBottom(), point.y);
    const float dx = point.x - closestX;
    const float dy = point.y - closestY;
    return dx * dx + dy * dy <= radius * radius;
}

}

void PianoRollToolHandler::updateEraserCursor(const juce::MouseEvent& e)
{
    auto& eraser = ctx_.getState().eraser;
    eraser.cursorPosition = { static_cast<float>(e.x),
                               static_cast<float>(e.y - ctx_.contentOriginY) };
    eraser.hasCursor = e.x >= ctx_.getPianoKeyWidth() && e.y >= ctx_.contentOriginY;
    if (ctx_.invalidateSelectionFeedback)
        ctx_.invalidateSelectionFeedback();
}

void PianoRollToolHandler::handleEraserTool(const juce::MouseEvent& e)
{
    auto& eraser = ctx_.getState().eraser;
    if (!eraser.isErasing) {
        eraser.clear();
        eraser.isErasing = true;
        eraser.expectedNotesRevision = ctx_.getNotesRevision();
    }

    updateEraserCursor(e);
    if (!eraser.hasCursor)
        return;

    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty())
        return;

    const auto leftSourceTime = pixelXToSourceTime(
        e.x - static_cast<int>(std::ceil(kEraserBrushRadiusPx)));
    const auto rightSourceTime = pixelXToSourceTime(
        e.x + static_cast<int>(std::ceil(kEraserBrushRadiusPx)));
    if (!leftSourceTime || !rightSourceTime)
        return;

    const auto editRange = sourceEditRange();
    const double startTime = juce::jlimit(editRange.startSeconds,
                                          editRange.endSeconds,
                                          std::min(*leftSourceTime, *rightSourceTime));
    const double endTime = juce::jlimit(editRange.startSeconds,
                                        editRange.endSeconds,
                                        std::max(*leftSourceTime, *rightSourceTime));
    const auto brushRange = f0tl.nonEmptyRangeForTimes(startTime, endTime);
    if (!brushRange.isEmpty()) {
        if (eraser.startFrame < 0) {
            eraser.startFrame = brushRange.startFrame;
            eraser.endFrameExclusive = brushRange.endFrameExclusive;
        } else {
            eraser.startFrame = std::min(eraser.startFrame, brushRange.startFrame);
            eraser.endFrameExclusive = std::max(eraser.endFrameExclusive,
                                                 brushRange.endFrameExclusive);
        }
    }

    const auto& notes = ctx_.getCommittedNotes();
    const auto mapper = ctx_.getViewMapper();
    const auto point = eraser.cursorPosition;
    for (int noteIndex = 0; noteIndex < static_cast<int>(notes.size()); ++noteIndex) {
        if (std::find(eraser.noteIndices.begin(), eraser.noteIndices.end(), noteIndex)
            != eraser.noteIndices.end()) {
            continue;
        }

        const auto& note = notes[static_cast<size_t>(noteIndex)];
        const float adjustedPitch = note.getAdjustedPitch();
        if (adjustedPitch <= 0.0f)
            continue;

        const int x1 = sourceTimeToScreenX(note.startTime);
        const int x2 = sourceTimeToScreenX(note.endTime);
        const float noteHeight = std::max(1.0f, mapper.pixelsPerSemitone);
        const float noteY = mapper.freqToY(adjustedPitch) - noteHeight * 0.5f;
        const juce::Rectangle<float> noteBounds(
            static_cast<float>(std::min(x1, x2)), noteY,
            static_cast<float>(std::max(1, std::abs(x2 - x1))), noteHeight);
        if (!eraserTouchesRectangle(point, noteBounds, kEraserBrushRadiusPx))
            continue;

        eraser.noteIndices.push_back(noteIndex);
    }
}

void PianoRollToolHandler::handleEraserUp()
{
    auto& eraser = ctx_.getState().eraser;
    eraser.isErasing = false;

    const auto clearFeedbackState = [&]() {
        eraser.startFrame = -1;
        eraser.endFrameExclusive = -1;
        eraser.noteIndices.clear();
        if (ctx_.invalidateSelectionFeedback)
            ctx_.invalidateSelectionFeedback();
    };

    if (eraser.startFrame < 0 || eraser.endFrameExclusive <= eraser.startFrame) {
        clearFeedbackState();
        return;
    }

    if (!ctx_.editEraserStroke) {
        clearFeedbackState();
        return;
    }

    const auto before = beginParameterTargetChange();
    const auto result = ctx_.editEraserStroke(
        F0FrameRange{ eraser.startFrame, eraser.endFrameExclusive },
        eraser.noteIndices,
        eraser.expectedNotesRevision,
        LOC_RAW("Eraser"));
    if (result.status == EraserStrokeEditStatus::Applied) {
        if (ctx_.clearLineAnchorSegmentSelection)
            ctx_.clearLineAnchorSegmentSelection();
        if (ctx_.invalidateLiveNotes)
            ctx_.invalidateLiveNotes(result.beforeNotes, ctx_.getCommittedNotes());
        ctx_.notifyPitchCurveEdited(result.affectedRange.startFrame,
                                    result.affectedRange.endFrameExclusive - 1);
    }
    if (result.status != EraserStrokeEditStatus::StaleRevision)
        finishParameterTargetChange(before, result.status == EraserStrokeEditStatus::Applied);

    clearFeedbackState();
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
        beginMouseParameterTargetChange();
        auto& noteSelection = ctx_.getState().noteSelection;
        const int noteCount = static_cast<int>(committedNotes.size());
        if (isCtrlDown) {
            noteSelection.toggle(existingNoteIndex, noteCount);
        } else {
            if (!noteSelection.isSelected(existingNoteIndex)) {
                noteSelection.setSingle(existingNoteIndex, noteCount);
                ctx_.getState().frameSelection.clear();
            }
        }
        if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
        finishMouseParameterTargetChange();
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

        float currentMidi = juce::jlimit(ctx_.getMinMidi(), ctx_.getMaxMidi(),
            ctx_.getViewMapper().freqToMidi(
                ctx_.getViewMapper().yToFreq(static_cast<float>(e.y - ctx_.contentOriginY))));
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
        // 显式 F0 选择 = 框选时间范围内所有有效帧；音符派生范围由
        // collectSelectedFrameRanges 在消费时按需合并，不在此存储。
        ctx_.getState().frameSelection.clear();
        collectF0StretchesInRange(selStartTime, selEndTime, ctx_.getState().frameSelection);
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

        auto& draft = ctx_.getNoteDraft();
        if (ctx_.getState().noteResize.noteIndex >= static_cast<int>(draft.baselineNotes.size())) {
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

        const int resizeIdx = static_cast<int>(ctx_.getState().noteResize.noteIndex);
        const auto plan = planNoteResizeEdit(draft.baselineNotes,
                                             resizeIdx,
                                             ctx_.getState().noteResize.edge,
                                             clampedTime,
                                             kNoteResizeMinDuration);
        draft.workingNotes = plan.afterNotes;
        ctx_.getState().noteResize.clampedTime = clampedTime;
        auto& notes = draft.workingNotes;
        if (ctx_.invalidateLiveNotes) ctx_.invalidateLiveNotes(beforeNotes, notes);
        return;
    }

    if (!ctx_.getState().noteDrag.draggedNoteIndices.empty()) {
        dragNotePitch(e);
        return;
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

    // 只要 mouseDown 落在音符上（draggedNoteIndices 非空）就进入 endNotePitchDrag：
    // 无实际拖拽的点击也在其内部走统一终止路径（noteDrag.clear + draft 清理）。
    if (!ctx_.getState().noteDrag.draggedNoteIndices.empty()) {
        suppressFinalNoteDraftCommit = endNotePitchDrag(e);
    }

    auto notes = std::vector<Note>(displayNotes(ctx_));
    const auto f0tl = ctx_.getF0Timeline();

    bool resizeMoved = false;
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
    }

    notes.erase(
        std::remove_if(notes.begin(), notes.end(), [](const Note& n) {
            return n.startTime >= n.endTime;
        }),
        notes.end());

    if (ctx_.getState().noteResize.isResizing && resizeMoved) {
        const auto& baseline = draftBaselineNotes(ctx_);
        const auto& resize = ctx_.getState().noteResize;
        const auto resizePlan = planNoteResizeEdit(baseline,
                                                   resizeIdx,
                                                   resize.edge,
                                                   resize.clampedTime,
                                                   kNoteResizeMinDuration);
        const auto contentSnapshot = ctx_.getEditableContentSnapshot();
        F0FrameRange editRange;
        if (contentSnapshot != nullptr && contentSnapshot->pitchCurve != nullptr) {
            editRange = f0tl.rangeForTimes(resizePlan.dirtyStartTime, resizePlan.dirtyEndTime);
        }

        ctx_.getNoteDraft().workingNotes = notes;
        ctx_.setUndoDescription(LOC_RAW("Resize note"));
        suppressFinalNoteDraftCommit = true;

        const auto expectedNotesRevision = resize.expectedNotesRevision;
        const auto edge = resize.edge;
        const auto clampedTime = resize.clampedTime;
        if (contentSnapshot != nullptr && contentSnapshot->pitchCurve != nullptr && !editRange.isEmpty()) {
            const auto commitResize = [this, expectedNotesRevision, resizeIdx, edge, clampedTime]
                (const std::vector<PitchCorrectionSegment>& segments, F0FrameRange affectedRange) {
                return ctx_.commitNoteResize(expectedNotesRevision,
                                             resizeIdx,
                                             edge,
                                             clampedTime,
                                             kNoteResizeMinDuration,
                                             segments,
                                             affectedRange,
                                             LOC_RAW("Resize note"));
            };
            commitNoteBasedCorrection(ctx_, notes, editRange, commitResize);
        } else {
            ctx_.commitNoteResize(expectedNotesRevision,
                                  resizeIdx,
                                  edge,
                                  clampedTime,
                                  kNoteResizeMinDuration,
                                  {},
                                  {},
                                  LOC_RAW("Resize note"));
        }
    }

    if (ctx_.getNoteDraft().active && !suppressFinalNoteDraftCommit) {
        ctx_.getNoteDraft().workingNotes = notes;
        ctx_.setUndoDescription(LOC_RAW("Edit note"));
        ctx_.commitNoteDraft();
    }

    ctx_.getState().noteResize.clear();

    if (ctx_.getState().selection.isSelectingArea) {
        ctx_.getState().selection.isSelectingArea = false;
        // frameSelection 已在拖拽中构建（框内显式 F0），音符派生范围由
        // collectSelectedFrameRanges 消费时按需合并，此处不再重建。
        // EQ 工具：框选完成且 selection 非空时，以 anchor 音符打开 EQ 弹窗一次。
        // 纯点击不进入此处（由 handleEqToolMouseUp 消费 pending 主音符）。
        if (currentTool_ == ToolId::Eq && !ctx_.getState().noteSelection.empty()) {
            ctx_.openEqPreview(ctx_.getState().noteSelection.anchorIndex);
        }
    }

    ctx_.getState().selection.isSelectingF0 = false;
    ctx_.getState().selection.f0SelectionAnchorFrame = -1;
    ctx_.getState().selection.f0SelectionCurrentFrame = -1;
    // note drag 终止状态由 endNotePitchDrag 唯一清理（noteDrag.clear），不再局部写
    ctx_.clearNoteDraft();
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
}

// ============================================================================
// OpenDyne（NotesPrimary）工具
//
// 唯一 pitch-drag 内部流程：OpenTune Select 与 OpenDyne Pitch Tool 共用。
// OpenTune 的整体移动吸附由偏好决定；OpenDyne 由 Pitch Grid 三态决定。
// ============================================================================

void PianoRollToolHandler::beginNotePitchDrag(const std::vector<Note>& notes, int anchorNoteIndex)
{
    auto& state = ctx_.getState();
    state.noteDrag.draggedNoteIndices = collectSelectedNoteIndices(notes);
    state.noteDrag.anchorNoteIndex = anchorNoteIndex;
    state.noteDrag.lastAuditionTargetHz = (anchorNoteIndex >= 0 && anchorNoteIndex < (int)notes.size())
        ? notes[(size_t)anchorNoteIndex].getAdjustedPitch()
        : 0.0f;
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

    state.noteDrag.previewSnapshot = buildNoteBasedCorrectionState(ctx_, notes, editRange);
}

void PianoRollToolHandler::dragNotePitch(const juce::MouseEvent& e, bool allowAudition)
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
                // 允许超出±100%（Melodyne支持224%、-112%等），DSP端不做钳位
                // 方向翻转：向上拖拽减小pitchDriftScale，向下拖拽增大
                const float target = baseline.pitchDriftScale - paramDelta;
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

    if (state.noteDrag.draggedNoteIndices.empty()) {
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

    const bool openDyne = AudioEditingScheme::usesNotesPrimaryScheme(ctx_.getAudioEditingScheme());
    // Alt 拖拽期间临时解除吸附（保留连续 cents），OpenTune/OpenDyne 统一。
    const bool altBypass = e.mods.isAltDown();
    const bool snapWholeMoveToStandardPitch = !openDyne
        && !altBypass
        && ctx_.shouldSnapWholeNoteMoveToStandardPitch
        && ctx_.shouldSnapWholeNoteMoveToStandardPitch();
    auto& notes = workingDraftNotes(ctx_);
    resetDraftNotesToBaseline(ctx_);
    float wholeMoveDeltaSemitones = deltaSemitones;
    if (snapWholeMoveToStandardPitch) {
        const auto& baselineNotes = draftBaselineNotes(ctx_);
        const auto& anchor = baselineNotes[static_cast<size_t>(state.noteDrag.anchorNoteIndex)];
        const float anchorMidi = PitchUtils::freqToMidi(anchor.pitch) + anchor.pitchOffset;
        wholeMoveDeltaSemitones = std::round(anchorMidi + deltaSemitones) - anchorMidi;
    }

    // 循环不变量：拖拽期间活动调式不变，量化配置只取一次
    const auto scaleSnap = ctx_.getActiveScaleSnap ? ctx_.getActiveScaleSnap() : std::nullopt;

    for (int noteIndex : state.noteDrag.draggedNoteIndices) {
        auto& note = notes[static_cast<size_t>(noteIndex)];
        const float initialOffset = draftBaselineNotes(ctx_)[static_cast<size_t>(noteIndex)].pitchOffset;
        // 连续基准 MIDI（不提前取整）：SNAP 目标全程保持连续语义
        const float baseMidi = PitchUtils::freqToMidi(note.pitch);
        const float targetMidi = baseMidi + initialOffset + wholeMoveDeltaSemitones;
        // OpenTune 恒量化到活动调式；OpenDyne 按 Pitch Grid 三态，KeyScale 与 OpenTune 同源量化。
        float snappedMidi = targetMidi;
        if (!altBypass && !snapWholeMoveToStandardPitch) {
            if (!openDyne || pitchGridMode_ == PitchGridMode::KeyScale) {
                snappedMidi = scaleSnap.value_or(ScaleSnapConfig{}).quantizeMidiToActiveScale(targetMidi);
            } else if (pitchGridMode_ == PitchGridMode::Chromatic) {
                snappedMidi = std::round(targetMidi);  // 吸附到最近半音
            }
            // NoSnap：自由模式，保留连续 cents
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

    if (allowAudition && ctx_.auditionNotePitch) {
        const int anchorIndex = state.noteDrag.anchorNoteIndex;
        if (anchorIndex >= 0 && anchorIndex < (int)notes.size()) {
            const auto& anchor = notes[(size_t)anchorIndex];
            const float targetHz = anchor.getAdjustedPitch();
            if (targetHz > 0.0f) {
                if (targetHz != state.noteDrag.lastAuditionTargetHz) {
                    state.noteDrag.lastAuditionTargetHz = targetHz;
                    ctx_.auditionNotePitch(anchor.startTime, anchor.endTime, targetHz);
                }
            } else {
                state.noteDrag.lastAuditionTargetHz = 0.0f;
            }
        }
    }
}

bool PianoRollToolHandler::endNotePitchDrag(const juce::MouseEvent& e)
{
    auto& state = ctx_.getState();

    if (ctx_.stopNotePitchAudition)
        ctx_.stopNotePitchAudition();
    state.noteDrag.lastAuditionTargetHz = 0.0f;

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
        dragNotePitch(e, false);

        auto baselineNotes = std::vector<Note>(draftBaselineNotes(ctx_));
        auto notes = std::vector<Note>(displayNotes(ctx_));
        const auto contentSnapshot = ctx_.getEditableContentSnapshot();

        // 唯一 editRange：由实际变化的音符派生（noteDragEditRange），与预览一致
        const F0FrameRange editRange = noteDragEditRange(notes);

        // 最终采样后、任何提交前复位预览：mouseUp 时刻 build 的 previewSnapshot
        // 只供拖拽期间渲染；无提交时也必须清掉，由下方 invalidation 把
        // retained raster 抹回 committed 状态。
        state.noteDrag.previewSnapshot.reset();

        bool committed = false;
        if (!editRange.isEmpty() && contentSnapshot != nullptr && contentSnapshot->pitchCurve != nullptr) {
            ctx_.setUndoDescription(currentTool_ == ToolId::PitchModulation
                ? LOC_RAW("Modulation depth")
                : LOC_RAW("Drift correction"));
            // applyCorrectionToRange + commitNotesAndSegments 唯一提交链
            committed = commitNoteBasedCorrection(ctx_, notes, editRange);
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

    if (state.noteDrag.draggedNoteIndices.empty()) {
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
    dragNotePitch(e, false);

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
        const auto contentSnapshot = ctx_.getEditableContentSnapshot();
        ctx_.getNoteDraft().workingNotes = notes;
        ctx_.setUndoDescription(LOC_RAW("Move note"));

        // 音高编辑失败（无 PitchCurve）不允许 note-only 提交。
        if (contentSnapshot != nullptr && contentSnapshot->pitchCurve != nullptr) {
            committed = commitNoteBasedCorrection(ctx_, notes, editRange);
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

    beginMouseParameterTargetChange();
    auto& noteSelection = ctx_.getState().noteSelection;
    const int noteCount = static_cast<int>(notes.size());
    if (noteSelection.isSelected(clickedNoteIndex) && !noteSelection.isAllSelected(noteCount)) {
        // 点击已选中音符：保留多选，准备批量拖拽
    } else {
        // 点击未选中音符或全选状态：单选该音符
        noteSelection.setSingle(clickedNoteIndex, noteCount);
        ctx_.getState().frameSelection.clear();
    }
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
    finishMouseParameterTargetChange();

    if (!noteSelection.isSelected(clickedNoteIndex))
        return;

    // PitchModulation / PitchDrift：记录拖拽起点，初始化选中音符索引
    if (currentTool_ == ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift) {
        dragStartPos_ = e.position.toInt();
        beginNotePitchDrag(notes, clickedNoteIndex);
        return;
    }

    // Pitch Tool (F2×1)：启动 pitch drag
    beginNotePitchDrag(notes, clickedNoteIndex);
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
            ? note.retuneSpeed
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
            ? LOC_RAW("Modulation depth")
            : LOC_RAW("Drift correction"));

        const auto contentSnapshot = ctx_.getEditableContentSnapshot();
        const auto f0tl = ctx_.getF0Timeline();
        const F0FrameRange noteRange = f0tl.rangeForTimes(note.startTime, note.endTime);
        if (contentSnapshot != nullptr && contentSnapshot->pitchCurve != nullptr && !noteRange.isEmpty()) {
            commitNoteBasedCorrection(ctx_, work, noteRange);
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
    ctx_.setUndoDescription(LOC_RAW("Pitch snap"));

    const auto contentSnapshot = ctx_.getEditableContentSnapshot();
    const auto f0tl = ctx_.getF0Timeline();
    const F0FrameRange noteRange = f0tl.rangeForTimes(original.startTime, original.endTime);
    if (contentSnapshot != nullptr && contentSnapshot->pitchCurve != nullptr && !noteRange.isEmpty()) {
        commitNoteBasedCorrection(ctx_, work, noteRange);
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

    beginMouseParameterTargetChange();
    auto& state = ctx_.getState();
    if (!state.noteSelection.isSelected(clickedNoteIndex) || state.noteSelection.isAllSelected(static_cast<int>(notes.size()))) {
        state.noteSelection.setSingle(clickedNoteIndex, static_cast<int>(notes.size()));
        state.frameSelection.clear();
    }

    state.isVolumeDragging = true;
    const auto snap = ctx_.getEditableContentSnapshot();
    if (!snap) { state.isVolumeDragging = false; return; }
    volumeDragBaselineEnvelope_ = snap->volumeEnvelope;
    volumeDragSelectedIndices_ = collectSelectedNoteIndices(notes);
    volumeDragExpectedNotesRevision_ = ctx_.getNotesRevision();
    state.volumePreviewEnvelope = volumeDragBaselineEnvelope_;
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();
}

AutomationLane PianoRollToolHandler::buildVolumeEnvelopeDragPreview(const std::vector<Note>& notes,
                                                                     float deltaGainDb)
{
    // 预览与 mouseUp 事务共用同一纯函数：基准恒为拖拽起始 baseline，
    // 重叠选中音符按选择顺序 setRegionGain，保证预览 == 最终提交结果。
    return applyVolumeEnvelopeRegionDelta(
        volumeDragBaselineEnvelope_, notes, volumeDragSelectedIndices_, deltaGainDb);
}

void PianoRollToolHandler::handleVolumeEnvelopeToolDrag(const juce::MouseEvent& e)
{
    auto& state = ctx_.getState();
    if (!state.isVolumeDragging)
        return;

    const float dbPerPixel = 12.0f / ctx_.getViewMapper().pixelsPerSemitone;
    const float deltaGainDb = static_cast<float>(dragStartPos_.y - e.y) * dbPerPixel;
    state.volumePreviewDeltaDb = deltaGainDb;  // 新增：存储 dB 偏移
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

    const float dbPerPixel = 12.0f / ctx_.getViewMapper().pixelsPerSemitone;
    const float deltaGainDb = static_cast<float>(dragStartPos_.y - e.y) * dbPerPixel;

    const auto selectedIndices = std::move(volumeDragSelectedIndices_);
    state.volumePreviewEnvelope.clear();
    state.volumePreviewDeltaDb = 0.0f;
    if (std::abs(deltaGainDb) < 0.001f) {
        const auto& notes = committedNotes(ctx_);
        if (ctx_.invalidateLiveNotes)
            ctx_.invalidateLiveNotes(notes, notes);
        state.isVolumeDragging = false;
        return;
    }

    state.isVolumeDragging = false;
    if (ctx_.editVolumeEnvelopeRegions)
        ctx_.editVolumeEnvelopeRegions(selectedIndices, volumeDragExpectedNotesRevision_,
                                       deltaGainDb, LOC(kToolVolumeEnvelope));
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
    volumeDragSelectedIndices_.clear();
    state.volumePreviewEnvelope.clear();
    const auto before = beginParameterTargetChange();
    state.noteSelection.setSingle(noteIndex, static_cast<int>(notes.size()));
    state.frameSelection.clear();
    if (ctx_.invalidateSelectionFeedback)
        ctx_.invalidateSelectionFeedback();

    if (ctx_.resetVolumeEnvelopeRegion)
        ctx_.resetVolumeEnvelopeRegion(noteIndex, ctx_.getNotesRevision(),
                                       LOC(kToolVolumeEnvelope));
    finishParameterTargetChange(before);
}

// Keyboard version of the Volume Envelope drag: every selected note keeps its own level
// and gets the same step, exactly like buildVolumeEnvelopeDragPreview.
void PianoRollToolHandler::adjustSelectedNotesVolumeByKeyboard(int direction, bool fine)
{
    const auto& notes = committedNotes(ctx_);
    const auto selected = collectSelectedNoteIndices(notes);
    if (selected.empty()) {
        notifyKeyboardNote(notes.empty() ? KeyboardNoteEvent::NoNotes : KeyboardNoteEvent::NoSelection, notes, -1);
        return;
    }
    if (!ctx_.editVolumeEnvelopeRegions)
        return;

    // Same transaction as the mouse drag: every selected note keeps its own level and
    // gets the same step (1 dB, Alt 0.1 dB).
    const float step = static_cast<float>(direction) * (fine ? 0.1f : 1.0f);
    const auto result = ctx_.editVolumeEnvelopeRegions(selected, ctx_.getNotesRevision(), step,
                                                       LOC(kToolVolumeEnvelope));
    if (result.status == VolumeEnvelopeRegionEditStatus::StaleRevision
        || result.status == VolumeEnvelopeRegionEditStatus::Failed) {
        notifyKeyboardNote(KeyboardNoteEvent::EditFailed, committedNotes(ctx_), -1);
        return;
    }
    // NoChange: at the limit; stay silent, as at list ends.
    if (result.status != VolumeEnvelopeRegionEditStatus::Applied)
        return;
    notifyKeyboardNote(KeyboardNoteEvent::VolumeChanged, committedNotes(ctx_), selected.front());
}

// Enter with the Volume Envelope tool, like its double-click: back to 0 dB.
void PianoRollToolHandler::resetSelectedNotesVolumeByKeyboard()
{
    const auto& notes = committedNotes(ctx_);
    const auto selected = collectSelectedNoteIndices(notes);
    if (selected.empty() || !ctx_.resetVolumeEnvelopeRegion)
        return;

    // The double-click resets one note; the keyboard resets each selected note the same way.
    // A note already at 0 dB gives NoChange and nothing is committed, but the level is still said.
    for (int index : selected) {
        const auto result = ctx_.resetVolumeEnvelopeRegion(index, ctx_.getNotesRevision(), LOC(kToolVolumeEnvelope));
        // After a failure the other indices may point at different notes: stop here.
        if (result.status == VolumeEnvelopeRegionEditStatus::StaleRevision
            || result.status == VolumeEnvelopeRegionEditStatus::Failed) {
            notifyKeyboardNote(KeyboardNoteEvent::EditFailed, committedNotes(ctx_), -1);
            return;
        }
    }
    notifyKeyboardNote(KeyboardNoteEvent::VolumeChanged, committedNotes(ctx_), selected.front());
}

void PianoRollToolHandler::updateScissorsPreview(const juce::MouseEvent& e)
{
    auto& state = ctx_.getState();
    double newPreview = -1.0;
    const auto sourceTime = pixelXToSourceTime(e.x);
    if (sourceTime) {
        const auto editRange = sourceEditRange();
        if (editRange.contains(*sourceTime)) {
            // 分离线容差：鼠标在音符边界 ±kScissorsSeparatorTolerancePx 像素内
            // 不显示竖虚线，提示用户"此处可双击合并"。
            const auto tPlus = pixelXToSourceTime(e.x + kScissorsSeparatorTolerancePx);
            const double tolerance = tPlus ? std::abs(*tPlus - *sourceTime) : 1.0 / TimeCoordinate::kRenderSampleRate;
            for (const auto& note : committedNotes(ctx_)) {
                // 音符内部且远离两端边界 → 显示切割线
                if (note.startTime + tolerance < *sourceTime
                    && *sourceTime < note.endTime - tolerance) {
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

    // === 自定义双击合并检测（800ms / 30px） ===
    // 不依赖 JUCE mouseDoubleClick 回调，在每次 mouseUp 时独立检测，
    // 解决操作系统双击时间（~500ms）过短导致合并难以触发的问题。
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - scissorsLastClickTime_).count();
    const int dx = std::abs(e.x - scissorsLastClickPos_.x);
    const int dy = std::abs(e.y - scissorsLastClickPos_.y);
    const bool isSecondClick = (scissorsLastClickTime_.time_since_epoch().count() > 0)
                            && (elapsedMs < kScissorsDoubleClickMs)
                            && (dx <= kScissorsDoubleClickMaxDistPx)
                            && (dy <= kScissorsDoubleClickMaxDistPx);

    if (isSecondClick && handleScissorsToolMerge(e)) {
        // 双击合并成功，重置防连续合并
        scissorsLastClickTime_ = {};
        scissorsLastClickPos_ = {};
        return;
    }
    // === 结束合并检测 ===

    // 分离线区域（±kScissorsSeparatorTolerancePx 像素）：单击无效。
    // 与 updateScissorsPreview 的竖虚线消失范围一致——此处无竖虚线 = 不切割。
    {
        const auto tPlus = pixelXToSourceTime(e.x + kScissorsSeparatorTolerancePx);
        const double tol = tPlus ? std::abs(*tPlus - splitTime) : 1.0 / TimeCoordinate::kRenderSampleRate;
        for (const auto& note : beforeNotes) {
            if (std::abs(note.endTime - splitTime) < tol
                || std::abs(note.startTime - splitTime) < tol) {
                // 命中分离线，记录点击供双击合并，单击不切割
                scissorsLastClickTime_ = now;
                scissorsLastClickPos_ = e.getPosition();
                return;
            }
        }
    }

    // 获取 effective (corrected) F0 数据用于 pitch center 重算；
    // F0 数据不可用时保持原 pitch（不重算）。
    const auto effectiveF0 = loadEffectiveF0();
    const auto f0tl = ctx_.getF0Timeline();

    // 严格内部切割：切点时间竖线穿过的所有音符一律切割，与选中状态无关。
    const auto plan = planScissorsSplit(beforeNotes, splitTime, effectiveF0, f0tl);
    if (plan.cutCount == 0) {
        // 无音符可切割，记录本次点击供后续合并检测
        scissorsLastClickTime_ = now;
        scissorsLastClickPos_ = e.getPosition();
        return;
    }

    // 提交事务前交接旧参数目标；提交可能重排 notes 并清除旧选择。
    const auto before = beginParameterTargetChange();
    if (!ctx_.commitScissorsNotes(plan.afterNotes, LOC_RAW("Split note"))) {
        return;
    }

    // 记录本次点击的时间和位置，用于自定义双击合并检测
    scissorsLastClickTime_ = now;
    scissorsLastClickPos_ = e.getPosition();

    // 选中切割后的右侧段（最后一个被切割 note 的右段），避免下次拖拽同时移动两侧。
    if (plan.lastRightIndex >= 0
        && plan.lastRightIndex < static_cast<int>(plan.afterNotes.size())) {
        state.noteSelection.setSingle(plan.lastRightIndex,
                                      static_cast<int>(plan.afterNotes.size()));
    }
    finishParameterTargetChange(before, true);
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();

    // 不切换工具 —— 保持 Scissors，与 Melodyne 一致。
}

std::vector<float> PianoRollToolHandler::loadEffectiveF0() const
{
    const auto contentSnapshot = ctx_.getEditableContentSnapshot();
    const auto f0tl = ctx_.getF0Timeline();
    if (!contentSnapshot || f0tl.isEmpty())
        return {};

    std::vector<float> effectiveF0(static_cast<size_t>(f0tl.endFrameExclusive()), 0.0f);
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
    return effectiveF0;
}

bool PianoRollToolHandler::handleScissorsToolMerge(const juce::MouseEvent& e)
{
    auto& state = ctx_.getState();
    state.scissorsPreviewTime = -1.0;

    const auto beforeNotes = committedNotes(ctx_);
    const auto sourceTime = pixelXToSourceTime(e.x);
    if (!sourceTime)
        return false;
    const auto editRange = sourceEditRange();
    if (!editRange.contains(*sourceTime))
        return false;

    // 容差 = kScissorsSeparatorTolerancePx 像素对应的源时间：分离线两侧音符边界
    // 均来自 pixelXToSourceTime 的像素位置，此范围内视为同一时间点。
    // 与 updateScissorsPreview 的视觉反馈容差一致。
    const double splitTime = *sourceTime;
    const auto tPlus = pixelXToSourceTime(e.x + kScissorsSeparatorTolerancePx);
    const double tolerance = tPlus ? std::abs(*tPlus - splitTime) : 1.0 / TimeCoordinate::kRenderSampleRate;

    // 分离线 = left.endTime ≈ splitTime 且相邻 right.startTime ≈ splitTime。
    // 按序配对（left 取其后第一个未消费的 right），分离线两侧多个音符
    // （不同 pitch 行）各自独立合并。始终以鼠标命中的分离线为准，与选择无关。
    const auto effectiveF0 = loadEffectiveF0();
    const auto f0tl = ctx_.getF0Timeline();

    const auto plan = planScissorsMerge(beforeNotes, splitTime, tolerance, effectiveF0, f0tl);
    if (plan.mergeCount == 0)
        return false;

    const auto before = beginParameterTargetChange();
    if (!ctx_.commitScissorsNotes(plan.afterNotes, LOC_RAW("Merge notes"))) {
        return false;
    }
    finishParameterTargetChange(before, true);
    if (ctx_.invalidateSelectionFeedback)
        ctx_.invalidateSelectionFeedback();

    // 合并后不自动选中（Melodyne 行为）。
    return true;
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
    
    const auto contentSnapshot = ctx_.getEditableContentSnapshot();
    auto& handDrawBuffer = ctx_.getState().drawing.handDrawBuffer;
    if (contentSnapshot != nullptr && contentSnapshot->pitchCurve != nullptr
        && ctx_.getDirtyStartTime() >= 0.0 && ctx_.getDirtyEndTime() >= 0.0 && !handDrawBuffer.empty()) {
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

        auto ops = planManualCorrectionOps(
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
            ctx_.editManualCorrection(std::move(ops), editedStartFrame, editedEndFrameExclusive - 1,
                                      LOC_RAW("Hand-drawn curve"));
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

    // 提交规范化后的 source-time 音符范围。
    const DrawNoteEditInput input{
        startTime,
        endTime,
        ctx_.getDrawingNotePitch(),
        ctx_.getRetuneSpeed(),
        ctx_.getVibratoDepth(),
        ctx_.getVibratoRate()
    };
    const auto before = beginParameterTargetChange();
    const auto result = ctx_.commitDrawNote(
        input, ctx_.getNotesRevision(), LOC_RAW("Draw note"));

    int selectedNoteIndex = -1;
    if (result.status == DrawNoteCommitStatus::Applied && result.contentSnapshot != nullptr) {
        const double midTime = (startTime + endTime) / 2.0;
        selectedNoteIndex = findNoteIndexAt(
            result.contentSnapshot->notes, midTime, result.insertedNote.getAdjustedPitch(), 1.0f);
    }
    ctx_.applyDrawNoteResult(result, beforeNotes, selectedNoteIndex);
    if (result.status == DrawNoteCommitStatus::Applied)
        finishParameterTargetChange(before, true);
}

void PianoRollToolHandler::showToolContextMenu(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    ctx_.showToolSelectionMenu();
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

    const auto contentSnapshot = ctx_.getEditableContentSnapshot();
    if (contentSnapshot == nullptr || contentSnapshot->pitchCurve == nullptr) return;
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

    auto ops = planManualCorrectionOps(
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
    ctx_.editManualCorrection(std::move(ops), editedStartFrame, editedEndFrameExclusive - 1,
                              LOC_RAW("Anchor correction"));
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

// ============================================================================
// F0 选择：帧段收集与连续段查找
//
// 有效性判定统一为 effective F0（data * gain > 0），与 hitTestF0Curve 一致。
// ============================================================================

void PianoRollToolHandler::collectF0StretchesInRange(double startTime, double endTime, FrameSelection& out) const
{
    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) return;
    // 与 F0Timeline::rangeForTimes 一致的端点语义
    collectF0StretchesInFrames(f0tl.frameAtOrBefore(startTime),
                               f0tl.exclusiveFrameAt(endTime), out);
}

void PianoRollToolHandler::collectF0StretchesInFrames(int startFrame, int endFrameExclusive, FrameSelection& out) const
{
    if (endFrameExclusive <= startFrame) return;

    auto contentSnapshot = ctx_.getEditableContentSnapshot();
    if (!contentSnapshot) return;

    contentSnapshot->forEachEffectiveF0Span(startFrame, endFrameExclusive,
        [&](int spanStart, const float* data, int length, float gain) {
            for (int i = 0; i < length; ++i) {
                if (data[i] * gain > 0.0f)
                    out.addRange(spanStart + i, spanStart + i + 1);
            }
        });
}

FrameSelection PianoRollToolHandler::buildFrameSelectionForNotes(const std::vector<Note>& notes) const
{
    FrameSelection out;
    const auto& selectedIndices = ctx_.getState().noteSelection.selectedIndices;
    if (selectedIndices.empty()) return out;

    const auto f0tl = ctx_.getF0Timeline();
    if (f0tl.isEmpty()) return out;

    constexpr int kGapMarginFrames = 10;   // 音符前后间隙上下文（帧）
    for (int idx : selectedIndices) {
        if (idx < 0 || idx >= static_cast<int>(notes.size())) continue;
        const auto& note = notes[static_cast<size_t>(idx)];
        const auto range = f0tl.rangeForTimesWithMargin(note.startTime, note.endTime, kGapMarginFrames);
        collectF0StretchesInFrames(range.startFrame, range.endFrameExclusive, out);
    }
    return out;
}

bool PianoRollToolHandler::findF0ContiguousStretch(int frame, int& outStart, int& outEndExclusive) const
{
    auto contentSnapshot = ctx_.getEditableContentSnapshot();
    if (!contentSnapshot || contentSnapshot->pitchCurve == nullptr || contentSnapshot->pitchCurve->isEmpty())
        return false;

    const int n = static_cast<int>(contentSnapshot->pitchCurve->size());
    if (frame < 0 || frame >= n) return false;

    // 一次遍历构建有效帧掩码，再向两侧扩展
    std::vector<char> effective(static_cast<size_t>(n), 0);
    contentSnapshot->forEachEffectiveF0Span(0, n,
        [&](int spanStart, const float* data, int length, float gain) {
            for (int i = 0; i < length; ++i) {
                const size_t f = static_cast<size_t>(spanStart + i);
                if (f < effective.size() && data[i] * gain > 0.0f)
                    effective[f] = 1;
            }
        });

    if (!effective[static_cast<size_t>(frame)]) return false;   // 中心帧必须有效

    outStart = frame;
    outEndExclusive = frame + 1;
    while (outStart > 0 && effective[static_cast<size_t>(outStart - 1)]) --outStart;
    while (outEndExclusive < n && effective[static_cast<size_t>(outEndExclusive)]) ++outEndExclusive;
    return true;
}

FrameSelection PianoRollToolHandler::collectSelectedFrameRanges() const
{
    FrameSelection result = buildFrameSelectionForNotes(ctx_.getCommittedNotes());
    result.merge(ctx_.getState().frameSelection);
    return result;
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
            ctx_.notifyPlayheadChange(timelineTime, true);
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

    bool validationRejected = false;
    auto newSnap = planTimeGridDrag(
        tt.dragOriginalSnapshot,
        tt.draggedHandleId,
        tt.dragStartOutputSeconds,
        newOutputTime,
        tt.selectedHandleId,
        tt.additionalSelectedIds,
        tt.dragSnapDisabled,
        validationRejected);
    if (newSnap == nullptr) {
        if (validationRejected)
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
                            LOC_RAW("Drag time handle"));
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
    auto insert = planTimeGridInsert(snap, clickedTime);
    if (insert.status != TimeGridInsertStatus::Inserted) {
        switch (insert.status) {
            case TimeGridInsertStatus::OutputSpacing:
                AppLogger::log("[TimeTool] insert rejected: output spacing invariant");
                break;
            case TimeGridInsertStatus::SourceSpacing:
                AppLogger::log("[TimeTool] insert rejected: source spacing invariant");
                break;
            case TimeGridInsertStatus::OutsideRange:
                AppLogger::log("[TimeTool] insert rejected: click outside [ClipStart, ClipEnd] range");
                break;
            case TimeGridInsertStatus::ValidationRejected:
                AppLogger::warn("[TimeTool] insert: makeFromHandles validation failed");
                break;
            case TimeGridInsertStatus::Inserted:
            case TimeGridInsertStatus::InvalidTime:
            case TimeGridInsertStatus::InvalidGrid:
                break;
        }
        return;
    }

    ctx_.commitTimeGrid(insert.snapshot, snap, LOC_RAW("Insert time handle"));

    // Auto-select the newly-inserted handle so user can immediately drag.
    // commitTimeGrid already triggers cache dirty internally.
    auto& tt = ctx_.getState().timeTool;
    tt.selectedHandleId = insert.insertedHandleId;
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

    auto deletion = planTimeGridDelete(snap, tt.selectedHandleId);
    if (deletion.status != TimeGridDeleteStatus::Deleted) {
        if (deletion.status == TimeGridDeleteStatus::NotFound
            || deletion.status == TimeGridDeleteStatus::Endpoint)
            AppLogger::log("[TimeTool] delete rejected: cannot delete endpoint or unknown handle");
        else if (deletion.status == TimeGridDeleteStatus::ValidationRejected)
            AppLogger::warn("[TimeTool] delete: makeFromHandles validation failed");
        return false;
    }

    ctx_.commitTimeGrid(deletion.snapshot, snap, LOC_RAW("Delete time handle"));

    tt.selectedHandleId = 0;
    tt.hoveredHandleId  = 0;
    // commitTimeGrid already triggers cache dirty internally.
    return true;
}

// ============================================================================
// EQ Tool Handlers
// ============================================================================

void PianoRollToolHandler::handleEqToolMouseDown(const juce::MouseEvent& e)
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

    beginMouseParameterTargetChange();
    auto& noteSelection = ctx_.getState().noteSelection;
    const int noteCount = static_cast<int>(notes.size());
    if (noteSelection.isSelected(clickedNoteIndex)) {
        // 命中已选音符：保留组选中，并把该音符设为 anchor（add 内部维护 anchorIndex）
        noteSelection.add(clickedNoteIndex, noteCount);
    } else {
        noteSelection.setSingle(clickedNoteIndex, noteCount);
        ctx_.getState().frameSelection.clear();
    }
    if (ctx_.invalidateSelectionFeedback) ctx_.invalidateSelectionFeedback();

    // 记录 pending 主音符：不立即开弹窗，纯点击 mouseUp 时消费一次
    pendingEqPrimaryIndex_ = clickedNoteIndex;
}

void PianoRollToolHandler::handleEqToolMouseDrag(const juce::MouseEvent& e)
{
    // 仅音符点击手势进入：mouseDown 未命中音符的 empty-space 拖拽由
    // consumeEmptySpaceIntentDrag 处理，不经过此处。
    if (pendingEqPrimaryIndex_ < 0)
        return;

    const int dx = e.x - dragStartPos_.x;
    const int dy = e.y - dragStartPos_.y;
    const int threshold = ctx_.getDragThreshold();
    if (dx * dx + dy * dy <= threshold * threshold)
        return;

    // 超过拖拽阈值：清 pending，从 dragStartPos_ 起严格框选
    pendingEqPrimaryIndex_ = -1;
    const auto startEvent = e.withNewPosition(dragStartPos_.toFloat());
    beginAreaSelection(startEvent);
    handleSelectDrag(e);
}

void PianoRollToolHandler::handleEqToolMouseUp(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    // 框选路径由 handleSelectUp 完成（isSelectingArea 时 mouseUp 先于 switch 返回），
    // 此处只处理纯点击：消费 pending 主音符打开 EQ 弹窗一次。
    if (pendingEqPrimaryIndex_ >= 0) {
        const int primaryIndex = pendingEqPrimaryIndex_;
        pendingEqPrimaryIndex_ = -1;
        ctx_.openEqPreview(primaryIndex);
    }
}

} // namespace OpenTune
