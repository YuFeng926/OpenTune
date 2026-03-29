#include "PianoRollToolHandler.h"
#include "PianoRollCorrectionWorker.h"
#include "../../../Utils/PitchUtils.h"
#include "../../../Utils/AppLogger.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace OpenTune {

using ManualOp = PianoRollCorrectionWorker::AsyncCorrectionRequest::ManualOp;

// ============================================================================
// PianoRollToolHandler - 钢琴卷帘工具处理器实现
// ============================================================================

PianoRollToolHandler::PianoRollToolHandler(Context context)
    : ctx_(std::move(context))
{
    AppLogger::debug("[PianoRollToolHandler] Created with default tool");
}

void PianoRollToolHandler::mouseMove(const juce::MouseEvent& e)
// 鼠标移动处理：更新光标形状（音符边缘调整、线锚点预览）
{
    if (currentTool_ == ToolId::LineAnchor && ctx_.getIsPlacingAnchors()) {
        ctx_.setCurrentMousePos(e.position);
        ctx_.requestRepaint();
        return;
    }

    if (currentTool_ != ToolId::Select) {
        return;
    }

    int edgeThreshold = 6;
    double offsetSeconds = ctx_.getTrackOffsetSeconds();
    
    bool cursorSet = false;
    float mousePitch = ctx_.yToFreq((float)e.y);
    auto mouseMidi = [mousePitch]() { return 69.0f + 12.0f * std::log2(mousePitch / 440.0f); };
    float mouseMidiVal = mouseMidi();

    for (const auto& note : ctx_.getNotes()) {
        int x1 = ctx_.timeToX(note.startTime + offsetSeconds) + ctx_.getPianoKeyWidth();
        int x2 = ctx_.timeToX(note.endTime + offsetSeconds) + ctx_.getPianoKeyWidth();
        
        bool nearLeft = std::abs(e.x - x1) <= edgeThreshold;
        bool nearRight = std::abs(e.x - x2) <= edgeThreshold;
        
        if (nearLeft || nearRight) {
            float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f);
            if (std::abs(mouseMidiVal - noteMidi) < 1.0f) {
                ctx_.setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
                cursorSet = true;
                break;
            }
        }
    }

    if (!cursorSet) {
        ctx_.setMouseCursor(juce::MouseCursor::NormalCursor);
    }
}

void PianoRollToolHandler::mouseDown(const juce::MouseEvent& e)
// 鼠标按下处理：根据当前工具分发到对应处理器，支持右键菜单、空格平移、时间轴点击
{
    AppLogger::debug("[PianoRollToolHandler] mouseDown: pos=(" + juce::String(e.x) + "," + juce::String(e.y) 
        + "), tool=" + juce::String(static_cast<int>(currentTool_)));

    ctx_.grabKeyboardFocus();

    if (e.mods.isPopupMenu()) {
        if (currentTool_ == ToolId::LineAnchor && ctx_.getIsPlacingAnchors()) {
            ctx_.commitF0EditUndo("Line Anchor");
            ctx_.setIsPlacingAnchors(false);
            ctx_.clearPendingAnchors();
            ctx_.clearLineAnchorSegmentSelection();
            ctx_.requestRepaint();
            return;
        }
        AppLogger::debug("[PianoRollToolHandler] mouseDown: showing context menu");
        showToolContextMenu(e);
        return;
    }

    if (juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey)) {
        isPanning_ = true;
        dragStartPos_ = e.getPosition();
        ctx_.setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        AppLogger::debug("[PianoRollToolHandler] mouseDown: starting pan mode");
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

    userIsInteracting_ = true;
    isDragging_ = true;
    dragStartPos_ = e.getPosition();
    dragStartTime_ = ctx_.xToTime(e.x);
    dragStartF0_ = ctx_.yToFreq((float)e.y);

    if (e.x > ctx_.getPianoKeyWidth()) {
        double clickedTime = ctx_.xToTime(e.x);
        if (clickedTime >= 0) {
            ctx_.notifyPlayheadChange(clickedTime);
        }
    }

    switch (currentTool_) {
        case ToolId::AutoTune:
            AppLogger::debug("[PianoRollToolHandler] mouseDown: handling AutoTune tool");
            handleAutoTuneTool(e);
            break;
        case ToolId::Select:
            AppLogger::debug("[PianoRollToolHandler] mouseDown: handling Select tool");
            handleSelectTool(e);
            break;
        case ToolId::DrawNote:
            AppLogger::debug("[PianoRollToolHandler] mouseDown: handling DrawNote tool");
            handleDrawNoteMouseDown(e);
            break;
        case ToolId::HandDraw:
            ctx_.setHandDrawPendingDrag(true);
            AppLogger::debug("[PianoRollToolHandler] mouseDown: HandDraw tool pending drag");
            break;
        case ToolId::LineAnchor:
            AppLogger::debug("[PianoRollToolHandler] mouseDown: handling LineAnchor tool");
            handleLineAnchorMouseDown(e);
            break;
        default:
            AppLogger::warn("[PianoRollToolHandler] mouseDown: unknown tool " + juce::String(static_cast<int>(currentTool_)));
            break;
    }
}

void PianoRollToolHandler::mouseDrag(const juce::MouseEvent& e)
// 鼠标拖拽处理：根据当前工具分发到对应的拖拽处理器
{
    if (isPanning_) {
        return;
    }

    if (!isDragging_) {
        return;
    }

    AppLogger::debug("[PianoRollToolHandler] mouseDrag: pos=(" + juce::String(e.x) + "," + juce::String(e.y) 
        + "), tool=" + juce::String(static_cast<int>(currentTool_)));

    switch (currentTool_) {
        case ToolId::Select:
            handleSelectDrag(e);
            break;
        case ToolId::DrawNote:
            handleDrawNoteDrag(e);
            break;
        case ToolId::HandDraw:
            if (ctx_.getHandDrawPendingDrag()) {
                int dx = e.x - dragStartPos_.x;
                int dy = e.y - dragStartPos_.y;
                int threshold = ctx_.getDragThreshold();
                if (dx * dx + dy * dy > threshold * threshold) {
                    ctx_.setHandDrawPendingDrag(false);
                    AppLogger::debug("[PianoRollToolHandler] mouseDrag: HandDraw threshold exceeded, starting curve draw");
                    handleDrawCurveTool(e);
                }
            } else if (ctx_.getIsDrawingF0()) {
                handleDrawCurveTool(e);
            }
            break;
        case ToolId::LineAnchor:
            handleLineAnchorMouseDrag(e);
            break;
        default:
            break;
    }

    ctx_.requestRepaint();
}

