#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <utility>
#include <memory>
#include <unordered_map>
#include "Utils/Note.h"
#include "Utils/AutomationLane.h"
#include "UI/ToolIds.h"
#include "Utils/TimeGrid.h"   // vocal-time-stretch §8.3 — Time tool

namespace OpenTune {

enum class NoteResizeEdge
{
    None,
    Left,
    Right
};

struct SelectionState
{
    bool hasSelectionArea = false;
    bool isSelectingArea = false;
    double selectionStartTime = 0.0;
    double selectionEndTime = 0.0;
    float selectionStartMidi = 0.0f;
    float selectionEndMidi = 0.0f;
    
    int selectedF0StartFrame = -1;
    int selectedF0EndFrameExclusive = -1;
    bool hasF0Selection = false;
    bool isSelectingF0 = false;
    int f0SelectionAnchorFrame = -1;
    
    void setF0Range(int startFrame, int endFrameExclusive);
    void clearF0Selection();
};

struct NoteSelectionState
{
    std::vector<int> selectedIndices;
    int anchorIndex = -1;

    void clear();
    void trimToNoteCount(int noteCount);
    bool isSelected(int noteIndex) const;
    void add(int noteIndex, int noteCount);
    void setSingle(int noteIndex, int noteCount);
    void toggle(int noteIndex, int noteCount);
    void selectAll(int noteCount);
    void selectRange(int startIndex, int endIndex, const std::vector<Note>& notes);
    void setFromIndices(std::vector<int> indices, int noteCount);
    bool empty() const noexcept { return selectedIndices.empty(); }
};

struct NoteDragManualTarget
{
    int frame = -1;
    float f0 = 0.0f;
};

struct NoteDragState
{
    int draggedNoteIndex = -1;
    std::vector<int> draggedNoteIndices;
    bool isDraggingNotes = false;
    
    int manualStartFrame = -1;
    int manualEndFrameExclusive = -1;
    std::vector<NoteDragManualTarget> initialManualTargets;
    int previewStartFrame = -1;
    int previewEndFrameExclusive = -1;
    std::vector<float> previewF0;
    
    void clear();
};

struct NoteResizeState
{
    bool isResizing = false;
    bool isDirty = false;
    int noteIndex = -1;
    NoteResizeEdge edge = NoteResizeEdge::None;
    double originalStartTime = 0.0;
    double originalEndTime = 0.0;
    
    void clear();
};

struct NoteInteractionDraft
{
    bool active = false;
    bool contentDirty = false;
    std::vector<Note> baselineNotes;
    std::vector<Note> workingNotes;

    void clear();
};

struct DrawingState
{
    bool isDrawingF0 = false;
    std::vector<float> handDrawBuffer;
    double dirtyStartTime = -1.0;
    double dirtyEndTime = -1.0;
    juce::Point<float> lastDrawPoint;
    
    bool isDrawingNote = false;
    double drawingNoteStartTime = 0.0;
    double drawingNoteEndTime = 0.0;
    float drawingNotePitch = 0.0f;

    bool isPlacingAnchors = false;
    std::vector<LineAnchor> pendingAnchors;
    juce::Point<float> currentMousePos;
    
};

struct EmptySpaceMouseIntent
{
    bool active = false;
    ToolId tool = ToolId::Select;
    juce::Point<int> mouseDownPos;
    double mouseDownTime = 0.0;

    void clear();
};

// vocal-time-stretch §8.3 — Time tool transient interaction state.
//
// Holds drag-time data so the renderer can show a preview while the user
// is mid-drag without committing to content owner until mouseUp.
//
// Selection model:
//   - Single click → selectedHandleId is the primary selection
//   - Shift+click → add/toggle in additionalSelectedIds
// Group drag → all selected handles move by the same delta
// Endpoint handles (ClipStart / ClipEnd) are not selectable / not draggable.
struct TimeToolState
{
    // Hovered handle id under the mouse cursor (0 = none).  Updated by
    // mouseMove for cursor-shape feedback and renderer highlight.
    uint64_t hoveredHandleId = 0;

