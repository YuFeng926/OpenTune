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
#include "Utils/ContentTimelineProjection.h"
#include "Utils/KeyShortcutConfig.h"
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "UI/ToolIds.h"
#include "InteractionState.h"
#include "UI/ViewMapper.h"
#include "../../../Content/ContentEditCommands.h"
#include <vector>
#include <functional>
#include <cstdint>
#include <optional>

namespace OpenTune {

// ============================================================================
// SourceEditRange — source-domain 编辑边界
//
// 数据层 Note/F0/curve/anchor 存储 source time。TimeGrid 负责 source <-> output，
// Projection 负责 output <-> timeline。ToolHandler 编辑入口只操作 source-domain，
// 所以编辑范围必须由 TimeGridSnapshot::totalDurationSeconds() 推导，不是 projection。
//
// invalid projection 表示没有 edit target，返回空编辑时间。
// ============================================================================
struct SourceEditRange
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    double minDurationSeconds = 0.0;

    static SourceEditRange fromTimeGrid(const TimeGridSnapshot& grid,
                                        double minDurationSeconds = 0.0) noexcept
    {
        return {0.0, grid.totalDurationSeconds(), minDurationSeconds};
    }

    bool contains(double sourceSeconds) const noexcept
    {
        return endSeconds > startSeconds
            && sourceSeconds >= startSeconds
            && sourceSeconds <= endSeconds;
    }

    void normalizeForCommit(double& startSecondsInSource,
                            double& endSecondsInSource) const noexcept
    {
        if (startSecondsInSource > endSecondsInSource)
            std::swap(startSecondsInSource, endSecondsInSource);

        startSecondsInSource = juce::jlimit(startSeconds, endSeconds, startSecondsInSource);
        endSecondsInSource = juce::jlimit(startSeconds, endSeconds, endSecondsInSource);

        if (endSecondsInSource - startSecondsInSource < minDurationSeconds) {
            endSecondsInSource = juce::jmin(endSeconds, startSecondsInSource + minDurationSeconds);
            startSecondsInSource = juce::jmax(startSeconds, endSecondsInSource - minDurationSeconds);
        }
    }
};

class PianoRollToolHandler
{
public:
    // 手动修正操作描述：帧范围 + F0 数据 + 来源类型
    struct ManualCorrectionOp
    {
        int startFrame = 0;
        int endFrameExclusive = 0;
        std::vector<float> f0Data;
        PitchCorrectionSegment::Source source = PitchCorrectionSegment::Source::HandDraw;
    };

    // PianoRoll 组件提供的回调上下文。
    // 所有数据读写均通过这些 std::function 回调完成，
    // 使 ToolHandler 可独立于具体组件实例进行测试。
    struct Context
    {
        std::function<InteractionState&()> getState;

        // Coordinate mapper — replaces timeToX/xToTime/freqToY/yToFreq callbacks.
        // Returns ViewMapper by value to ensure fresh coordinate state.
        std::function<ViewMapper()> getViewMapper;

        std::function<const std::vector<Note>&()> getCommittedNotes;
        std::function<const std::vector<Note>&()> getDisplayNotes;
        std::function<NoteInteractionDraft&()> getNoteDraft;
        std::function<void()> beginNoteDraft;
        std::function<bool()> commitNoteDraft;
        std::function<void()> clearNoteDraft;
        // 第三参 affectedRange 来自 ToolHandler 编辑时计算的精确范围，用于
        // undo/redo 时只重渲染该范围（而不是 segments 列表反推的并集 = 全长）。
        std::function<ContentCommitSnapshot(const std::vector<Note>&, const std::vector<PitchCorrectionSegment>&, F0FrameRange)> commitNotesAndSegments;

        std::function<std::shared_ptr<PitchCurve>()> getPitchCurve;

        std::function<int()> getPianoKeyWidth;
        std::function<ContentTimelineProjection()> getContentProjection;
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

        std::function<int()> getNoteDragManualStartFrame;
        std::function<void(int)> setNoteDragManualStartFrame;
        std::function<int()> getNoteDragManualEndFrameExclusive;
        std::function<void(int)> setNoteDragManualEndFrameExclusive;
        std::function<std::vector<NoteDragManualTarget>&()> getNoteDragInitialManualTargets;
        std::function<std::vector<float>&()> getNoteDragPreviewF0;
        std::function<int()> getNoteDragPreviewStartFrame;
        std::function<void(int)> setNoteDragPreviewStartFrame;
        std::function<int()> getNoteDragPreviewEndFrameExclusive;
        std::function<void(int)> setNoteDragPreviewEndFrameExclusive;

        // === 精确 invalidation ===
        // 拖拽/缩放 note 内容变化时调用
        std::function<void(const std::vector<Note>&, const std::vector<Note>&)> invalidateLiveNotes;
        // 仅选择变化（Ctrl+A、点击选择、框选），不涉及 note 内容变化
        std::function<void()> invalidateSelectionFeedback;
        // 交互预览区域变化（hand draw、line anchor、draw note cursor preview）
        std::function<void(const juce::Rectangle<int>&)> invalidateInteractionPreview;
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

