#pragma once
#include "../Utils/Note.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include <vector>
#include <memory>
#include <cstdint>

namespace OpenTune {

// 仅含可编辑字段。不含分析结果、store ID、placement、render queues、worker state、cache ownership。
struct EditableContentState
{
    std::vector<Note> notes;
    std::vector<CorrectedSegment> correctedSegments;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;
    PitchShiftSettings pitchShiftSettings;
    uint64_t notesRevision{0};
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};
    uint64_t contentRevision{0};
};

} // namespace OpenTune
