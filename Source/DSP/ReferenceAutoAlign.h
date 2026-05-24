#pragma once

#include "../MaterializationStore.h"
#include "../Utils/Note.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "TimeGridPatchBuilder.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace OpenTune {

using AlignmentFeatures = MaterializationStore::DerivedAnalysis;

struct ReferenceClipProjection {
    uint64_t placementId{0};
    uint64_t materializationId{0};
    double timelineStartSeconds{0.0};
    double timelineEndSeconds{0.0};
    std::shared_ptr<const TimeGridSnapshot> timeGrid;

    double durationSeconds() const noexcept
    {
        return timelineEndSeconds - timelineStartSeconds;
    }
};

struct ReferenceAlignmentRequest {
    ReferenceClipProjection target;
    ReferenceClipProjection reference;
    AlignmentFeatures targetFeatures;
    AlignmentFeatures referenceFeatures;
    std::vector<Note> targetNotesBefore;
    std::vector<CorrectedSegment> targetSegmentsBefore;
    double overlapStartTimelineSeconds{0.0};
    double overlapEndTimelineSeconds{0.0};
};

struct AlignmentPatch {
    enum class ErrorCode : uint8_t {
        None = 0,
        InvalidRequest,
        NoOverlap,
        TargetAnalysisNotReady,
        ReferenceAnalysisNotReady,
        InsufficientFeatures,
        InsufficientNotes,
        TimeGridInvalid,
        NoMutation
    };

    bool success{false};
    uint64_t targetMaterializationId{0};
    int affectedStartFrame{0};
    int affectedEndFrame{0};
    std::vector<Note> notesAfter;
    std::vector<CorrectedSegment> correctedSegmentsAfter;
    std::vector<TimeGridIntent> timingIntents;
    bool pitchChanged{false};
    bool timingChanged{false};
    ErrorCode error{ErrorCode::None};
    juce::String diagnostics;
};

class ReferenceAutoAlign {
public:
    ReferenceAutoAlign() = delete;

    static AlignmentPatch align(const ReferenceAlignmentRequest& request);
};

} // namespace OpenTune
