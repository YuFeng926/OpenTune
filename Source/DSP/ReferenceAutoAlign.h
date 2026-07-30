#pragma once

#include "ReferenceFeatures.h"
#include "../Utils/Note.h"
#include "../Content/ContentKey.h"

#include <cstdint>
#include <vector>

namespace OpenTune {

struct ReferenceClipProjection {
    uint64_t placementId{0};
    ContentKey contentKey;
    double timelineStartSeconds{0.0};
    double timelineEndSeconds{0.0};
    double sourceStartSeconds{0.0};
    double sourceEndSeconds{0.0};

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
        NoMutation
    };

    bool success{false};
    ContentKey targetContentKey;
    double affectedSourceStartSeconds{0.0};
    double affectedSourceEndSeconds{0.0};
    std::vector<Note> notesAfter;
    bool pitchChanged{false};
    ErrorCode error{ErrorCode::None};
    juce::String diagnostics;
};

class ReferenceAutoAlign {
public:
    ReferenceAutoAlign() = delete;

    static AlignmentPatch align(const ReferenceAlignmentRequest& request);
};

} // namespace OpenTune
