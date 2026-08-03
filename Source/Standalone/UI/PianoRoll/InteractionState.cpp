#include "InteractionState.h"
#include "Utils/Note.h"
#include <algorithm>

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
    isSelectingF0 = false;
    f0SelectionAnchorFrame = -1;
}

void NoteSelectionState::clear()
{
    selectedIndices.clear();
    anchorIndex = -1;
}

void NoteSelectionState::trimToNoteCount(int noteCount)
{
    if (noteCount <= 0) {
        clear();
        return;
    }

    selectedIndices.erase(
        std::remove_if(selectedIndices.begin(), selectedIndices.end(),
                       [noteCount](int index) { return index < 0 || index >= noteCount; }),
        selectedIndices.end());
    std::sort(selectedIndices.begin(), selectedIndices.end());
    selectedIndices.erase(std::unique(selectedIndices.begin(), selectedIndices.end()), selectedIndices.end());

    if (anchorIndex < 0 || anchorIndex >= noteCount) {
        anchorIndex = selectedIndices.empty() ? -1 : selectedIndices.back();
    }
}

bool NoteSelectionState::isSelected(int noteIndex) const
{
    return std::find(selectedIndices.begin(), selectedIndices.end(), noteIndex) != selectedIndices.end();
}

void NoteSelectionState::add(int noteIndex, int noteCount)
{
    if (noteIndex < 0 || noteIndex >= noteCount) {
        trimToNoteCount(noteCount);
        return;
    }

    if (!isSelected(noteIndex)) {
        selectedIndices.push_back(noteIndex);
        std::sort(selectedIndices.begin(), selectedIndices.end());
        selectedIndices.erase(std::unique(selectedIndices.begin(), selectedIndices.end()), selectedIndices.end());
    }
    anchorIndex = noteIndex;
    trimToNoteCount(noteCount);
}

void NoteSelectionState::setSingle(int noteIndex, int noteCount)
{
    if (noteIndex < 0 || noteIndex >= noteCount) {
        clear();
        return;
    }

    selectedIndices = { noteIndex };
    anchorIndex = noteIndex;
}

void NoteSelectionState::toggle(int noteIndex, int noteCount)
{
    if (noteIndex < 0 || noteIndex >= noteCount) {
        trimToNoteCount(noteCount);
        return;
    }

    auto it = std::find(selectedIndices.begin(), selectedIndices.end(), noteIndex);
    if (it != selectedIndices.end()) {
        selectedIndices.erase(it);
        if (anchorIndex == noteIndex) {
            anchorIndex = selectedIndices.empty() ? -1 : selectedIndices.back();
        }
    } else {
        selectedIndices.push_back(noteIndex);
        std::sort(selectedIndices.begin(), selectedIndices.end());
        selectedIndices.erase(std::unique(selectedIndices.begin(), selectedIndices.end()), selectedIndices.end());
        anchorIndex = noteIndex;
    }

    trimToNoteCount(noteCount);
}

void NoteSelectionState::selectAll(int noteCount)
{
    if (noteCount <= 0) {
        clear();
        return;
    }

    selectedIndices.resize(static_cast<size_t>(noteCount));
    for (int i = 0; i < noteCount; ++i) {
        selectedIndices[static_cast<size_t>(i)] = i;
    }
    anchorIndex = noteCount - 1;
}

void NoteSelectionState::selectRange(int startIndex, int endIndex, const std::vector<Note>& notes)
{
    const int noteCount = static_cast<int>(notes.size());
    if (startIndex < 0 || endIndex < 0 || startIndex >= noteCount || endIndex >= noteCount) {
        trimToNoteCount(noteCount);
        return;
    }

    const double minTime = std::min(notes[static_cast<size_t>(startIndex)].startTime,
                                    notes[static_cast<size_t>(endIndex)].startTime);
    const double maxTime = std::max(notes[static_cast<size_t>(startIndex)].startTime,
                                    notes[static_cast<size_t>(endIndex)].startTime);

    for (int i = 0; i < noteCount; ++i) {
        const auto& note = notes[static_cast<size_t>(i)];
        if (note.startTime >= minTime && note.startTime <= maxTime
            && !isSelected(i)) {
            selectedIndices.push_back(i);
        }
    }
    anchorIndex = endIndex;
    trimToNoteCount(noteCount);
}

void NoteSelectionState::setFromIndices(std::vector<int> indices, int noteCount)
{
    selectedIndices = std::move(indices);
    anchorIndex = selectedIndices.empty() ? -1 : selectedIndices.back();
    trimToNoteCount(noteCount);
}

void NoteDragState::clear()
{
    draggedNoteIndex = -1;
    draggedNoteIndices.clear();
    isDraggingNotes = false;
    manualStartFrame = -1;
    manualEndFrameExclusive = -1;
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
    contentDirty = false;
    baselineNotes.clear();
    workingNotes.clear();
}

void EmptySpaceMouseIntent::clear()
{
    active = false;
    tool = ToolId::Select;
    mouseDownPos = {};
    mouseDownTime = 0.0;
}

void InteractionState::resetTransient() noexcept
{
    selection.isSelectingArea = false;
    selection.isSelectingF0 = false;
    selection.f0SelectionAnchorFrame = -1;
    noteDraft.clear();
    noteDrag.clear();
    noteResize.clear();
    drawing.isDrawingF0 = false;
    drawing.handDrawBuffer.clear();
    drawing.isDrawingNote = false;
    drawing.isPlacingAnchors = false;
    drawing.pendingAnchors.clear();
    timeTool.clear();
    scissorsPreviewTime = -1.0;
    isVolumeDragging = false;
    volumePreviewGainDb = 0.0f;
    isPanning = false;
    dragStartPos = {};
    drawNoteToolPendingDrag = false;
    drawNoteToolMouseDownPos = {};
    handDrawPendingDrag = false;
    emptySpaceIntent.clear();
    selectedLineAnchorSegmentIds.clear();
}

} // namespace OpenTune
