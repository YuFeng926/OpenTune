#pragma once

#include "../MaterializationStore.h"
#include "../Utils/Note.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"

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
    std::shared_ptr<const TimeGridSnapshot> targetTimeGridBefore;
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
        InsufficientAnchors,
        TimeGridInvalid,
        NoMutation
    };

    bool success{false};
    uint64_t targetMaterializationId{0};
    int affectedStartFrame{0};
    int affectedEndFrame{0};
    std::vector<Note> notesAfter;
    std::vector<CorrectedSegment> correctedSegmentsAfter;
    std::shared_ptr<const TimeGridSnapshot> timeGridAfter;
    bool pitchChanged{false};
    bool timeGridChanged{false};
    ErrorCode error{ErrorCode::None};
    juce::String diagnostics;
};

class ReferenceAutoAlign {
public:
    ReferenceAutoAlign() = delete;

    static AlignmentPatch align(const ReferenceAlignmentRequest& request);
};

} // namespace OpenTune
