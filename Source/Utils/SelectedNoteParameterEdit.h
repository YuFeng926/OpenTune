#pragma once

#include "AudioEditingScheme.h"
#include "Content/ContentEditCommands.h"
#include "Content/ContentKey.h"
#include "F0Timeline.h"
#include "Note.h"
#include "PitchCurve.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace OpenTune {

struct EditableContentSnapshot;

struct SelectedNoteParameterPatch
{
    ContentKey contentKey;
    F0FrameRange affectedRange;
    std::vector<Note> beforeNotes;
    std::vector<Note> afterNotes;
    std::vector<PitchCorrectionSegment> beforeSegments;
    std::vector<PitchCorrectionSegment> afterSegments;
    bool changed = false;
};

struct SelectedNoteParameterEditResult
{
    AudioEditingScheme::ParameterEditStatus status = AudioEditingScheme::ParameterEditStatus::NoTarget;
    bool changed = false;
    F0FrameRange affectedRange;
    std::shared_ptr<const EditableContentSnapshot> contentSnapshot;
};

std::optional<SelectedNoteParameterPatch> planSelectedNoteParameterEdit(
    const ContentEditCommands& commands,
    ContentKey contentKey,
    const EditableContentSnapshot& snapshot,
    const std::vector<int>& selectedNoteIndices,
    uint64_t expectedNotesRevision,
    AudioEditingScheme::ParameterId parameterId,
    float value,
    float retuneSpeedDefault,
    float vibratoDepthDefault,
    float vibratoRateDefault);

} // namespace OpenTune