void PianoRollToolHandler::mouseUp(const juce::MouseEvent& e)
// 鼠标释放处理：完成当前操作，提交撤销记录
{
    AppLogger::debug("[PianoRollToolHandler] mouseUp: tool=" + juce::String(static_cast<int>(currentTool_)));

    if (isPanning_) {
        isPanning_ = false;
        ctx_.setMouseCursor(juce::MouseCursor::NormalCursor);
        AppLogger::debug("[PianoRollToolHandler] mouseUp: panning ended");
        return;
    }

    if (ctx_.hasSelectionArea()) {
        double timeDelta = std::abs(ctx_.getSelectionEndTime() - ctx_.getSelectionStartTime());
        float midiDelta = std::abs(ctx_.getSelectionEndMidi() - ctx_.getSelectionStartMidi());
        if (timeDelta < 0.01 || midiDelta < 0.5f) {
            AppLogger::debug("[PianoRollToolHandler] mouseUp: clearing small selection area");
            ctx_.setHasSelectionArea(false);
        }
    }

    switch (currentTool_) {
        case ToolId::Select:
            AppLogger::debug("[PianoRollToolHandler] mouseUp: handling Select tool");
            handleSelectUp(e);
            break;
        case ToolId::HandDraw:
            AppLogger::debug("[PianoRollToolHandler] mouseUp: handling HandDraw tool");
            handleDrawCurveUp(e);
            break;
        case ToolId::DrawNote:
            AppLogger::debug("[PianoRollToolHandler] mouseUp: handling DrawNote tool");
            handleDrawNoteUp(e);
            break;
        case ToolId::LineAnchor:
            AppLogger::debug("[PianoRollToolHandler] mouseUp: handling LineAnchor tool");
            handleLineAnchorMouseUp(e);
            break;
        default:
            isDragging_ = false;
            userIsInteracting_ = false;
            draggedNote_ = nullptr;
            break;
    }

    ctx_.requestRepaint();
}

void PianoRollToolHandler::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    juce::ignoreUnused(e, wheel);
}

bool PianoRollToolHandler::keyPressed(const juce::KeyPress& key)
// 键盘按下处理：处理快捷键（撤销、重做、全选、工具切换、播放控制、删除）
{
    AppLogger::debug("[PianoRollToolHandler] keyPressed: keyCode=" + juce::String(key.getKeyCode()));

    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) {
        AppLogger::debug("[PianoRollToolHandler] keyPressed: undo action");
        ctx_.undo();
        return true;
    }

    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) {
        AppLogger::debug("[PianoRollToolHandler] keyPressed: redo action");
        ctx_.redo();
        return true;
    }

    if (key == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)) {
        AppLogger::debug("[PianoRollToolHandler] keyPressed: select all");
        auto& notes = ctx_.getNotes();
        if (!notes.empty()) {
            ctx_.setHasSelectionArea(false);
            ctx_.selectAllNotes();
        } else {
            auto curve = ctx_.getPitchCurve();
            if (curve && !curve->isEmpty()) {
                ctx_.setHasSelectionArea(true);
                ctx_.setSelectionStartMidi(ctx_.getMinMidi());
                ctx_.setSelectionEndMidi(ctx_.getMaxMidi());
                ctx_.setSelectionStartTime(0.0);
                double curveSeconds = (static_cast<double>(curve->size()) * static_cast<double>(ctx_.getCurveHopSize())) / ctx_.getCurveSampleRate();
                ctx_.setSelectionEndTime(curveSeconds);
            }
        }
        ctx_.requestRepaint();
        return true;
    }

    if (!key.getModifiers().isAnyModifierKeyDown()) {
        if (key.getTextCharacter() == '2') {
            AppLogger::debug("[PianoRollToolHandler] keyPressed: switching to DrawNote tool");
            ctx_.setCurrentTool(ToolId::DrawNote);
            return true;
        }

        if (key.getTextCharacter() == '3') {
            AppLogger::debug("[PianoRollToolHandler] keyPressed: switching to Select tool");
            ctx_.setCurrentTool(ToolId::Select);
            return true;
        }

        if (key.getTextCharacter() == '4') {
            AppLogger::debug("[PianoRollToolHandler] keyPressed: switching to LineAnchor tool");
            ctx_.setCurrentTool(ToolId::LineAnchor);
            return true;
        }

        if (key.getTextCharacter() == '6') {
            AppLogger::debug("[PianoRollToolHandler] keyPressed: AutoTune requested");
            ctx_.notifyAutoTuneRequested();
            return true;
        }
    }

    if (key == juce::KeyPress::spaceKey) {
        AppLogger::debug("[PianoRollToolHandler] keyPressed: play/pause toggle");
        ctx_.notifyPlayPauseToggle();
        return true;
    }

    if (key == juce::KeyPress::returnKey) {
        AppLogger::debug("[PianoRollToolHandler] keyPressed: stop playback");
        ctx_.notifyStopPlayback();
        return true;
    }

    if (key.getTextCharacter() == '1' || key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) {
        AppLogger::debug("[PianoRollToolHandler] keyPressed: delete key pressed");
        handleDeleteKey();
        ctx_.requestRepaint();
        return true;
    }

    if (key == juce::KeyPress::escapeKey) {
        AppLogger::debug("[PianoRollToolHandler] keyPressed: escape key pressed");
        ctx_.notifyEscapeKey();
        return true;
    }

    return false;
}

