#include "InteractionState.h"
#include "Utils/Note.h"

namespace OpenTune {

void SelectionState::setF0Range(int startFrame, int endFrame)
{
    selectedF0StartFrame = startFrame;
    selectedF0EndFrame = endFrame;
    hasF0Selection = true;
}

void SelectionState::clearF0Selection()
{
    selectedF0StartFrame = -1;
    selectedF0EndFrame = -1;
    hasF0Selection = false;
}

void NoteDragState::clear()
{
    draggedNote = nullptr;
    initialNoteOffsets.clear();
    isDraggingNotes = false;
    manualStartTime = -1.0;
    manualEndTime = -1.0;
    initialManualTargets.clear();
}

void NoteResizeState::clear()
{
    isResizing = false;
    isDirty = false;
    note = nullptr;
    edge = NoteResizeEdge::None;
    originalStartTime = 0.0;
    originalEndTime = 0.0;
}

void DrawingState::clearF0Drawing()
{
    isDrawingF0 = false;
    handDrawBuffer.clear();
    dirtyStartTime = -1.0;
    dirtyEndTime = -1.0;
}

void DrawingState::clearNoteDrawing()
{
    isDrawingNote = false;
    drawingNoteStartTime = 0.0;
    drawingNoteEndTime = 0.0;
    drawingNotePitch = 0.0f;
    drawingNoteIndex = -1;
}

void DrawingState::clearAnchors()
{
    isPlacingAnchors = false;
    pendingAnchors.clear();
    currentMousePos = juce::Point<float>();
}

} // namespace OpenTune
