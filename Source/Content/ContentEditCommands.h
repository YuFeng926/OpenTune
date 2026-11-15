#pragma once

#include "ContentKey.h"
#include "../Utils/F0Timeline.h"
#include "../Utils/Note.h"
#include "../Utils/AutomationLane.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include "../Utils/NoteGeneratorTypes.h"
#include "../Utils/DetectedKey.h"
#include "../DSP/ReferenceFeatures.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace OpenTune {

class PitchShiftEditAction;
class PianoRollEditAction;
class TimeGridEditAction;
struct EditableContentSnapshot;

using ContentCommitSnapshot = std::shared_ptr<const EditableContentSnapshot>;

struct ContentEditRangeFrames {
    int startFrame{0};
    int endFrameExclusive{0};
};

using ContentEditRangeSeconds = NoteRangeSeconds;

struct ContentNoteRangePatch {
    ContentEditRangeSeconds affectedRange;
    std::vector<Note> afterNotesInRange;
};

struct ContentNoteValueEdit {
    enum class Kind { TargetPitch, ShiftCents, SetPitchOffset, Parameters };
    Kind kind = Kind::TargetPitch;
    std::size_t position = 0;
    double targetMidi = 0.0;
    double deltaCents = 0.0;
    double pitchOffset = 0.0;
    std::optional<double> retuneSpeed;
    std::optional<double> vibratoDepth;
    std::optional<double> vibratoRate;
    std::optional<double> pitchDriftScale;
};

struct ContentNoteValueEditRequest {
    std::vector<ContentNoteValueEdit> edits;
};

struct ContentNoteValueEditPlan {
    std::vector<Note> notes;
    std::vector<std::size_t> changedPositions;
};

struct ContentNoteCorrectionPlan {
    F0FrameRange affectedRange;
    std::vector<Note> notes;
    std::vector<PitchCorrectionSegment> segments;
    std::shared_ptr<const PitchCurveSnapshot> correctedCurve;
};

struct ContentAutoSnapPlan {
    enum class Status { NoChange, EmptyRange, Commit };
    Status status = Status::NoChange;
    std::vector<Note> notes;
    ContentNoteCorrectionPlan correction;
};

struct ContentEditCommitResult {
    ContentCommitSnapshot snapshot;
    // Non-owning: the content owner has already pushed this action into UndoManager.
    PianoRollEditAction* undoAction = nullptr;
};

struct ContentDetectedKeyEditResult {
    DetectedKey before;
    bool changed = false;
};

class ContentEditCommands
{
public:
    virtual ~ContentEditCommands() = default;

    virtual bool replaceContentNotesForFullMutation(ContentKey key,
                                                     std::vector<Note> notes) = 0;

    // 只替换 notes（mergeNotesRange 合并入 affectedRange）、推进 notesRevision/contentRevision、
    // 发布新 snapshot；调用方统一按 affectedRange 调度局部 Stage1 重渲染；
    // 不推进 pitchRevision、不写 correction segments。
    virtual ContentCommitSnapshot commitNoteTopologyPatch(ContentKey key,
                                                          ContentNoteRangePatch patch) = 0;

    virtual ContentCommitSnapshot commitNotesAndSegments(
        ContentKey key,
        std::vector<Note> notes,
        std::vector<PitchCorrectionSegment> segments,
        ContentEditRangeFrames affectedRange) = 0;

    virtual ContentCommitSnapshot commitNotesAndSegmentsInRanges(
        ContentKey key,
        std::vector<Note> notes,
        std::vector<PitchCorrectionSegment> segments,
        const std::vector<ContentEditRangeFrames>& affectedRanges) = 0;

    virtual ContentCommitSnapshot commitNotesAndSegmentsWithOriginalF0(
        ContentKey key,
        std::vector<Note> notes,
        std::vector<PitchCorrectionSegment> segments,
        ContentEditRangeFrames affectedRange,
        std::vector<float> originalF0InRange,
        std::optional<std::vector<F0FrameRange>> erasedRanges = std::nullopt) = 0;

    virtual ContentEditCommitResult commitNotesAndSegmentsUndoable(
        ContentKey key,
        std::vector<Note> notes,
        std::vector<PitchCorrectionSegment> segments,
        ContentEditRangeFrames affectedRange,
        const juce::String& description) = 0;

    virtual ContentEditCommitResult commitNotesAndSegmentsUndoableInRanges(
        ContentKey key,
        std::vector<Note> notes,
        std::vector<PitchCorrectionSegment> segments,
        const std::vector<ContentEditRangeFrames>& affectedRanges,
        const juce::String& description) = 0;