void PianoRollToolHandler::handleDeleteKey()
// 删除键处理：删除选中的音符和选区内的内容，同时清除对应的音高修正
{
    AppLogger::debug("[PianoRollToolHandler] handleDeleteKey: starting delete operation");

    int globalDirtyStartFrame = INT_MAX;
    int globalDirtyEndFrame = INT_MIN;

    struct CorrectionDigest {
        int segCount = 0;
        int64_t totalSpan = 0;
        uint64_t checksum = 1469598103934665603ull;
        bool operator!=(const CorrectionDigest& other) const {
            return segCount != other.segCount || totalSpan != other.totalSpan || checksum != other.checksum;
        }
    };

    auto buildCorrectionDigest = [this]() -> CorrectionDigest {
        CorrectionDigest d;
        auto curve = ctx_.getPitchCurve();
        if (!curve) return d;
        auto snapshot = curve->getSnapshot();
        const auto& segs = snapshot->getCorrectedSegments();
        d.segCount = static_cast<int>(segs.size());
        for (const auto& seg : segs) {
            const uint64_t span = static_cast<uint64_t>(std::max(0, seg.endFrame - seg.startFrame));
            d.totalSpan += static_cast<int64_t>(span);
            d.checksum ^= static_cast<uint64_t>(seg.startFrame + 0x9e3779b9);
            d.checksum *= 1099511628211ull;
            d.checksum ^= static_cast<uint64_t>(seg.endFrame + 0x9e3779b9);
            d.checksum *= 1099511628211ull;
            d.checksum ^= static_cast<uint64_t>(seg.f0Data.size() + 0x9e3779b9);
            d.checksum *= 1099511628211ull;
            d.checksum ^= static_cast<uint64_t>(seg.source);
            d.checksum *= 1099511628211ull;
        }
        return d;
    };

    const CorrectionDigest correctionBeforeDelete = buildCorrectionDigest();

    double deleteStartTime = 1e30;
    double deleteEndTime = -1e30;

    if (ctx_.hasSelectionArea()) {
        deleteStartTime = std::min(ctx_.getSelectionStartTime(), ctx_.getSelectionEndTime());
        deleteEndTime = std::max(ctx_.getSelectionStartTime(), ctx_.getSelectionEndTime());
    }

    auto selectedNotes = ctx_.getSelectedNotes();
    for (const auto* n : selectedNotes) {
        deleteStartTime = std::min(deleteStartTime, n->startTime);
        deleteEndTime = std::max(deleteEndTime, n->endTime);
    }

    if (deleteEndTime <= deleteStartTime && selectedNotes.empty() && !ctx_.hasSelectionArea()) {
        AppLogger::debug("[PianoRollToolHandler] handleDeleteKey: nothing to delete");
        return;
    }

    AppLogger::debug("[PianoRollToolHandler] handleDeleteKey: deleting " 
        + juce::String(selectedNotes.size()) + " notes, hasSelection=" 
        + juce::String(ctx_.hasSelectionArea() ? "true" : "false"));

    int deleteStartFrame = -1;
    int deleteEndFrameExclusive = -1;
    auto curve = ctx_.getPitchCurve();

    if (deleteEndTime > deleteStartTime && curve && !curve->isEmpty()) {
        const double frameDuration = static_cast<double>(ctx_.getCurveHopSize()) / ctx_.getCurveSampleRate();
        deleteStartFrame = static_cast<int>(deleteStartTime / frameDuration);
        deleteEndFrameExclusive = static_cast<int>(deleteEndTime / frameDuration);
        const int maxFrameExclusive = ctx_.getCurveSize();
        if (deleteStartFrame < 0) deleteStartFrame = 0;
        if (deleteEndFrameExclusive > maxFrameExclusive) deleteEndFrameExclusive = maxFrameExclusive;
    }

    bool handled = false;

    if (!selectedNotes.empty()) {
        AppLogger::debug("[PianoRollToolHandler] handleDeleteKey: deleting notes");
        ctx_.beginNotesEditUndo("Delete Notes");

        double deletedNotesStartTime = 1e30;
        double deletedNotesEndTime = -1e30;
        for (const auto* n : selectedNotes) {
            deletedNotesStartTime = std::min(deletedNotesStartTime, n->startTime);
            deletedNotesEndTime = std::max(deletedNotesEndTime, n->endTime);
        }

        if (curve && deletedNotesEndTime > deletedNotesStartTime) {
            const double frameDuration = static_cast<double>(ctx_.getCurveHopSize()) / ctx_.getCurveSampleRate();
            int noteStartFrame = static_cast<int>(deletedNotesStartTime / frameDuration);
            int noteEndFrameExclusive = static_cast<int>(deletedNotesEndTime / frameDuration);
            if (noteEndFrameExclusive > noteStartFrame) {
                ctx_.clearCorrectionRange(noteStartFrame, noteEndFrameExclusive);
                globalDirtyStartFrame = std::min(globalDirtyStartFrame, noteStartFrame);
                globalDirtyEndFrame = std::max(globalDirtyEndFrame, noteEndFrameExclusive - 1);
            }
        }

        deleteSelectedNotes();
        ctx_.commitNotesEditUndo("Delete Notes");
        handled = true;
    }

    if (ctx_.hasSelectionArea() && deleteStartFrame >= 0 && deleteEndFrameExclusive > deleteStartFrame) {
        AppLogger::debug("[PianoRollToolHandler] handleDeleteKey: deleting in selection area");
        auto& notes = ctx_.getNotes();
        double startTime = std::min(ctx_.getSelectionStartTime(), ctx_.getSelectionEndTime());
        double endTime = std::max(ctx_.getSelectionStartTime(), ctx_.getSelectionEndTime());

        notes.erase(
            std::remove_if(notes.begin(), notes.end(),
                [startTime, endTime](const Note& n) {
                    return n.endTime > startTime && n.startTime < endTime;
                }),
            notes.end()
        );

        if (curve) {
            ctx_.clearCorrectionRange(deleteStartFrame, deleteEndFrameExclusive);
        }

        globalDirtyStartFrame = std::min(globalDirtyStartFrame, deleteStartFrame);
        globalDirtyEndFrame = std::max(globalDirtyEndFrame, deleteEndFrameExclusive - 1);
        ctx_.setHasSelectionArea(false);
        handled = true;
    }

    if (handled) {
        AppLogger::debug("[PianoRollToolHandler] handleDeleteKey: delete completed, notifying pitch curve edit");
        const CorrectionDigest correctionAfterDelete = buildCorrectionDigest();
        const bool correctionChanged = (correctionBeforeDelete != correctionAfterDelete);

        if (correctionChanged && globalDirtyEndFrame >= globalDirtyStartFrame && globalDirtyStartFrame != INT_MAX) {
            ctx_.notifyPitchCurveEdited(globalDirtyStartFrame, globalDirtyEndFrame);
        }
    }
}

void PianoRollToolHandler::cancelDrag()
// 取消拖拽操作：重置所有拖拽相关状态
{
    AppLogger::debug("[PianoRollToolHandler] cancelDrag: canceling all drag operations");
    isDragging_ = false;
    userIsInteracting_ = false;
    isPanning_ = false;
    isSelectingArea_ = false;
    isResizingNote_ = false;
    isResizingNoteDirty_ = false;
    resizingNote_ = nullptr;
    resizingEdge_ = NoteResizeEdge::None;
    draggedNote_ = nullptr;
    initialNoteOffsets_.clear();
    ctx_.setIsDraggingNotes(false);
}

