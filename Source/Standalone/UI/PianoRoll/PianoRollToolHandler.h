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
#include "../../../Utils/AutomationLane.h"
#include "Utils/PitchCurve.h"
#include "Utils/NoteGeneratorTypes.h"   // ScaleSnapConfig — OpenDyne Pitch 吸附
#include "Utils/UndoManager.h"          // UndoAction — OpenDyne Scissors undo
#include "UI/ToolIds.h"
#include "InteractionState.h"
#include "UI/ViewMapper.h"
#include "../../../Content/ContentEditCommands.h"
#include "../../../Content/EditableContentSnapshot.h"
#include <vector>
#include <functional>
#include <cstdint>
#include <optional>
#include <chrono>

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
        int contentOriginY;

        std::function<const std::vector<Note>&()> getCommittedNotes;
        std::function<const std::vector<Note>&()> getDisplayNotes;
        std::function<NoteInteractionDraft&()> getNoteDraft;
        std::function<void()> beginNoteDraft;
        std::function<bool()> commitNoteDraft;
        std::function<void()> clearNoteDraft;
        // 第三参 affectedRange 来自 ToolHandler 编辑时计算的精确范围，用于
        // undo/redo 时只重渲染该范围（而不是 segments 列表反推的并集 = 全长）。
        std::function<ContentCommitSnapshot(const std::vector<Note>&, const std::vector<PitchCorrectionSegment>&, F0FrameRange)> commitNotesAndSegments;

        // === OpenDyne（NotesPrimary）提交与配置回调 ===
        // Volume Envelope 提交：整体替换 AutomationLane，推进 outputGain/content
        // revision，内部 republish，零 render。
        std::function<ContentCommitSnapshot(AutomationLane, AutomationLane)> commitVolumeEnvelope;
        // 全量替换 notes（Scissors 分割）：只推进 notes/content revision，零 render。
        std::function<bool(const std::vector<Note>&)> replaceContentNotesForFullMutation;
        // 无渲染发布 playback source。
        std::function<void()> republishPlaybackSource;
        // Pitch Tool 拖拽吸附配置；nullopt = 无配置（默认 Chromatic，即 round 半音）。
        std::function<std::optional<ScaleSnapConfig>()> getActiveScaleSnap;
        // 把 undo action 推入组件 UndoManager。
        std::function<void(std::unique_ptr<UndoAction>)> pushUndoAction;

        std::function<std::shared_ptr<PitchCurve>()> getPitchCurve;
        std::function<std::shared_ptr<const EditableContentSnapshot>()> getEditableContentSnapshot;

        std::function<int()> getPianoKeyWidth;
        std::function<ContentTimelineProjection()> getContentProjection;
        std::function<juce::Rectangle<int>()> getHandDrawPreviewBounds;
        std::function<juce::Rectangle<int>()> getLineAnchorPreviewBounds;

        std::function<float()> getMinMidi;
        std::function<float()> getMaxMidi;
        std::function<float()> getRetuneSpeed;
        std::function<float()> getVibratoDepth;
        std::function<float()> getVibratoRate;
        std::function<AudioEditingScheme::Scheme()> getAudioEditingScheme;
        std::function<const KeyShortcutConfig::KeyShortcutSettings&()> getShortcutSettings;
        std::function<float(Note&)> calculateEffectivePIP;

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

        std::function<bool()> getDrawNoteToolPendingDrag;
        std::function<void(bool)> setDrawNoteToolPendingDrag;
        std::function<juce::Point<int>()> getDrawNoteToolMouseDownPos;
        std::function<void(juce::Point<int>)> setDrawNoteToolMouseDownPos;
        std::function<int()> getDragThreshold;

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

        // === EQ Tool ===
        // 打开 EQ 弹窗预览：参数为主音符 index（纯点击 = mouseDown 记录的 pending
        // 主音符；框选 = 完成选择后的 anchor 音符）。只打开弹窗，不改 Note 数据。
        std::function<void(int)> openEqPreview;
        // EQ 工具专属光标（组件侧构造一次，值拷贝共享 handle）。
        juce::MouseCursor eqCursor;

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
        // (handles are in the transparent overlay, not in the retained cache).
        std::function<void()> repaintTimeGridHandles;
    };

    explicit PianoRollToolHandler(Context context);

    void setTool(ToolId tool);
    void setPitchGridMode(PitchGridMode mode) { pitchGridMode_ = mode; }
    void mouseMove(const juce::MouseEvent& e);
    void mouseDown(const juce::MouseEvent& e);
    void mouseDrag(const juce::MouseEvent& e);
    void mouseUp(const juce::MouseEvent& e);
    // ⚡️ §8.4 — double-click insert (Time tool only)
    void mouseDoubleClick(const juce::MouseEvent& e);

    bool keyPressed(const juce::KeyPress& key);
    // 删除键命令入口（Delete/Backspace/'1'/Cut 共用）：删除选中音符与选区内容
    void handleDeleteKey();

