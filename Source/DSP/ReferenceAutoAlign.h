#pragma once

#include "ReferenceFeatures.h"
#include "TimeGridPatchBuilder.h"
#include "../Utils/Note.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../Content/ContentKey.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace OpenTune {

struct ReferenceClipProjection {
    uint64_t placementId{0};
    ContentKey contentKey;
    double timelineStartSeconds{0.0};
    double timelineEndSeconds{0.0};

    double durationSeconds() const noexcept
    {
        return timelineEndSeconds - timelineStartSeconds;
    }
};

struct ReferenceAlignmentRequest {
    ReferenceClipProjection target;
    ReferenceClipProjection reference;
    EffectiveTimeMap targetTimeMap;
    EffectiveTimeMap referenceTimeMap;
    ReferenceFeatureSet targetFeatures;
    ReferenceFeatureSet referenceFeatures;
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
        TimeGridInvalid,
        NoMutation
    };

    bool success{false};
    ContentKey targetContentKey;
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