void PianoRollToolHandler::handleSelectTool(const juce::MouseEvent& e)
// 选择工具鼠标按下处理：检测音符边缘调整、音符选中/取消选中、框选区域开始
{
    AppLogger::debug("[PianoRollToolHandler] handleSelectTool: pos=(" + juce::String(e.x) + "," + juce::String(e.y) + ")");
    
    isResizingNote_ = false;
    resizingNote_ = nullptr;
    resizingEdge_ = NoteResizeEdge::None;

    double clickedTime = ctx_.xToTime(e.x);
    double offsetSeconds = ctx_.getTrackOffsetSeconds();
    double trackRelativeTime = clickedTime - offsetSeconds;

    float clickedPitch = ctx_.yToFreq((float)e.y);

    Note* clickedNote = nullptr;
    if (trackRelativeTime >= 0) {
        clickedNote = ctx_.findNoteAt(trackRelativeTime, clickedPitch, 100.0f);
    }

    bool isCtrlDown = e.mods.isCtrlDown() || e.mods.isCommandDown();

 int edgeThreshold = 6;
     float mouseMidi = 69.0f + 12.0f * std::log2(clickedPitch / 440.0f);
     bool isShiftDown = e.mods.isShiftDown();
     
     auto& notes = ctx_.getNotes();
     for (auto& note : notes) {
         int x1 = ctx_.timeToX(note.startTime + offsetSeconds) + ctx_.getPianoKeyWidth();
         int x2 = ctx_.timeToX(note.endTime + offsetSeconds) + ctx_.getPianoKeyWidth();
         
         bool nearLeft = std::abs(e.x - x1) <= edgeThreshold;
         bool nearRight = std::abs(e.x - x2) <= edgeThreshold;
         
         if (nearLeft || nearRight) {
              float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f);
              if (std::abs(mouseMidi - noteMidi) < 1.0f) {
                  isResizingNote_ = true;
                  isResizingNoteDirty_ = false;
                  resizingNote_ = &note;
                  resizingEdge_ = nearLeft ? NoteResizeEdge::Left : NoteResizeEdge::Right;
                  resizeStartTime_ = note.startTime;
                  resizeEndTime_ = note.endTime;
                  
                  if (!note.selected && !isCtrlDown && !isShiftDown) {
                      ctx_.deselectAllNotes();
                  }
                  note.selected = true;
                  
                  ctx_.requestRepaint();
                  return;
              }
         }
     }

    if (clickedNote) {
        bool alreadyInDragGroup = false;
        if (ctx_.getIsDraggingNotes() && !initialNoteOffsets_.empty()) {
            for (const auto& p : initialNoteOffsets_) {
                if (p.first == clickedNote) {
                    alreadyInDragGroup = true;
                    break;
                }
            }
        }

        if (isCtrlDown) {
            clickedNote->selected = !clickedNote->selected;
        } else if (isShiftDown) {
            clickedNote->selected = true;
        } else if (alreadyInDragGroup) {
        } else {
            if (!isCtrlDown) {
                ctx_.deselectAllNotes();
                initialNoteOffsets_.clear();
                ctx_.setIsDraggingNotes(false);
            }
        }

        if (clickedNote->selected) {
            draggedNote_ = clickedNote;
            originalPitchOffset_ = clickedNote->pitchOffset;

            initialNoteOffsets_.clear();
            auto selectedNotes = ctx_.getSelectedNotes();
            for (auto* note : selectedNotes) {
                initialNoteOffsets_.push_back({ note, note->pitchOffset });
            }

            ctx_.setNoteDragManualStartFrame(-1);
            ctx_.setNoteDragManualEndFrame(-1);
            ctx_.getNoteDragInitialManualTargets().clear();

            auto pitchCurve = ctx_.getPitchCurve();
            if (pitchCurve && !initialNoteOffsets_.empty()) {
                double rangeStart = 0;
                double rangeEnd = 0;

                if (ctx_.hasSelectionArea()) {
                    rangeStart = std::min(ctx_.getSelectionStartTime(), ctx_.getSelectionEndTime());
                    rangeEnd = std::max(ctx_.getSelectionStartTime(), ctx_.getSelectionEndTime());
                } else {
                    rangeStart = 1e30;
                    rangeEnd = -1e30;
                    for (const auto& p : initialNoteOffsets_) {
                        rangeStart = std::min(rangeStart, p.first->startTime);
                        rangeEnd = std::max(rangeEnd, p.first->endTime);
                    }
                }

                if (rangeStart < 0) rangeStart = 0;
                
                auto* audioBuffer = ctx_.getAudioBuffer();
                if (audioBuffer) {
                    double maxTime = static_cast<double>(audioBuffer->getNumSamples()) / ctx_.getAudioSampleRate();
                    rangeEnd = std::min(rangeEnd, maxTime);
                }
                if (rangeEnd < rangeStart) std::swap(rangeStart, rangeEnd);

                const double frameDuration = static_cast<double>(ctx_.getCurveHopSize()) / ctx_.getCurveSampleRate();
                int startFrame = static_cast<int>(rangeStart / frameDuration);
                int endFrame = static_cast<int>(rangeEnd / frameDuration);
                if (startFrame < 0) startFrame = 0;
                if (endFrame < startFrame) endFrame = startFrame;

                auto originalF0 = ctx_.getOriginalF0();
                int maxFrame = static_cast<int>(originalF0.size());
                if (maxFrame > 0) {
                    startFrame = std::max(0, std::min(startFrame, maxFrame));
                    endFrame = std::max(0, std::min(endFrame, maxFrame));

                    if (endFrame > startFrame && pitchCurve->hasCorrectionInRange(startFrame, endFrame)) {
                        int rangeSize = endFrame - startFrame;
                        std::vector<float> renderedF0(rangeSize, -1.0f);

                        pitchCurve->renderF0Range(startFrame, endFrame,
                            [&](int frameIndex, const float* data, int length) {
                                if (data == nullptr || length <= 0) return;

                                int relStart = frameIndex - startFrame;
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

                        ctx_.setNoteDragManualStartFrame(startFrame);
                        ctx_.setNoteDragManualEndFrame(endFrame);

                        auto& manualTargets = ctx_.getNoteDragInitialManualTargets();
                        for (int relIdx = 0; relIdx < rangeSize; ++relIdx) {
                            int f = startFrame + relIdx;
                            float v = renderedF0[relIdx];
                            if (v <= 0.0f) continue;
                            manualTargets.push_back({ f, v });
                        }

                        if (manualTargets.empty()) {
                            ctx_.setNoteDragManualStartFrame(-1);
                            ctx_.setNoteDragManualEndFrame(-1);
                        }
                    }
                }
            }
        } else {
            draggedNote_ = nullptr;
            initialNoteOffsets_.clear();
        }

        ctx_.requestRepaint();
    } else {
        if (!isCtrlDown) {
            ctx_.deselectAllNotes();
            draggedNote_ = nullptr;
            initialNoteOffsets_.clear();
            isResizingNote_ = false;
            resizingNote_ = nullptr;
            resizingEdge_ = NoteResizeEdge::None;
            if (e.x > ctx_.getPianoKeyWidth()) {
                isSelectingArea_ = true;
                ctx_.setHasSelectionArea(true);
                ctx_.setSelectionStartTime(std::max(0.0, trackRelativeTime));
                ctx_.setSelectionEndTime(ctx_.getSelectionStartTime());
                float midiVal = 69.0f + 12.0f * std::log2(clickedPitch / 440.0f);
                ctx_.setSelectionStartMidi(midiVal);
                ctx_.setSelectionEndMidi(midiVal);
            } else {
                isSelectingArea_ = false;
                ctx_.setHasSelectionArea(false);
            }
            ctx_.requestRepaint();
        }
    }
}

void PianoRollToolHandler::handleDrawCurveTool(const juce::MouseEvent& e)
// 手绘曲线工具处理：将鼠标位置转换为F0值，在帧间进行对数插值，记录脏区域
{
    auto pitchCurve = ctx_.getPitchCurve();
    if (!pitchCurve) {
        AppLogger::warn("[PianoRollToolHandler] handleDrawCurveTool: pitchCurve is null");
        return;
    }

    double curveTime = ctx_.xToTime(e.x);
    auto* audioBuffer = ctx_.getAudioBuffer();
    if (audioBuffer != nullptr) {
        double maxTime = static_cast<double>(audioBuffer->getNumSamples()) / ctx_.getAudioSampleRate();
        curveTime = juce::jlimit(0.0, maxTime, curveTime);
    }

    float targetF0 = ctx_.yToFreq((float)e.y);

    const double frameDuration = static_cast<double>(ctx_.getCurveHopSize()) / ctx_.getCurveSampleRate();
    int frameIndex = static_cast<int>(curveTime / frameDuration);
    const auto& originalF0 = ctx_.getOriginalF0();
    if (frameIndex < 0 || static_cast<size_t>(frameIndex) >= originalF0.size()) {
        AppLogger::debug("[PianoRollToolHandler] handleDrawCurveTool: frameIndex out of range");
        return;
    }

    if (!ctx_.getIsDrawingF0()) {
        AppLogger::debug("[PianoRollToolHandler] handleDrawCurveTool: starting new curve draw at frame=" + juce::String(frameIndex));
        ctx_.setIsDrawingF0(true);
        ctx_.setDirtyRangeStart(frameIndex);
        ctx_.setDirtyRangeEnd(frameIndex);
        lastDrawPoint_ = juce::Point<float>(static_cast<float>(curveTime), targetF0);

        auto& handDrawBuffer = ctx_.getHandDrawBuffer();
        handDrawBuffer.clear();
        handDrawBuffer.resize(originalF0.size(), -1.0f);
        
        ctx_.beginF0EditUndo("Draw F0 Curve");
    } else {
        AppLogger::debug("[PianoRollToolHandler] handleDrawCurveTool: continuing draw at frame=" + juce::String(frameIndex) + ", f0=" + juce::String(targetF0));
    }

    auto& handDrawBuffer = ctx_.getHandDrawBuffer();
    
    double lastTime = static_cast<double>(lastDrawPoint_.x);

    auto writeFrame = [&](int f, float v) -> void {
        if (f < 0 || static_cast<size_t>(f) >= originalF0.size()) {
            return;
        }
        handDrawBuffer[(size_t)f] = v;
        int dirtyStart = ctx_.getDirtyRangeStart();
        int dirtyEnd = ctx_.getDirtyRangeEnd();
        dirtyStart = (dirtyStart < 0) ? f : std::min(dirtyStart, f);
        dirtyEnd = (dirtyEnd < 0) ? f : std::max(dirtyEnd, f);
        ctx_.setDirtyRangeStart(dirtyStart);
        ctx_.setDirtyRangeEnd(dirtyEnd);
    };

    int lastFrame = static_cast<int>(lastTime / frameDuration);
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
    ctx_.requestRepaint();
}

void PianoRollToolHandler::handleDrawNoteMouseDown(const juce::MouseEvent& e)
// 绘制音符工具鼠标按下处理：检测是否点击已有音符进行选择，设置待拖拽状态
{
    AppLogger::debug("[PianoRollToolHandler] handleDrawNoteMouseDown: pos=(" + juce::String(e.x) + "," + juce::String(e.y) + ")");
    
    auto& notes = ctx_.getNotes();
    
    Note* existingNote = nullptr;
    float clickedPitch = ctx_.yToFreq((float)e.y);
    float mouseMidi = 69.0f + 12.0f * std::log2(clickedPitch / 440.0f);
    
    double offsetSeconds = ctx_.getTrackOffsetSeconds();
    
    for (auto& note : notes) {
        int x1 = ctx_.timeToX(note.startTime + offsetSeconds) + ctx_.getPianoKeyWidth();
        int x2 = ctx_.timeToX(note.endTime + offsetSeconds) + ctx_.getPianoKeyWidth();
        float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f);
        
        if (e.x >= x1 && e.x <= x2 && std::abs(mouseMidi - noteMidi) < 1.0f) {
            existingNote = &note;
            break;
        }
    }
    
    if (existingNote != nullptr) {
        bool isCtrlDown = e.mods.isCtrlDown() || e.mods.isCommandDown();
        if (isCtrlDown) {
            existingNote->selected = !existingNote->selected;
        } else {
            if (!existingNote->selected) {
                ctx_.deselectAllNotes();
                existingNote->selected = true;
            }
        }
        ctx_.requestRepaint();
    }
    
    ctx_.setDrawNoteToolPendingDrag(true);
    ctx_.setDrawNoteToolMouseDownPos(e.getPosition());
}

