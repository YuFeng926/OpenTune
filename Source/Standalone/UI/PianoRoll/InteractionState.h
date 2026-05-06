#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <utility>
#include "Utils/Note.h"

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

struct ReferenceDragState
{
    bool isDragging = false;
    double dragStartOffsetSeconds = 0.0;  // offset value when drag began
    double dragStartMouseTime = 0.0;      // mouse time when drag began
    double currentOffset = 0.0;           // live offset during drag (not yet committed)
    
    void clear() { isDragging = false; dragStartOffsetSeconds = 0.0; dragStartMouseTime = 0.0; currentOffset = 0.0; }
};

class InteractionState
{
public:
    SelectionState selection;
    NoteInteractionDraft noteDraft;
    NoteDragState noteDrag;
    NoteResizeState noteResize;
    DrawingState drawing;
    ReferenceDragState referenceDrag;
    
    bool isPanning = false;
    juce::Point<int> dragStartPos;
    
    bool drawNoteToolPendingDrag = false;
    juce::Point<int> drawNoteToolMouseDownPos;
    bool handDrawPendingDrag = false;
    
    std::vector<int> selectedLineAnchorSegmentIds;
};

} // namespace OpenTune
