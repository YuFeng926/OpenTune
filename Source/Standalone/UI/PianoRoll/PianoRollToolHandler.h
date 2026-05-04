/**
 * PianoRoll 工具交互处理器（PianoRollToolHandler）
 *
 * 将鼠标/键盘事件路由到当前选中的编辑工具（Select / HandDraw / DrawNote /
 * LineAnchor / AutoTune），并通过 Context 回调与 PianoRollComponent 通信。
 * 本类不持有任何音频/编辑数据，只负责交互逻辑的状态机。
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Utils/AudioEditingScheme.h"
#include "Utils/F0Timeline.h"
#include "Utils/MaterializationTimelineProjection.h"
#include "Utils/KeyShortcutConfig.h"
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "UI/ToolIds.h"
#include "InteractionState.h"
#include <vector>
#include <functional>
#include <cstdint>

namespace OpenTune {

class PianoRollToolHandler
{
public:
    // 手动修正操作描述：帧范围 + F0 数据 + 来源类型
    struct ManualCorrectionOp
    {
        int startFrame = 0;
        int endFrameExclusive = 0;
        std::vector<float> f0Data;
        CorrectedSegment::Source source = CorrectedSegment::Source::HandDraw;
        float retuneSpeed = -1.0f;
    };

    // PianoRoll 组件提供的回调上下文。
    // 所有数据读写均通过这些 std::function 回调完成，
    // 使 ToolHandler 可独立于具体组件实例进行测试。
    struct Context
    {
        std::function<InteractionState&()> getState;

        std::function<double(int)> xToTime;
        std::function<int(double)> timeToX;
        std::function<float(float)> yToFreq;
        std::function<float(float)> freqToY;

        std::function<const std::vector<Note>&()> getCommittedNotes;
        std::function<const std::vector<Note>&()> getDisplayNotes;
        std::function<NoteInteractionDraft&()> getNoteDraft;
        std::function<void()> beginNoteDraft;
        std::function<bool()> commitNoteDraft;
        std::function<void()> clearNoteDraft;
        std::function<bool(const std::vector<Note>&, const std::vector<CorrectedSegment>&)> commitNotesAndSegments;

        std::function<std::shared_ptr<PitchCurve>()> getPitchCurve;

        std::function<int()> getPianoKeyWidth;
        std::function<MaterializationTimelineProjection()> getMaterializationProjection;
        std::function<double(double)> projectTimelineTimeToMaterialization;
        std::function<double(double)> projectMaterializationTimeToTimeline;
        std::function<juce::Rectangle<int>(const std::vector<Note>&)> getNotesBounds;
        std::function<juce::Rectangle<int>()> getSelectionBounds;
        std::function<juce::Rectangle<int>()> getHandDrawPreviewBounds;
        std::function<juce::Rectangle<int>()> getLineAnchorPreviewBounds;
        std::function<juce::Rectangle<int>()> getNoteDragCurvePreviewBounds;

        std::function<float()> getMinMidi;
        std::function<float()> getMaxMidi;
        std::function<float()> getRetuneSpeed;
        std::function<float()> getVibratoDepth;
        std::function<float()> getVibratoRate;
        std::function<AudioEditingScheme::Scheme()> getAudioEditingScheme;
        std::function<const KeyShortcutConfig::KeyShortcutSettings&()> getShortcutSettings;
        std::function<float(Note&)> recalculatePIP;

        std::function<double()> getDirtyStartTime;
        std::function<void(double)> setDirtyStartTime;
        std::function<double()> getDirtyEndTime;
        std::function<void(double)> setDirtyEndTime;

        std::function<double()> getDrawingNoteStartTime;
        std::function<void(double)> setDrawingNoteStartTime;
        std::function<double()> getDrawingNoteEndTime;
        std::function<void(double)> setDrawingNoteEndTime;
        std::function<float()> getDrawingNotePitch;
        std::function<void(float)> setDrawingNotePitch;
        std::function<int()> getDrawingNoteIndex;
        std::function<void(int)> setDrawingNoteIndex;

        std::function<bool()> getDrawNoteToolPendingDrag;
        std::function<void(bool)> setDrawNoteToolPendingDrag;
        std::function<juce::Point<int>()> getDrawNoteToolMouseDownPos;
        std::function<void(juce::Point<int>)> setDrawNoteToolMouseDownPos;
        std::function<int()> getDragThreshold;

        std::function<double()> getNoteDragManualStartTime;
        std::function<void(double)> setNoteDragManualStartTime;
        std::function<double()> getNoteDragManualEndTime;
        std::function<void(double)> setNoteDragManualEndTime;
        std::function<std::vector<std::pair<double, float>>&()> getNoteDragInitialManualTargets;
        std::function<std::vector<float>&()> getNoteDragPreviewF0;
        std::function<int()> getNoteDragPreviewStartFrame;
        std::function<void(int)> setNoteDragPreviewStartFrame;
        std::function<int()> getNoteDragPreviewEndFrameExclusive;
        std::function<void(int)> setNoteDragPreviewEndFrameExclusive;

        std::function<void(const juce::Rectangle<int>&)> invalidateVisual;
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
        std::function<void(size_t, float, float)> notifyNoteOffsetChanged;

        std::function<bool(std::vector<ManualCorrectionOp>, int, int, bool)> applyManualCorrection;
        std::function<bool(int, int)> selectNotesOverlappingFrames;
        std::function<std::vector<float>()> getOriginalF0;
        std::function<F0Timeline()> getF0Timeline;

        std::function<int(int, int)> findLineAnchorSegmentNear;
        std::function<void(int)> selectLineAnchorSegment;
        std::function<void(int)> toggleLineAnchorSegmentSelection;
        std::function<void()> clearLineAnchorSegmentSelection;

        std::function<void(juce::String)> setUndoDescription;
    };

    explicit PianoRollToolHandler(Context context);

    void setTool(ToolId tool) { currentTool_ = tool; }
    void mouseMove(const juce::MouseEvent& e);
    void mouseDown(const juce::MouseEvent& e);
    void mouseDrag(const juce::MouseEvent& e);
    void mouseUp(const juce::MouseEvent& e);

    bool keyPressed(const juce::KeyPress& key);

private:
    // === 各工具的 mouseDown/mouseDrag/mouseUp 分派 ===
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
    void handleDrawNoteDrag(const juce::MouseEvent& e);

    void handleSelectUp(const juce::MouseEvent& e);
    void handleDrawCurveUp(const juce::MouseEvent& e);
    void handleDrawNoteUp(const juce::MouseEvent& e);

    void showToolContextMenu(const juce::MouseEvent& e);

    void deleteSelectedNotes(std::vector<Note>& notes);
    void handleDeleteKey();

    // === Note 选择辅助 ===
    static int findNoteIndexAt(const std::vector<Note>& notes, double time, float targetPitchHz, float pitchToleranceHz);
    static std::vector<int> collectSelectedNoteIndices(const std::vector<Note>& notes);
    static void deselectAllNotes(std::vector<Note>& notes);
    static void selectAllNotes(std::vector<Note>& notes);
    static int findLastSelectedNoteIndex(const std::vector<Note>& notes);
    static void selectNotesBetween(std::vector<Note>& notes, int startIndex, int endIndex);
    void updateF0SelectionFromNotes(const std::vector<Note>& notes);

    Context ctx_;
    ToolId currentTool_ = ToolId::Select;

    juce::Point<int> dragStartPos_;
    juce::Point<float> lastDrawPoint_;
};

} // namespace OpenTune