void PianoRollToolHandler::handleDrawNoteTool(const juce::MouseEvent& e)
// 绘制音符工具处理：创建新音符或更新正在绘制的音符，音高自动对齐到半音
{
    double offsetSeconds = ctx_.getTrackOffsetSeconds();
    double currentTime = ctx_.xToTime(e.x) - offsetSeconds;
    if (currentTime < 0) {
        currentTime = 0;
    }

    float targetF0 = ctx_.yToFreq((float)e.y);
    float midiNote = 69.0f + 12.0f * std::log2(targetF0 / 440.0f);
    int roundedMidi = static_cast<int>(std::round(midiNote));
    float snappedF0 = 440.0f * std::pow(2.0f, (roundedMidi - 69) / 12.0f);

    if (!ctx_.getIsDrawingNote()) {
        AppLogger::debug("[PianoRollToolHandler] handleDrawNoteTool: starting note draw, midi=" + juce::String(roundedMidi) + ", time=" + juce::String(currentTime, 3));
        ctx_.beginNotesEditUndo("Draw Note");
        ctx_.setIsDrawingNote(true);
        ctx_.setDrawingNoteStartTime(currentTime);
        ctx_.setDrawingNoteEndTime(currentTime);
        ctx_.setDrawingNotePitch(snappedF0);

        Note newNote;
        newNote.startTime = currentTime;
        newNote.endTime = currentTime;
        newNote.pitch = snappedF0;
        newNote.pitchOffset = 0.0f;
        newNote.selected = false;
        newNote.dirty = true;
        
        float newPip = ctx_.recalculatePIP(newNote);
        
        if (newPip > 0.0f) {
            newNote.pitch = Note::midiToFrequency(Note::frequencyToMidi(newPip));
            newNote.originalPitch = newPip;
            int targetMidi = Note::frequencyToMidi(snappedF0);
            int sourceMidi = Note::frequencyToMidi(newNote.pitch);
            newNote.pitchOffset = static_cast<float>(targetMidi - sourceMidi);
        } else {
            newNote.pitch = snappedF0;
            newNote.originalPitch = snappedF0;
            newNote.pitchOffset = 0.0f;
        }

        auto& notes = ctx_.getNotes();
        notes.push_back(newNote);
        ctx_.setDrawingNoteIndex(static_cast<int>(notes.size()) - 1);

        ctx_.requestRepaint();
        return;
    }

    if (ctx_.getDrawingNoteIndex() >= 0) {
        ctx_.setDrawingNoteEndTime(currentTime);
        auto& notes = ctx_.getNotes();
        int idx = ctx_.getDrawingNoteIndex();
        if (idx < static_cast<int>(notes.size())) {
            double startTime = ctx_.getDrawingNoteStartTime();
            double endTime = ctx_.getDrawingNoteEndTime();
            notes[(size_t)idx].startTime = std::min(startTime, endTime);
            notes[(size_t)idx].endTime = std::max(startTime, endTime);
            notes[(size_t)idx].dirty = true;
        }
    }

    ctx_.requestRepaint();
}

void PianoRollToolHandler::handleAutoTuneTool(const juce::MouseEvent& e)
// 自动音调工具处理：触发自动音调生成请求
{
    juce::ignoreUnused(e);
    AppLogger::debug("[PianoRollToolHandler] handleAutoTuneTool: triggering AutoTune");
    ctx_.notifyAutoTuneRequested();
}

