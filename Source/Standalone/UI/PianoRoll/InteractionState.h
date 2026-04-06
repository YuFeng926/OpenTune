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
    // Temporary rubber-band drag state (only valid while isSelectingArea == true)
    bool isSelectingArea = false;
    double dragStartTime = 0.0;
    double dragEndTime = 0.0;
    float dragStartMidi = 0.0f;
    float dragEndMidi = 0.0f;
    
    // F0 frame range derived from selected notes
    int selectedF0StartFrame = -1;
    int selectedF0EndFrame = -1;
    bool hasF0Selection = false;
    
    void setF0Range(int startFrame, int endFrame);
    void clearF0Selection();
};

struct NoteDragState
{
    Note* draggedNote = nullptr;
    std::vector<std::pair<Note*, float>> initialNoteOffsets;
    bool isDraggingNotes = false;
    
    double manualStartTime = -1.0;
    double manualEndTime = -1.0;
    std::vector<std::pair<double, float>> initialManualTargets;
    
    void clear();
};

struct NoteResizeState
{
    bool isResizing = false;
    bool isDirty = false;
    Note* note = nullptr;
    NoteResizeEdge edge = NoteResizeEdge::None;
    double originalStartTime = 0.0;
    double originalEndTime = 0.0;
    
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
    
    void clearF0Drawing();
    void clearNoteDrawing();
    void clearAnchors();
};

class InteractionState
{
public:
    SelectionState selection;
    NoteDragState noteDrag;
    NoteResizeState noteResize;
    DrawingState drawing;
    
    bool isPanning = false;
    juce::Point<int> dragStartPos;
    
    bool drawNoteToolPendingDrag = false;
    juce::Point<int> drawNoteToolMouseDownPos;
    bool handDrawPendingDrag = false;
};

} // namespace OpenTune
