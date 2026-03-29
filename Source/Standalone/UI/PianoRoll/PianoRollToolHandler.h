#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "Utils/UndoAction.h"
#include "UI/ToolIds.h"
#include "PianoRollCorrectionWorker.h"
#include <vector>
#include <functional>
#include <cstdint>

namespace OpenTune {

/**
 * 音符调整边缘枚举
 * 标识音符时间边缘调整操作的类型
 */
enum class NoteResizeEdge
{
    None,
    Left,
    Right
};

/**
 * 钢琴卷帘工具处理器
 * 处理钢琴卷帘界面中的各种工具操作，包括选择、绘制音符、绘制曲线、线性锚点等
 * 通过 Context 回调与主界面通信，实现松耦合设计
 */
class PianoRollToolHandler
{
public:
    /**
     * 工具处理器上下文结构体
     * 提供工具操作所需的所有回调函数和状态访问接口
     */
    struct Context
    {
        std::function<double(int)> xToTime;
        std::function<int(double)> timeToX;
        std::function<float(float)> yToFreq;
        std::function<float(float)> freqToY;

        std::function<double(double)> clipSecondsToFrameIndex;
        std::function<std::pair<int, int>(double, double, int)> clipTimeRangeToFrameRangeHalfOpen;

        std::function<std::vector<Note>&()> getNotes;
        std::function<std::vector<Note*>()> getSelectedNotes;
        std::function<Note*(double, float, float)> findNoteAt;
        std::function<void()> deselectAllNotes;
        std::function<void()> selectAllNotes;
        std::function<void(const Note&)> insertNoteSorted;

        std::function<std::shared_ptr<PitchCurve>()> getPitchCurve;
        std::function<int()> getCurveSize;
        std::function<int()> getCurveHopSize;
        std::function<double()> getCurveSampleRate;
        std::function<void(int, int)> clearCorrectionRange;
        std::function<void()> clearAllCorrections;
        std::function<void(const CorrectedSegment&)> restoreCorrectedSegment;

        std::function<int()> getPianoKeyWidth;
        std::function<double()> getTrackOffsetSeconds;
        std::function<double()> getAudioSampleRate;
        std::function<juce::AudioBuffer<float>*()> getAudioBuffer;

        std::function<float()> getMinMidi;
        std::function<float()> getMaxMidi;
        std::function<float()> getRetuneSpeed;
        std::function<float()> getVibratoDepth;
        std::function<float()> getVibratoRate;
        std::function<float(Note&)> recalculatePIP;

        std::function<bool()> hasSelectionArea;
        std::function<double()> getSelectionStartTime;
        std::function<double()> getSelectionEndTime;
        std::function<float()> getSelectionStartMidi;
        std::function<float()> getSelectionEndMidi;
        std::function<void(bool)> setHasSelectionArea;
        std::function<void(double)> setSelectionStartTime;
        std::function<void(double)> setSelectionEndTime;
        std::function<void(float)> setSelectionStartMidi;
        std::function<void(float)> setSelectionEndMidi;

        std::function<bool()> getIsDrawingF0;
        std::function<void(bool)> setIsDrawingF0;
        std::function<std::vector<float>&()> getHandDrawBuffer;
        std::function<int()> getDirtyRangeStart;
        std::function<void(int)> setDirtyRangeStart;
        std::function<int()> getDirtyRangeEnd;
        std::function<void(int)> setDirtyRangeEnd;

        std::function<bool()> getIsDrawingNote;
        std::function<void(bool)> setIsDrawingNote;
        std::function<double()> getDrawingNoteStartTime;
        std::function<void(double)> setDrawingNoteStartTime;
        std::function<double()> getDrawingNoteEndTime;
        std::function<void(double)> setDrawingNoteEndTime;
        std::function<float()> getDrawingNotePitch;
        std::function<void(float)> setDrawingNotePitch;
        std::function<int()> getDrawingNoteIndex;
        std::function<void(int)> setDrawingNoteIndex;

        std::function<bool()> getIsDraggingNotes;
        std::function<void(bool)> setIsDraggingNotes;
        std::function<bool()> getDrawNoteToolPendingDrag;
        std::function<void(bool)> setDrawNoteToolPendingDrag;
        std::function<juce::Point<int>()> getDrawNoteToolMouseDownPos;
        std::function<void(juce::Point<int>)> setDrawNoteToolMouseDownPos;
        std::function<int()> getDragThreshold;

        std::function<bool()> getHandDrawPendingDrag;
        std::function<void(bool)> setHandDrawPendingDrag;

        std::function<int()> getNoteDragManualStartFrame;
        std::function<void(int)> setNoteDragManualStartFrame;
        std::function<int()> getNoteDragManualEndFrame;
        std::function<void(int)> setNoteDragManualEndFrame;
        std::function<std::vector<std::pair<int, float>>&()> getNoteDragInitialManualTargets;

        std::function<void()> requestRepaint;
        std::function<void(const juce::MouseCursor&)> setMouseCursor;
        std::function<void()> grabKeyboardFocus;
        std::function<void(ToolId)> setCurrentTool;
        std::function<void()> showToolSelectionMenu;