void PianoRollToolHandler::handleSelectDrag(const juce::MouseEvent& e)
// 选择工具拖拽处理：框选区域、音符边缘调整、音符拖拽移动
{
    if (isSelectingArea_) {
        double offsetSeconds = ctx_.getTrackOffsetSeconds();
        double currentTime = ctx_.xToTime(e.x) - offsetSeconds;
        ctx_.setSelectionEndTime(std::max(0.0, currentTime));
        
        float currentMidi = 69.0f + 12.0f * std::log2(ctx_.yToFreq((float)e.y) / 440.0f);
        ctx_.setSelectionEndMidi(currentMidi);
        
        double selStartTime = std::min(ctx_.getSelectionStartTime(), ctx_.getSelectionEndTime());
        double selEndTime = std::max(ctx_.getSelectionStartTime(), ctx_.getSelectionEndTime());
        float selMinMidi = std::min(ctx_.getSelectionStartMidi(), ctx_.getSelectionEndMidi());
        float selMaxMidi = std::max(ctx_.getSelectionStartMidi(), ctx_.getSelectionEndMidi());
        
        auto& notes = ctx_.getNotes();
        for (auto& note : notes) {
            float noteMidi = 69.0f + 12.0f * std::log2(note.getAdjustedPitch() / 440.0f);
            bool timeOverlap = (note.endTime > selStartTime && note.startTime < selEndTime);
            bool pitchOverlap = (noteMidi >= selMinMidi - 0.5f && noteMidi <= selMaxMidi + 0.5f);
            note.selected = timeOverlap && pitchOverlap;
        }
        
        ctx_.requestRepaint();
        return;
    }

    if (isResizingNote_ && resizingNote_) {
        if (!isResizingNoteDirty_) {
            isResizingNoteDirty_ = true;
            ctx_.beginNotesEditUndo("Resize Note");
        }
        
        double offsetSeconds = ctx_.getTrackOffsetSeconds();
        double currentTime = ctx_.xToTime(e.x) - offsetSeconds;
        currentTime = std::max(0.0, currentTime);
        double minDuration = 0.02;

        if (resizingEdge_ == NoteResizeEdge::Left) {
            double newStart = std::min(currentTime, resizingNote_->endTime - minDuration);
            newStart = std::max(0.0, newStart);
            resizingNote_->startTime = newStart;
        } else if (resizingEdge_ == NoteResizeEdge::Right) {
            double newEnd = std::max(currentTime, resizingNote_->startTime + minDuration);
            resizingNote_->endTime = newEnd;
        }

        resizingNote_->dirty = true;
        return;
    }

    if (draggedNote_) {
        if (initialNoteOffsets_.empty()) {
            return;
        }

        if (!ctx_.getIsDraggingNotes()) {
            ctx_.setIsDraggingNotes(true);
            ctx_.beginNotesEditUndo("Move Notes");
        }

        float startF0 = ctx_.yToFreq((float)dragStartPos_.y);
        float currentF0 = ctx_.yToFreq((float)e.y);

        float deltaSemitones = 0.0f;
        if (startF0 > 0.0f && currentF0 > 0.0f) {
            deltaSemitones = 12.0f * std::log2(currentF0 / startF0);
        }

        for (auto& pair : initialNoteOffsets_) {
            Note* note = pair.first;
            float initialOffset = pair.second;
            int baseMidi = note->getBaseMidiNote();
            float targetMidi = static_cast<float>(baseMidi) + initialOffset + deltaSemitones;
            float snappedOffset = std::round(targetMidi) - static_cast<float>(baseMidi);
            note->pitchOffset = snappedOffset;
            note->dirty = true;
        }

        float appliedDeltaSemitones = 0.0f;
        if (!initialNoteOffsets_.empty()) {
            appliedDeltaSemitones = initialNoteOffsets_[0].first->pitchOffset - initialNoteOffsets_[0].second;
        }
        float shiftFactor = std::pow(2.0f, appliedDeltaSemitones / 12.0f);

        for (auto& pair : initialNoteOffsets_) {
            float newPip = ctx_.recalculatePIP(*pair.first);
            if (newPip > 0.0f) {
                pair.first->originalPitch = newPip;
            }
        }

        auto& manualTargets = ctx_.getNoteDragInitialManualTargets();
        int manualStartFrame = ctx_.getNoteDragManualStartFrame();
        int manualEndFrame = ctx_.getNoteDragManualEndFrame();
        if (manualStartFrame >= 0 && manualEndFrame > manualStartFrame && !manualTargets.empty()) {
            int rangeSize = manualEndFrame - manualStartFrame;
            std::vector<float> shiftedF0(rangeSize, -1.0f);

            for (const auto& fv : manualTargets) {
                int f = fv.first;
                int relIdx = f - manualStartFrame;
                if (relIdx >= 0 && relIdx < rangeSize) {
                    shiftedF0[relIdx] = fv.second * shiftFactor;
                }
            }

            std::vector<ManualOp> ops;
            ManualOp op;
            op.type = ManualOp::Type::SetManualRange;
            op.startFrame = manualStartFrame;
            op.endFrameExclusive = manualEndFrame;
            op.f0Data = std::move(shiftedF0);
            op.source = CorrectedSegment::Source::HandDraw;
            ops.push_back(std::move(op));

            ctx_.enqueueManualCorrection(std::move(ops), manualStartFrame, manualEndFrame - 1, false);
        }
    }
}

void PianoRollToolHandler::handleDrawCurveDrag(const juce::MouseEvent& e)
{
    handleDrawCurveTool(e);
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
    } else if (ctx_.getIsDrawingNote()) {
        handleDrawNoteTool(e);
    }
}

