#pragma once

#include "Content/ContentEditCommands.h"
#include "Utils/PianoRollEditAction.h"

namespace OpenTune::TestSupport {

class ContentEditCommandsStub : public ContentEditCommands
{
public:
    bool replaceContentNotesForFullMutation(ContentKey, std::vector<Note>) override { return false; }
    ContentCommitSnapshot commitNoteTopologyPatch(ContentKey, ContentNoteRangePatch) override { return {}; }
    ContentCommitSnapshot commitNotesAndSegments(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>, ContentEditRangeFrames) override { return {}; }
    ContentCommitSnapshot commitNotesAndSegmentsInRanges(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>,
        const std::vector<ContentEditRangeFrames>&) override { return {}; }
    ContentCommitSnapshot commitNotesAndSegmentsWithOriginalF0(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>, ContentEditRangeFrames,
        std::vector<float>, std::optional<std::vector<F0FrameRange>>) override { return {}; }
    ContentEditCommitResult commitNotesAndSegmentsUndoable(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>, ContentEditRangeFrames,
        const juce::String&) override { return {}; }
    ContentEditCommitResult commitNotesAndSegmentsUndoableInRanges(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>,
        const std::vector<ContentEditRangeFrames>&, const juce::String&) override { return {}; }
    std::unique_ptr<PianoRollEditAction> createPianoRollEditAction(
        ContentKey, const std::vector<Note>&, const std::vector<PitchCorrectionSegment>&,
        ContentCommitSnapshot, const std::vector<ContentEditRangeFrames>&,
        const juce::String&) const override { return {}; }
    std::optional<ContentNoteCorrectionPlan> planNoteCorrection(
        const EditableContentSnapshot&, const std::vector<Note>&, F0FrameRange,
        const NoteGeneratorParams&) const override { return std::nullopt; }
    F0FrameRange noteFrameRange(
        const PitchCurveSnapshot&, const std::vector<Note>&, const std::vector<std::size_t>&) const override { return {}; }
    ContentAutoSnapPlan planAutoSnap(
        const EditableContentSnapshot&, const std::optional<std::vector<std::size_t>>&,
        const ScaleSnapConfig&, const NoteGeneratorParams&) const override { return {}; }
    std::optional<std::vector<Note>> planAutoTuneContentRange(
        const EditableContentSnapshot&, int, int, const NoteGeneratorParams&,
        const std::optional<ScaleSnapConfig>&) const override { return std::nullopt; }
    std::optional<ContentNoteRangePatch> planGenerateNotesOnly(
        const EditableContentSnapshot&, const NoteGeneratorParams&) const override { return std::nullopt; }
    bool setTimeGrid(ContentKey, std::shared_ptr<const TimeGridSnapshot>) override { return false; }
    TimeGridEditAction* commitTimeGridEdit(
        ContentKey, std::shared_ptr<const TimeGridSnapshot>, std::shared_ptr<const TimeGridSnapshot>,
        const juce::String&) override { return nullptr; }
    std::optional<ContentDetectedKeyEditResult> setDetectedKey(ContentKey, const DetectedKey&) override
    {
        return std::nullopt;
    }
    bool applyPitchShiftState(ContentKey, const PitchShiftEditState&) override { return false; }
    PitchShiftEditAction* commitPitchShiftEdit(ContentKey, const PitchShiftSettings&) override { return nullptr; }
    ContentEditCommitResult commitAutoTuneContentRangeUndoable(
        ContentKey, int, int, const NoteGeneratorParams&, const std::optional<ScaleSnapConfig>&,
        const juce::String&) override { return {}; }
    bool generateNotesOnly(ContentKey, const NoteGeneratorParams&) override { return false; }
    bool commitGeneratedNotesOnly(ContentKey, ContentNoteRangePatch) override { return false; }
    bool ensureTimeToolAnchorSeed(ContentKey) override { return false; }
    ContentCommitSnapshot commitVolumeEnvelope(ContentKey, AutomationLane) override { return {}; }
    void republishPlaybackSource(ContentKey) override {}
};

} // namespace OpenTune::TestSupport
