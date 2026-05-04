#include "InteractionState.h"
#include "Utils/Note.h"

namespace OpenTune {

void SelectionState::setF0Range(int startFrame, int endFrameExclusive)
{
    if (endFrameExclusive <= startFrame) {
        clearF0Selection();
        return;
    }
    selectedF0StartFrame = startFrame;
    selectedF0EndFrameExclusive = endFrameExclusive;
    hasF0Selection = true;
}

void SelectionState::clearF0Selection()
{
    selectedF0StartFrame = -1;
    selectedF0EndFrameExclusive = -1;
    hasF0Selection = false;
}

void NoteDragState::clear()
{
    draggedNoteIndex = -1;
    draggedNoteIndices.clear();
    isDraggingNotes = false;
    manualStartTime = -1.0;
    manualEndTime = -1.0;
    initialManualTargets.clear();
    previewStartFrame = -1;
    previewEndFrameExclusive = -1;
    previewF0.clear();
}

void NoteResizeState::clear()
{
    isResizing = false;
    isDirty = false;
    noteIndex = -1;
    edge = NoteResizeEdge::None;
    originalStartTime = 0.0;
    originalEndTime = 0.0;
}

void NoteInteractionDraft::clear()
{
    active = false;
    baselineNotes.clear();
    workingNotes.clear();
}

} // namespace OpenTune