void PianoRollToolHandler::handleSelectUp(const juce::MouseEvent& e)
// 选择工具鼠标释放处理：完成音符拖拽/调整/框选，提交音高修正
{
    juce::ignoreUnused(e);
    AppLogger::debug("[PianoRollToolHandler] handleSelectUp: finishing select operation");
    
    if (draggedNote_ && ctx_.getIsDraggingNotes()) {
        double dirtyStartTime = 1e30;
        double dirtyEndTime = -1e30;
        int changedCount = 0;

        for (const auto& pair : initialNoteOffsets_) {
            Note* note = pair.first;
            float initialOffset = pair.second;
            float finalOffset = note->pitchOffset;

            if (std::abs(finalOffset - initialOffset) > 0.001f) {
                ++changedCount;
                dirtyStartTime = std::min(dirtyStartTime, note->startTime);
                dirtyEndTime = std::max(dirtyEndTime, note->endTime);

                ctx_.notifyNoteOffsetChanged(note, initialOffset, finalOffset);
            }
        }

        bool hasManualTargets = ctx_.getIsDraggingNotes() && (ctx_.getNoteDragManualStartFrame() >= 0
            && ctx_.getNoteDragManualEndFrame() > ctx_.getNoteDragManualStartFrame()
            && !ctx_.getNoteDragInitialManualTargets().empty());

        if (ctx_.getIsDraggingNotes() && (dirtyEndTime > dirtyStartTime || hasManualTargets)) {
            int startFrame = INT_MAX;
            int endFrame = INT_MIN;

            if (dirtyEndTime > dirtyStartTime) {
                const double frameDuration = static_cast<double>(ctx_.getCurveHopSize()) / ctx_.getCurveSampleRate();
                startFrame = static_cast<int>(dirtyStartTime / frameDuration);
                endFrame = static_cast<int>(dirtyEndTime / frameDuration);
            }

            if (hasManualTargets) {
                startFrame = std::min(startFrame, ctx_.getNoteDragManualStartFrame());
                endFrame = std::max(endFrame, ctx_.getNoteDragManualEndFrame());
            }

            if (startFrame == INT_MAX || endFrame == INT_MIN) {
                startFrame = 0;
                endFrame = 0;
            }

            if (startFrame < 0) startFrame = 0;
            if (endFrame < startFrame) endFrame = startFrame;

            auto pitchCurve = ctx_.getPitchCurve();
            if (pitchCurve && !hasManualTargets) {
                ctx_.enqueueNoteBasedCorrection(startFrame, endFrame + 1, static_cast<int>(ctx_.getAudioSampleRate()),
                                               ctx_.getRetuneSpeed(), ctx_.getVibratoDepth(), ctx_.getVibratoRate());
            } else {
                ctx_.notifyPitchCurveEdited(startFrame, endFrame);
            }
        }
        
        initialNoteOffsets_.clear();
        ctx_.setNoteDragManualStartFrame(-1);
        ctx_.setNoteDragManualEndFrame(-1);
        ctx_.getNoteDragInitialManualTargets().clear();
    }

    bool resizeWasDirty = false;
    double resizedStartTime = 0;
    double resizedEndTime = 0;
    if (isResizingNote_ && resizingNote_ != nullptr && isResizingNoteDirty_) {
        resizeWasDirty = resizingNote_->dirty;
        resizedStartTime = resizingNote_->startTime;
        resizedEndTime = resizingNote_->endTime;
    }

    auto& notes = ctx_.getNotes();
    notes.erase(
        std::remove_if(notes.begin(), notes.end(), [](const Note& n) {
            return n.startTime >= n.endTime;
        }),
        notes.end()
    );
    
    if (ctx_.isNotesEditUndoActive()) {
        ctx_.commitNotesEditUndo(draggedNote_ != nullptr ? "Move Notes" : "Resize Notes");
    }

    if (isResizingNote_ && resizeWasDirty) {
        double dirtyStartTime = std::min(resizeStartTime_, resizedStartTime);
        double dirtyEndTime = std::max(resizeEndTime_, resizedEndTime);
        const double frameDuration = static_cast<double>(ctx_.getCurveHopSize()) / ctx_.getCurveSampleRate();
        int startFrame = static_cast<int>(dirtyStartTime / frameDuration);
        int endFrame = static_cast<int>(dirtyEndTime / frameDuration);
        if (startFrame < 0) startFrame = 0;
        if (endFrame < startFrame) endFrame = startFrame;

        auto pitchCurve = ctx_.getPitchCurve();
        if (pitchCurve) {
            ctx_.enqueueNoteBasedCorrection(startFrame, endFrame + 1, static_cast<int>(ctx_.getAudioSampleRate()),
                                           ctx_.getRetuneSpeed(), ctx_.getVibratoDepth(), ctx_.getVibratoRate());
        }
    }

    isResizingNote_ = false;
    isResizingNoteDirty_ = false;
    resizingNote_ = nullptr;
    resizingEdge_ = NoteResizeEdge::None;

    if (isSelectingArea_) {
        isSelectingArea_ = false;
        double timeDelta = std::abs(ctx_.getSelectionEndTime() - ctx_.getSelectionStartTime());
        float midiDelta = std::abs(ctx_.getSelectionEndMidi() - ctx_.getSelectionStartMidi());
        if (timeDelta < 0.01 || midiDelta < 0.5f) {
            ctx_.setHasSelectionArea(false);
        }
        auto selectedNotes = ctx_.getSelectedNotes();
        if (!selectedNotes.empty()) {
            initialNoteOffsets_.clear();
            for (auto* note : selectedNotes) {
                initialNoteOffsets_.push_back({ note, note->pitchOffset });
            }
            ctx_.setIsDraggingNotes(true);
        }
    }
    
    isDragging_ = false;
    userIsInteracting_ = false;
    draggedNote_ = nullptr;
}

void PianoRollToolHandler::handleDrawCurveUp(const juce::MouseEvent& e)
// 手绘曲线工具鼠标释放处理：将绘制的F0数据提交到音高修正队列
{
    juce::ignoreUnused(e);
    AppLogger::debug("[PianoRollToolHandler] handleDrawCurveUp: finishing curve draw");

    if (ctx_.getHandDrawPendingDrag()) {
        ctx_.setHandDrawPendingDrag(false);
        return;
    }

    if (!ctx_.getIsDrawingF0()) {
        return;
    }
    
    auto pitchCurve = ctx_.getPitchCurve();
    auto& handDrawBuffer = ctx_.getHandDrawBuffer();
    if (pitchCurve && ctx_.getDirtyRangeStart() >= 0 && ctx_.getDirtyRangeEnd() >= 0 && !handDrawBuffer.empty()) {
        int startFrame = std::min(ctx_.getDirtyRangeStart(), ctx_.getDirtyRangeEnd());
        int endFrame = std::max(ctx_.getDirtyRangeStart(), ctx_.getDirtyRangeEnd());

        const auto& originalF0 = ctx_.getOriginalF0();
        if (!originalF0.empty()) {
            int maxFrame = static_cast<int>(originalF0.size()) - 1;
            if (maxFrame >= 0) {
                startFrame = juce::jlimit(0, maxFrame, startFrame);
                endFrame = juce::jlimit(0, maxFrame, endFrame);
                if (endFrame < startFrame) std::swap(startFrame, endFrame);
            }
        }

        ctx_.beginF0EditUndo("Draw F0 Curve");

        std::vector<float> drawnF0;
        drawnF0.reserve(endFrame - startFrame + 1);
        for (int f = startFrame; f <= endFrame; ++f) {
            if (f >= 0 && f < static_cast<int>(handDrawBuffer.size())) {
                float val = handDrawBuffer[f];
                drawnF0.push_back(val >= 0.0f ? val : 0.0f);
            } else {
                drawnF0.push_back(0.0f);
            }
        }

        std::vector<ManualOp> ops;
        ManualOp op;
        op.type = ManualOp::Type::SetManualRange;
        op.startFrame = startFrame;
        op.endFrameExclusive = endFrame + 1;
        op.f0Data = std::move(drawnF0);
        op.source = CorrectedSegment::Source::HandDraw;
        ops.push_back(std::move(op));

        ctx_.enqueueManualCorrection(std::move(ops), startFrame, endFrame, false);
        ctx_.notifyPitchCurveEdited(startFrame, endFrame);
        ctx_.commitF0EditUndo("Draw F0 Curve");
    }

    ctx_.setIsDrawingF0(false);
    ctx_.setDirtyRangeStart(-1);
    ctx_.setDirtyRangeEnd(-1);
    ctx_.getHandDrawBuffer().clear();
}

void PianoRollToolHandler::handleDrawNoteUp(const juce::MouseEvent& e)
// 绘制音符工具鼠标释放处理：完成音符绘制，分割重叠音符，应用最小时长
{
    AppLogger::debug("[PianoRollToolHandler] handleDrawNoteUp: finishing note draw");

    if (ctx_.getDrawNoteToolPendingDrag()) {
        ctx_.setDrawNoteToolPendingDrag(false);
        return;
    }
    
    if (!ctx_.getIsDrawingNote()) {
        return;
    }
    
    ctx_.setIsDrawingNote(false);

    double offsetSeconds = ctx_.getTrackOffsetSeconds();
    double releaseTime = ctx_.xToTime(e.x) - offsetSeconds;
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

    auto& notes = ctx_.getNotes();
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
        ctx_.insertNoteSorted(finalNote);

        ctx_.deselectAllNotes();
        double midTime = (startTime + endTime) / 2.0;
        auto* newSelected = ctx_.findNoteAt(midTime, finalNote.getAdjustedPitch(), 100.0f);
        if (newSelected != nullptr) {
            newSelected->selected = true;
        }
    }

    ctx_.setDrawingNoteIndex(-1);

    {
        const double frameDuration = static_cast<double>(ctx_.getCurveHopSize()) / ctx_.getCurveSampleRate();
        int startFrame = static_cast<int>(startTime / frameDuration);
        int endFrame = static_cast<int>(endTime / frameDuration);
        if (startFrame < 0) startFrame = 0;
        if (endFrame < startFrame) endFrame = startFrame;

        auto pitchCurve = ctx_.getPitchCurve();
        if (pitchCurve) {
            ctx_.enqueueNoteBasedCorrection(startFrame, endFrame + 1, static_cast<int>(ctx_.getAudioSampleRate()),
                                           ctx_.getRetuneSpeed(), ctx_.getVibratoDepth(), ctx_.getVibratoRate());
        }
    }

    ctx_.commitNotesEditUndo("Draw Note");
    ctx_.requestRepaint();
}

