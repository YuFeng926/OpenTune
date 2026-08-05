#pragma once

#include "ContentKey.h"
#include "../Utils/Note.h"
#include "../Utils/AutomationLane.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include "../Utils/NoteGeneratorTypes.h"
#include "../DSP/ChromaKeyDetector.h"
#include "../DSP/ReferenceFeatures.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <optional>
#include <vector>

namespace OpenTune {

class PitchShiftEditAction;
struct EditableContentSnapshot;

using ContentCommitSnapshot = std::shared_ptr<const EditableContentSnapshot>;

struct ContentEditRangeFrames {
    int startFrame{0};
    int endFrameExclusive{0};
};

struct ContentEditRangeSeconds {
    double startSeconds{0.0};
    double endSeconds{0.0};
};

struct ContentNoteRangePatch {
    ContentEditRangeSeconds affectedRange;
    std::vector<Note> afterNotesInRange;
};

class ContentEditCommands
{
public:
    virtual ~ContentEditCommands() = default;

    virtual bool replaceContentNotesForFullMutation(ContentKey key,
                                                     std::vector<Note> notes) = 0;

    // 只替换 notes、推进 notesRevision/contentRevision、发布新 snapshot、
    // 不推进 pitchRevision、不请求 render（拓扑语义）。
    virtual ContentCommitSnapshot commitNoteTopologyPatch(ContentKey key,
                                                          ContentNoteRangePatch patch) = 0;

    virtual ContentCommitSnapshot commitNotesAndSegments(
        ContentKey key,
        std::vector<Note> notes,
        std::vector<PitchCorrectionSegment> segments,
        ContentEditRangeFrames affectedRange) = 0;

    virtual bool setPitchCurve(ContentKey key,
                               std::shared_ptr<PitchCurve> curve,
                               ContentEditRangeFrames affectedRange) = 0;

    virtual bool setTimeGrid(ContentKey key,
                             std::shared_ptr<const TimeGridSnapshot> grid) = 0;

    virtual bool setDetectedKey(ContentKey key,
                                const DetectedKey& detectedKey) = 0;

    virtual bool applyPitchShiftState(ContentKey key,
                                      const PitchShiftEditState& state) = 0;

    virtual std::unique_ptr<PitchShiftEditAction> commitPitchShiftEdit(
        ContentKey key,
        const PitchShiftSettings& newSettings) = 0;

    // 普通 AUTO 唯一入口：生成并提交 startFrame..endFrameExclusive 范围音符。
    // 底层执行 LegacyNoteGenerator::generate → scaleSnap apply → validate → commit。
    // 不创建 undo、不 mark dirty——事务与 dirty 归属调用方。
    virtual bool autoTuneContentRange(
        ContentKey key,
        int startFrame,
        int endFrameExclusive,
        const NoteGeneratorParams& params,
        const std::optional<ScaleSnapConfig>& scaleSnap) = 0;

    // 一次替换整个 AutomationLane，推进
    // outputGain/content revision，调用 republishPlaybackSource()；
    // 不 enqueue render、不失效 RenderCache。
    virtual ContentCommitSnapshot commitVolumeEnvelope(ContentKey key,
                                                       AutomationLane envelope) = 0;

    // 无渲染发布入口：按 content domain 调现有装配函数，只读最新 snapshot、
    // 发布包含最新 AutomationLane 与 TimeGrid 的不可变播放源。
    virtual void republishPlaybackSource(ContentKey key) = 0;
};

} // namespace OpenTune