        // ============================================================
        // ⚡️ vocal-time-stretch §8.4/8.7 — Time tool / TimeGrid integration
        //
        // Component injects these for the Time tool to read/publish the
        // current content's TimeGrid snapshot.  All four callbacks
        // are optional: if the content has none (e.g., loose source
        // not yet bound), Time tool drag is suppressed by ToolHandler.
        // ============================================================
        std::function<std::shared_ptr<const TimeGridSnapshot>()> getActiveContentTimeGrid;
        // commitTimeGrid: publish (newSnapshot) and record undo with
        // (oldSnapshot) supplied by caller.  Returns true when the processor
        // accepted the snapshot (validation passed).
        std::function<bool(std::shared_ptr<const TimeGridSnapshot> /*newSnapshot*/,
                            std::shared_ptr<const TimeGridSnapshot> /*oldSnapshot*/,
                            juce::String /*description*/)> commitTimeGrid;
        // repaintTimeGridHandles: visual-only repaint for hover/select/drag
        // (handles are in paintOverChildren overlay, not in cache).
        std::function<void()> repaintTimeGridHandles;
    };

    explicit PianoRollToolHandler(Context context);

    void setTool(ToolId tool);
    void mouseMove(const juce::MouseEvent& e);
    void mouseDown(const juce::MouseEvent& e);
    void mouseDrag(const juce::MouseEvent& e);
    void mouseUp(const juce::MouseEvent& e);
    // ⚡️ §8.4 — Phase G: double-click insert (Time tool only)
    void mouseDoubleClick(const juce::MouseEvent& e);

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
    void clearLineAnchorPreview();

    // ⚡️ §8.4 — Time tool handlers (Phase F minimal scaffolding;
    // Phase G adds full drag math + double-click insert + Alt-snap-disable).
    void handleTimeToolMouseMove(const juce::MouseEvent& e);
    void handleTimeToolMouseDown(const juce::MouseEvent& e);
    void handleTimeToolMouseDrag(const juce::MouseEvent& e);
    void handleTimeToolMouseUp(const juce::MouseEvent& e);
    // §8.4 Phase G: double-click empty area to insert UserAdded handle.
    void handleTimeToolMouseDoubleClick(const juce::MouseEvent& e);
    // §8.4 Phase G: Delete key removes selected handle (non-endpoint).
    bool handleTimeToolDeleteSelected();
    // Hit-test handles within ±5 px of a TimeGrid handle's output_seconds.
    // Returns 0 if no hit.
    uint64_t hitTestTimeGridHandle(const juce::MouseEvent& e) const;

    void handleSelectDrag(const juce::MouseEvent& e);
    void handleDrawNoteDrag(const juce::MouseEvent& e);

    void handleSelectUp(const juce::MouseEvent& e);
    void handleDrawCurveUp(const juce::MouseEvent& e);
    void handleDrawNoteUp(const juce::MouseEvent& e);

    void showToolContextMenu(const juce::MouseEvent& e);

    bool isEmptySpaceMouseDown(const juce::MouseEvent& e);
    bool hitsNoteBodyOrResizeEdge(const juce::MouseEvent& e);
    bool hitTestF0Curve(const juce::MouseEvent& e, int& frameIndex) const;
    void beginF0SelectionAt(const juce::MouseEvent& e, int frameIndex);
    void updateF0SelectionDrag(const juce::MouseEvent& e);
    void beginEmptySpaceIntent(const juce::MouseEvent& e);
    bool consumeEmptySpaceIntentDrag(const juce::MouseEvent& e);
    bool consumeEmptySpaceIntentUp(const juce::MouseEvent& e);
    juce::MouseEvent eventAtEmptySpaceMouseDown(const juce::MouseEvent& e);
    void cancelActiveMouseGesture();

    void deleteSelectedNotes(std::vector<Note>& notes);
    void handleDeleteKey();

    // === Note 选择辅助 ===
    static int findNoteIndexAt(const std::vector<Note>& notes, double time, float targetPitchHz, float pitchToleranceHz);
    std::vector<int> collectSelectedNoteIndices(const std::vector<Note>& notes);
    void deselectAllNotes();
    void selectAllNotes(const std::vector<Note>& notes);
    int findLastSelectedNoteIndex(const std::vector<Note>& notes);
    void selectNotesBetween(const std::vector<Note>& notes, int startIndex, int endIndex);
    void updateF0SelectionFromNotes(const std::vector<Note>& notes);

    // ⚡️ vocal-time-stretch §8.5 — 唯一时间域转换链
    //
    // 数据层 Note/F0/curve/anchor 存储 source time。
    // TimeGrid 负责 source <-> output(content)。
    // Projection 负责 output(content) <-> timeline。
    // ViewMapper 负责 timeline <-> screen x。
    //
    // pixel → source: xToTime → projectTimelineTimeToContent → tauInverse
    // source → screen: tauForward → projectContentTimeToTimeline → timeToX
    //
    // 所有编辑入口只接受 source-domain double。
    // invalid projection 意味着没有 edit target，返回 nullopt。
    std::optional<double> pixelXToSourceTime(int pixelX) const;
    double sourceTimeToTimelineTime(double sourceSeconds) const;
    int sourceTimeToScreenX(double sourceSeconds) const;
    SourceEditRange sourceEditRange(double minDurationSeconds = 0.0) const;

    Context ctx_;
    ToolId currentTool_ = ToolId::Select;

    static constexpr int kEmptySpaceDragThreshold = 12;

    juce::Point<int> dragStartPos_;
    juce::Point<float> lastDrawPoint_;
};

} // namespace OpenTune