    // Primary selected handle id (0 = none).  Survives across mouse events;
    // cleared on Escape, click-empty, or tool change.
    uint64_t selectedHandleId = 0;

    // Additional handle ids selected via Shift+click.
    // Group drag uses (selectedHandleId ∪ additionalSelectedIds) to compute
    // a uniform output-seconds delta that's applied to every member.
    std::vector<uint64_t> additionalSelectedIds;

    // Active drag state.
    bool   isDraggingHandle = false;
    uint64_t draggedHandleId = 0;

    // Alt modifier at drag start disables output-spacing clamp + future
    // snap-to-grid behavior.
    bool dragSnapDisabled = false;

    // Snapshot of the TimeGrid at drag start — used to recompute working
    // snapshot from drag delta (idempotent) instead of accumulating.
    std::shared_ptr<const TimeGridSnapshot> dragOriginalSnapshot;
    double dragStartOutputSeconds = 0.0;
    juce::Point<int> dragStartPixel;

    // Working snapshot during drag (preview).  Published to processor on
    // mouseUp via TimeGridEditAction.
    std::shared_ptr<const TimeGridSnapshot> dragWorkingSnapshot;

    // 拖动待定状态。handleMouseDown 命中 handle 后设为 true，
    // 但直到 mouseDrag 距离超过阈值才进入 isDraggingHandle。如果 mouseUp 时仍
    // 处于 dragPending（未越过阈值），则仅保留 selection 不提交 undo。
    bool dragPending = false;

    void clear()
    {
        hoveredHandleId = 0;
        selectedHandleId = 0;
        additionalSelectedIds.clear();
        isDraggingHandle = false;
        draggedHandleId = 0;
        dragSnapDisabled = false;
        dragOriginalSnapshot.reset();
        dragStartOutputSeconds = 0.0;
        dragStartPixel = {};
        dragWorkingSnapshot.reset();
        dragPending = false;
    }

    bool isSelected(uint64_t id) const
    {
        if (id == 0) return false;
        if (id == selectedHandleId) return true;
        for (uint64_t s : additionalSelectedIds) if (s == id) return true;
        return false;
    }
};

class InteractionState
{
public:
    SelectionState selection;
    NoteSelectionState noteSelection;
    NoteInteractionDraft noteDraft;
    NoteDragState noteDrag;
    NoteResizeState noteResize;
    DrawingState drawing;
    TimeToolState timeTool;          // ⚡️ §8.3 — Time tool

    // OpenDyne（NotesPrimary）瞬态：
    // Scissors 预览切点（content-local 绝对秒），-1 = 无预览。
    double scissorsPreviewTime = -1.0;
    // Volume Envelope Tool 拖拽预览。
    bool isVolumeDragging = false;
    AutomationLane volumePreviewEnvelope;
    // Modulation / Drift Tool 拖拽预览：参数值 + 哪个工具
    bool isModDriftDragging = false;
    float modDriftPreviewValue = 0.0f;
    ToolId modDriftTool = ToolId::PitchModulation;

    // Modulation/Drift 拖拽期间的临时 pitch curve 快照：
    // key = noteIndex（displayNotes 下标），value = 帧级 corrected F0（Hz）。
    // 由 ToolHandler 在拖拽中计算、mouseUp/中断时清空；renderer 在拖拽期间
    // 用它覆盖已提交 PitchCurve 的 corrected F0 曲线。非拖拽期间为空。
    std::unordered_map<size_t, std::vector<float>> tempPitchCurves;

    void clearTempPitchCurves() noexcept { tempPitchCurves.clear(); }

    bool isPanning = false;
    juce::Point<int> dragStartPos;
    
    bool drawNoteToolPendingDrag = false;
    juce::Point<int> drawNoteToolMouseDownPos;
    bool handDrawPendingDrag = false;
    EmptySpaceMouseIntent emptySpaceIntent;
    
    std::vector<int> selectedLineAnchorSegmentIds;

    // 清除全部瞬态（drag/resize/drawing/anchors/预览/工具状态），
    // 保留 noteSelection 与已完成的 selection 区域。
    void resetTransient() noexcept;
};

} // namespace OpenTune
