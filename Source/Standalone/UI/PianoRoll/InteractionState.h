#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <utility>
#include <memory>
#include "Utils/Note.h"
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
    
    void setF0Range(int startFrame, int endFrameExclusive);
    void clearF0Selection();
};

struct NoteDragState
{
    int draggedNoteIndex = -1;
    std::vector<int> draggedNoteIndices;
    bool isDraggingNotes = false;
    
    double manualStartTime = -1.0;
    double manualEndTime = -1.0;
    std::vector<std::pair<double, float>> initialManualTargets;
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
    int drawingNoteIndex = -1;
    
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
// is mid-drag without committing to MaterializationStore until mouseUp.
//
// Selection model:
//   - Single click → selectedHandleId is the primary selection
//   - Shift+click → add/toggle in additionalSelectedIds (Phase H)
//   - Group drag → all selected handles move by the same delta
// Endpoint handles (ClipStart / ClipEnd) are not selectable / not draggable.
struct TimeToolState
{
    // Hovered handle id under the mouse cursor (0 = none).  Updated by
    // mouseMove for cursor-shape feedback and renderer highlight.
    uint64_t hoveredHandleId = 0;

    // Primary selected handle id (0 = none).  Survives across mouse events;
    // cleared on Escape, click-empty, or tool change.
    uint64_t selectedHandleId = 0;

    // §8.4 (Phase H) — additional handle ids selected via Shift+click.
    // Group drag uses (selectedHandleId ∪ additionalSelectedIds) to compute
    // a uniform output-seconds delta that's applied to every member.
    std::vector<uint64_t> additionalSelectedIds;

    // Active drag state.
    bool   isDraggingHandle = false;
    uint64_t draggedHandleId = 0;

    // §8.4 (Phase H) — Alt modifier at drag start disables 30ms
    // minimum-spacing clamp + future snap-to-grid behavior.
    bool dragSnapDisabled = false;

    // Snapshot of the TimeGrid at drag start — used to recompute working
    // snapshot from drag delta (idempotent) instead of accumulating.
    std::shared_ptr<const TimeGridSnapshot> dragOriginalSnapshot;
    double dragStartOutputSeconds = 0.0;
    juce::Point<int> dragStartPixel;

    // Working snapshot during drag (preview).  Published to processor on
    // mouseUp via TimeGridEditAction.
    std::shared_ptr<const TimeGridSnapshot> dragWorkingSnapshot;

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
    NoteInteractionDraft noteDraft;
    NoteDragState noteDrag;
    NoteResizeState noteResize;
    DrawingState drawing;
    TimeToolState timeTool;          // ⚡️ §8.3 — Time tool
    
    bool isPanning = false;
    juce::Point<int> dragStartPos;
    
    bool drawNoteToolPendingDrag = false;
    juce::Point<int> drawNoteToolMouseDownPos;
    bool handDrawPendingDrag = false;
    EmptySpaceMouseIntent emptySpaceIntent;
    
    std::vector<int> selectedLineAnchorSegmentIds;
};

} // namespace OpenTune
