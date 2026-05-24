#pragma once

#include "../Utils/TimeGrid.h"

#include <memory>
#include <vector>

namespace OpenTune {

struct TimeGridIntent {
    double targetSourceSeconds{0.0};
    double desiredOutputSeconds{0.0};
    float confidence{0.0f};
};

struct TimeGridPatchRequest {
    std::shared_ptr<const TimeGridSnapshot> before;
    double affectedSourceStartSeconds{0.0};
    double affectedSourceEndSeconds{0.0};
    std::vector<TimeGridIntent> intents;
};

struct TimeGridPatchResult {
    bool success{false};
    std::shared_ptr<const TimeGridSnapshot> after;
    juce::String diagnostic;
};

class TimeGridPatchBuilder {
public:
    TimeGridPatchBuilder() = delete;

    static TimeGridPatchResult build(const TimeGridPatchRequest& request);
};

} // namespace OpenTune