void PianoRollToolHandler::showToolContextMenu(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    AppLogger::debug("[PianoRollToolHandler] showToolContextMenu: opening tool selection menu");
    ctx_.showToolSelectionMenu();
}

void PianoRollToolHandler::deleteSelectedNotes()
{
    auto& notes = ctx_.getNotes();
    int beforeCount = static_cast<int>(notes.size());
    notes.erase(
        std::remove_if(notes.begin(), notes.end(), [](const Note& n) {
            return n.selected;
        }),
        notes.end()
    );
    int afterCount = static_cast<int>(notes.size());
    AppLogger::debug("[PianoRollToolHandler] deleteSelectedNotes: removed " + juce::String(beforeCount - afterCount) + " notes");
}

void PianoRollToolHandler::duplicateSelectedNotes()
{
    auto selectedNotes = ctx_.getSelectedNotes();
    if (selectedNotes.empty()) {
        AppLogger::debug("[PianoRollToolHandler] duplicateSelectedNotes: no notes selected");
        return;
    }

    AppLogger::debug("[PianoRollToolHandler] duplicateSelectedNotes: duplicating " + juce::String(static_cast<int>(selectedNotes.size())) + " notes");
    auto& notes = ctx_.getNotes();
    for (auto* note : selectedNotes) {
        Note newNote = *note;
        newNote.selected = false;
        notes.push_back(newNote);
    }
}

void PianoRollToolHandler::handleLineAnchorMouseDown(const juce::MouseEvent& e)
// 线锚点工具鼠标按下处理：放置锚点，在锚点间生成线性插值的F0曲线
{
    const double sampleRate = ctx_.getCurveSampleRate();
    const int hopSize = ctx_.getCurveHopSize();
    if (sampleRate <= 0.0 || hopSize <= 0) return;

    const double frameDuration = static_cast<double>(hopSize) / sampleRate;
    const double offsetSeconds = ctx_.getTrackOffsetSeconds();
    const double clickTime = ctx_.xToTime(e.x) - offsetSeconds;
    float clickFreq = ctx_.yToFreq(static_cast<float>(e.y));
    clickFreq = std::max(20.0f, clickFreq);

    float midiNote = 69.0f + 12.0f * std::log2(clickFreq / 440.0f);
    int roundedMidi = static_cast<int>(std::round(midiNote));
    float snappedFreq = 440.0f * std::pow(2.0f, static_cast<float>(roundedMidi - 69) / 12.0f);

    if (e.getNumberOfClicks() >= 2 && ctx_.getIsPlacingAnchors()) {
        commitLineAnchorOperation();
        return;
    }

    if (!ctx_.getIsPlacingAnchors()) {
        int segmentIdx = ctx_.findLineAnchorSegmentNear(e.x, e.y);
        if (segmentIdx >= 0) {
            if (e.mods.isCtrlDown() || e.mods.isCommandDown()) {
                ctx_.toggleLineAnchorSegmentSelection(segmentIdx);
            } else {
                ctx_.selectLineAnchorSegment(segmentIdx);
            }
            ctx_.requestRepaint();
            return;
        }
        ctx_.clearLineAnchorSegmentSelection();

        ctx_.setIsPlacingAnchors(true);
        ctx_.beginF0EditUndo("Line Anchor");
        ctx_.getPendingAnchors().clear();
        LineAnchor firstAnchor;
        firstAnchor.time = clickTime;
        firstAnchor.freq = snappedFreq;
        firstAnchor.id = 0;
        firstAnchor.selected = false;
        ctx_.getPendingAnchors().push_back(firstAnchor);
        ctx_.setCurrentMousePos(e.position);
        ctx_.requestRepaint();
        return;
    }

    auto& anchors = ctx_.getPendingAnchors();
    const auto& prev = anchors.back();
    auto pitchCurve = ctx_.getPitchCurve();
    if (!pitchCurve) return;
    const auto& originalF0 = ctx_.getOriginalF0();
    if (originalF0.empty()) return;

    int prevFrame = static_cast<int>(prev.time / frameDuration);
    int currFrame = static_cast<int>(clickTime / frameDuration);
    int maxFrame = static_cast<int>(originalF0.size()) - 1;
    prevFrame = juce::jlimit(0, maxFrame, prevFrame);
    currFrame = juce::jlimit(0, maxFrame, currFrame);
    if (currFrame <= prevFrame) currFrame = prevFrame + 1;

    int startFrame = prevFrame;
    int endFrameExclusive = juce::jmin(currFrame + 1, maxFrame + 1);

    std::vector<float> f0Data;
    f0Data.reserve(endFrameExclusive - startFrame);
    float logA = std::log2(std::max(prev.freq, 1.0f));
    float logB = std::log2(std::max(snappedFreq, 1.0f));
    for (int f = startFrame; f < endFrameExclusive; ++f) {
        float t = static_cast<float>(f - startFrame) / static_cast<float>(endFrameExclusive - startFrame);
        f0Data.push_back(std::pow(2.0f, logA + (logB - logA) * t));
    }

    std::vector<PianoRollCorrectionWorker::AsyncCorrectionRequest::ManualOp> ops;
    PianoRollCorrectionWorker::AsyncCorrectionRequest::ManualOp op;
    op.type = PianoRollCorrectionWorker::AsyncCorrectionRequest::ManualOp::Type::SetManualRange;
    op.startFrame = startFrame;
    op.endFrameExclusive = endFrameExclusive;
    op.f0Data = std::move(f0Data);
    op.source = CorrectedSegment::Source::LineAnchor;
    op.retuneSpeed = ctx_.getRetuneSpeed();
    ops.push_back(std::move(op));

    ctx_.enqueueManualCorrection(std::move(ops), startFrame, endFrameExclusive - 1, false);
    ctx_.notifyPitchCurveEdited(startFrame, endFrameExclusive - 1);

    LineAnchor newAnchor;
    newAnchor.time = clickTime;
    newAnchor.freq = snappedFreq;
    newAnchor.id = static_cast<int>(anchors.size());
    newAnchor.selected = false;
    anchors.push_back(newAnchor);
    ctx_.setCurrentMousePos(e.position);
    ctx_.requestRepaint();
}

void PianoRollToolHandler::handleLineAnchorMouseDrag(const juce::MouseEvent& e) {
    if (!ctx_.getIsPlacingAnchors()) return;
    ctx_.setCurrentMousePos(e.position);
    ctx_.requestRepaint();
}

void PianoRollToolHandler::handleLineAnchorMouseUp(const juce::MouseEvent& e) {
    juce::ignoreUnused(e);
}

void PianoRollToolHandler::commitLineAnchorOperation()
// 提交线锚点操作：结束锚点放置，提交撤销记录
{
    ctx_.commitF0EditUndo("Line Anchor");
    ctx_.setIsPlacingAnchors(false);
    ctx_.getPendingAnchors().clear();
    ctx_.requestRepaint();
}

} // namespace OpenTune