    virtual std::unique_ptr<PianoRollEditAction> createPianoRollEditAction(
        ContentKey key,
        const std::vector<Note>& beforeNotes,
        const std::vector<PitchCorrectionSegment>& beforeSegments,
        ContentCommitSnapshot after,
        const std::vector<ContentEditRangeFrames>& affectedRanges,
        const juce::String& description) const = 0;

    virtual std::optional<ContentNoteCorrectionPlan> planNoteCorrection(
        const EditableContentSnapshot& snapshot,
        const std::vector<Note>& notes,
        F0FrameRange editRange,
        const NoteGeneratorParams& params) const = 0;

    virtual F0FrameRange noteFrameRange(
        const PitchCurveSnapshot& curve,
        const std::vector<Note>& notes,
        const std::vector<std::size_t>& positions) const = 0;

    virtual ContentAutoSnapPlan planAutoSnap(
        const EditableContentSnapshot& snapshot,
        const std::optional<std::vector<std::size_t>>& targets,
        const ScaleSnapConfig& scaleSnap,
        const NoteGeneratorParams& params) const = 0;

    virtual std::optional<std::vector<Note>> planAutoTuneContentRange(
        const EditableContentSnapshot& snapshot,
        int startFrame,
        int endFrameExclusive,
        const NoteGeneratorParams& params,
        const std::optional<ScaleSnapConfig>& scaleSnap) const = 0;

    virtual std::optional<ContentNoteRangePatch> planGenerateNotesOnly(
        const EditableContentSnapshot& snapshot,
        const NoteGeneratorParams& params) const = 0;

    virtual bool setTimeGrid(ContentKey key,
                             std::shared_ptr<const TimeGridSnapshot> grid) = 0;

    // Commits the grid and pushes the action; the returned pointer is non-owning.
    virtual TimeGridEditAction* commitTimeGridEdit(
        ContentKey key,
        std::shared_ptr<const TimeGridSnapshot> before,
        std::shared_ptr<const TimeGridSnapshot> after,
        const juce::String& description) = 0;

    virtual std::optional<ContentDetectedKeyEditResult> setDetectedKey(
        ContentKey key, const DetectedKey& detectedKey) = 0;

    virtual bool applyPitchShiftState(ContentKey key,
                                      const PitchShiftEditState& state) = 0;

    // Commits the shift and pushes the action; the returned pointer is non-owning.
    virtual PitchShiftEditAction* commitPitchShiftEdit(
        ContentKey key,
        const PitchShiftSettings& newSettings) = 0;

    // AUTO regenerate 的完整内容提交、owner 入栈原生 action 与编辑完成通知。
    virtual ContentEditCommitResult commitAutoTuneContentRangeUndoable(
        ContentKey key,
        int startFrame,
        int endFrameExclusive,
        const NoteGeneratorParams& params,
        const std::optional<ScaleSnapConfig>& scaleSnap,
        const juce::String& description) = 0;

    // 仅生成音符唯一入口：generate → validate → 拓扑提交。
    // 全内容范围（0..f0Count，内部从同一 snapshot 派生）。不写 correction segments、
    // 不吸附、不请求 render、不创建 undo；owner 清 redo 并发送编辑完成通知。
    virtual bool generateNotesOnly(ContentKey key, const NoteGeneratorParams& params) = 0;
    virtual bool commitGeneratedNotesOnly(ContentKey key, ContentNoteRangePatch patch) = 0;

    // Shared note-field rules used by editor tools and Control API operation planning.
    ContentNoteValueEditPlan planNoteValueEdit(
        const std::vector<Note>& notes,
        const ContentNoteValueEditRequest& request) const;

    // TimeGrid anchor seed entry (async preheat + seed). Actual grid changes are
    // finalized by the processor owner, including redo invalidation and edit notification.
    virtual bool ensureTimeToolAnchorSeed(ContentKey key) = 0;

    // 一次替换整个 AutomationLane，只推进 notesRevision（gain 仅 playback 读
    // late apply，不推进 contentRevision、不使 RenderCache 失效），调用
    // republishPlaybackSource()；不 enqueue render。
    virtual ContentCommitSnapshot commitVolumeEnvelope(ContentKey key,
                                                       AutomationLane envelope) = 0;

    // 无渲染发布入口：按 content domain 调现有装配函数，只读最新 snapshot、
    // 发布包含最新 AutomationLane 与 TimeGrid 的不可变播放源。
    virtual void republishPlaybackSource(ContentKey key) = 0;
};

} // namespace OpenTune
