#pragma once

#include "TimeGrid.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace OpenTune {

struct TimeGridHandleMove {
    std::uint64_t handleId = 0;
    double outputSeconds = 0.0;
};

std::shared_ptr<const TimeGridSnapshot> planTimeGridDrag(
    const std::shared_ptr<const TimeGridSnapshot>& original,
    uint64_t draggedHandleId,
    double dragStartOutputSeconds,
    double newOutputSeconds,
    uint64_t selectedHandleId,
    const std::vector<uint64_t>& additionalSelectedIds,
    bool snapDisabled,
    bool& validationRejected);

std::shared_ptr<const TimeGridSnapshot> planTimeGridMoves(
    const std::shared_ptr<const TimeGridSnapshot>& original,
    const std::vector<TimeGridHandleMove>& moves);

enum class TimeGridInsertStatus {
    Inserted,
    InvalidTime,
    InvalidGrid,
    OutputSpacing,
    SourceSpacing,
    OutsideRange,
    ValidationRejected
};

struct TimeGridInsertResult {
    TimeGridInsertStatus status{TimeGridInsertStatus::InvalidGrid};
    std::shared_ptr<const TimeGridSnapshot> snapshot;
    uint64_t insertedHandleId{0};
};

TimeGridInsertResult planTimeGridInsert(
    const std::shared_ptr<const TimeGridSnapshot>& original,
    double clickedOutputSeconds);

enum class TimeGridDeleteStatus {
    Deleted,
    NotFound,
    Endpoint,
    ValidationRejected
};

struct TimeGridDeleteResult {
    TimeGridDeleteStatus status{TimeGridDeleteStatus::NotFound};
    std::shared_ptr<const TimeGridSnapshot> snapshot;
};

TimeGridDeleteResult planTimeGridDelete(
    const std::shared_ptr<const TimeGridSnapshot>& original,
    uint64_t selectedHandleId);

} // namespace OpenTune