private:
    // === 各工具的 mouseDown/mouseDrag/mouseUp 分派 ===
    void handleSelectTool(const juce::MouseEvent& e);
    void beginAreaSelection(const juce::MouseEvent& e);
    void handleDrawCurveTool(const juce::MouseEvent& e);
    void handleDrawNoteTool(const juce::MouseEvent& e);
    void handleDrawNoteMouseDown(const juce::MouseEvent& e);
    void handleAutoTuneTool(const juce::MouseEvent& e);
    void handleLineAnchorMouseDown(const juce::MouseEvent& e);
    void handleLineAnchorMouseDrag(const juce::MouseEvent& e);
    void clearLineAnchorPreview();

    // === OpenDyne（NotesPrimary）工具 ===
    // 唯一 pitch-drag 内部流程：OpenTune Select 与 OpenDyne Pitch 共用。
    void beginNotePitchDrag(const std::vector<Note>& notes);
    void dragNotePitch(const juce::MouseEvent& e);
    bool endNotePitchDrag(const juce::MouseEvent& e);
    // 唯一拖拽 editRange：只聚合实际变化的音符（普通 Pitch 比 pitchOffset、
    // Modulation 比有效 retuneSpeed、Drift 比 pitchDriftScale，阈值 0.001f），
    // 无变化或范围无效时返回空 F0FrameRange。预览与 mouseUp 提交共用。
    F0FrameRange noteDragEditRange(const std::vector<Note>& notes) const;
    // 拖拽预览：经 noteDragEditRange 获取 editRange，
    // 再经 buildNoteBasedCorrectionState 构建临时 snapshot 写入 noteDrag.previewSnapshot。
    void updateNoteBasedCorrectionPreview(const std::vector<Note>& notes);
    void handlePitchToolMouseDown(const juce::MouseEvent& e);
    void handlePitchToolDoubleClick(const juce::MouseEvent& e);
    void handlePitchToolMouseUp(const juce::MouseEvent& e);
    void handleVolumeEnvelopeToolMouseDown(const juce::MouseEvent& e);
    void handleVolumeEnvelopeToolDrag(const juce::MouseEvent& e);
    void handleVolumeEnvelopeToolUp(const juce::MouseEvent& e);
    void handleVolumeEnvelopeToolDoubleClick(const juce::MouseEvent& e);
    AutomationLane buildVolumeEnvelopeDragPreview(const std::vector<Note>& notes,
                                                   float deltaGainDb);
    void updateScissorsPreview(const juce::MouseEvent& e);
    void handleScissorsToolMouseDown(const juce::MouseEvent& e);
    void handleScissorsToolUp(const juce::MouseEvent& e);
    // 双击分离线：合并相邻音符（left.endTime ≈ right.startTime ≈ 点击时间）。
    // 命中分离线返回 true；未命中返回 false（调用方回退为切割）。
    bool handleScissorsToolMerge(const juce::MouseEvent& e);
    // Scissors 切割/合并共用：加载 effective (corrected) F0 数组（不可用返回空）。
    std::vector<float> loadEffectiveF0() const;
    // Scissors 切割/合并共用：区间 [tStart, tEnd) 内有效 F0 帧的算术平均，无有效帧返回 0。
    float computeAvgF0InRange(const F0Timeline& f0tl,
                              const std::vector<float>& effectiveF0,
                              double tStart,
                              double tEnd) const;
    
    // === EQ Tool ===
    void handleEqToolMouseDown(const juce::MouseEvent& e);
    void handleEqToolMouseDrag(const juce::MouseEvent& e);
    void handleEqToolMouseUp(const juce::MouseEvent& e);

    // ⚡️ §8.4 — Time tool handlers (drag math, double-click insert,
    // Alt-snap-disable, group multi-handle drag, output spacing clamp).
    void handleTimeToolMouseMove(const juce::MouseEvent& e);
    void handleTimeToolMouseDown(const juce::MouseEvent& e);
    void handleTimeToolMouseDrag(const juce::MouseEvent& e);
    void handleTimeToolMouseUp(const juce::MouseEvent& e);
    // §8.4: double-click empty area to insert UserAdded handle.
    void handleTimeToolMouseDoubleClick(const juce::MouseEvent& e);
    // §8.4: Delete key removes selected handle (non-endpoint).
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
    void cancelActiveMouseGesture();

    void deleteSelectedNotes(std::vector<Note>& notes);

    // === Note 选择辅助 ===
    int findNoteIndexAt(const std::vector<Note>& notes, double time, float targetPitchHz, float pitchToleranceSemitones);
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
    PitchGridMode pitchGridMode_ = PitchGridMode::KeyScale;

    // F2 连按计数（Melodyne-style: F2×1=Pitch, F2×2=Modulation, F2×3=Drift）
    int f2PressCount_ = 0;
    std::chrono::steady_clock::time_point lastF2PressTime_{};
    static constexpr int kF2DoubleClickMs = 400;

    // Scissors 自定义双击检测：记录上一次点击的时间戳和位置
    // 在 handleScissorsToolUp 中检测：800ms / 30px 内的同位置点击触发合并。
    std::chrono::steady_clock::time_point scissorsLastClickTime_{};
    juce::Point<int> scissorsLastClickPos_{};
    static constexpr int kScissorsDoubleClickMs = 800;  // 800ms 双击阈值
    static constexpr int kScissorsDoubleClickMaxDistPx = 30;  // 30像素位置容差
    // 分离线容差（像素）：鼠标在分离线 ±10px 内 → 不显示竖虚线、单击无效、双击合并。
    static constexpr int kScissorsSeparatorTolerancePx = 10;

    juce::Point<int> dragStartPos_;
    // EQ 工具：mouseDown 命中的 pending 主音符 index（纯点击 mouseUp 时消费一次）。
    // 拖拽超过阈值转为框选时、setTool/cancelActiveMouseGesture 时清理。
    int pendingEqPrimaryIndex_ = -1;
    double lastDrawTime_ = 0.0;
    float lastDrawF0_ = 0.0f;
    AutomationLane volumeDragBaselineEnvelope_;
};

} // namespace OpenTune