        std::function<void(double)> notifyPlayheadChange;
        std::function<void(int, int)> notifyPitchCurveEdited;
        std::function<void()> notifyAutoTuneRequested;
        std::function<void()> notifyPlayPauseToggle;
        std::function<void()> notifyStopPlayback;
        std::function<void()> notifyEscapeKey;
        std::function<void(Note*, float, float)> notifyNoteOffsetChanged;

        std::function<void()> undo;
        std::function<void()> redo;
        std::function<void(const juce::String&)> beginNotesEditUndo;
        std::function<void(const juce::String&)> commitNotesEditUndo;
        std::function<void(const juce::String&)> beginF0EditUndo;
        std::function<void(const juce::String&)> commitF0EditUndo;
        std::function<void(std::unique_ptr<UndoAction>)> addActionToUndoManager;
        std::function<bool()> isNotesEditUndoActive;

        std::function<void(std::vector<PianoRollCorrectionWorker::AsyncCorrectionRequest::ManualOp>, int, int, bool)> enqueueManualCorrection;
        std::function<void(int, int, int, float, float, float)> enqueueNoteBasedCorrection;
        std::function<std::vector<float>()> getOriginalF0;
        std::function<bool()> isAutoTuneProcessing;
        std::function<void(bool)> setAutoTuneProcessing;

        std::function<bool()> getIsPlacingAnchors;
        std::function<void(bool)> setIsPlacingAnchors;
        std::function<std::vector<LineAnchor>&()> getPendingAnchors;
        std::function<void(const juce::Point<float>&)> setCurrentMousePos;
        std::function<void()> clearPendingAnchors;
        std::function<bool()> getIsLineAnchorToolActive;

        std::function<int(int, int)> findLineAnchorSegmentNear;
        std::function<void(int)> selectLineAnchorSegment;
        std::function<void(int)> toggleLineAnchorSegmentSelection;
        std::function<void()> clearLineAnchorSegmentSelection;
    };

    explicit PianoRollToolHandler(Context context);

    void setTool(ToolId tool) { currentTool_ = tool; }
    ToolId getTool() const { return currentTool_; }

    void mouseMove(const juce::MouseEvent& e);
    void mouseDown(const juce::MouseEvent& e);
    void mouseDrag(const juce::MouseEvent& e);
    void mouseUp(const juce::MouseEvent& e);
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel);

    bool keyPressed(const juce::KeyPress& key);

    void cancelDrag();

    bool isUserInteracting() const { return userIsInteracting_; }
    bool isDragging() const { return isDragging_; }
    bool isSelectingArea() const { return isSelectingArea_; }

private:
    void handleSelectTool(const juce::MouseEvent& e);
    void handleDrawCurveTool(const juce::MouseEvent& e);
    void handleDrawNoteTool(const juce::MouseEvent& e);
    void handleDrawNoteMouseDown(const juce::MouseEvent& e);
    void handleAutoTuneTool(const juce::MouseEvent& e);
    void handleLineAnchorMouseDown(const juce::MouseEvent& e);
    void handleLineAnchorMouseDrag(const juce::MouseEvent& e);
    void handleLineAnchorMouseUp(const juce::MouseEvent& e);
    void commitLineAnchorOperation();

    void handleSelectDrag(const juce::MouseEvent& e);
    void handleDrawCurveDrag(const juce::MouseEvent& e);
    void handleDrawNoteDrag(const juce::MouseEvent& e);

    void handleSelectUp(const juce::MouseEvent& e);
    void handleDrawCurveUp(const juce::MouseEvent& e);
    void handleDrawNoteUp(const juce::MouseEvent& e);

    void showToolContextMenu(const juce::MouseEvent& e);

    void deleteSelectedNotes();
    void duplicateSelectedNotes();
    void handleDeleteKey();

    Context ctx_;
    ToolId currentTool_ = ToolId::Select;

    bool userIsInteracting_ = false;
    bool isDragging_ = false;
    bool isPanning_ = false;

    juce::Point<int> dragStartPos_;
    double dragStartTime_ = 0.0;
    float dragStartF0_ = 440.0f;

    Note* draggedNote_ = nullptr;
    float originalPitchOffset_ = 0.0f;
    std::vector<std::pair<Note*, float>> initialNoteOffsets_;

    bool isResizingNote_ = false;
    bool isResizingNoteDirty_ = false;
    Note* resizingNote_ = nullptr;
    NoteResizeEdge resizingEdge_ = NoteResizeEdge::None;
    double resizeStartTime_ = 0.0;
    double resizeEndTime_ = 0.0;

    bool drawCurveEditUndoActive_ = false;
    int drawCurveEditStartFrame_ = -1;
    int drawCurveEditEndFrame_ = -1;
    std::vector<float> drawCurveF0Before_;
    std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot> drawCurveSegmentsBefore_;
    
    juce::Point<float> lastDrawPoint_;

    bool isSelectingArea_ = false;
    bool handDrawPendingDrag_ = false;
};

} // namespace OpenTune
