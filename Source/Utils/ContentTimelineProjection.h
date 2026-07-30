#pragma once

namespace OpenTune {

struct ContentTimelineProjection {
    double timelineStartSeconds{0.0};
    double timelineDurationSeconds{0.0};
    double contentStartSeconds{0.0};
    double contentDurationSeconds{0.0};

    bool isValid() const noexcept
    {
        return timelineDurationSeconds > 0.0 && contentDurationSeconds > 0.0;
    }

    double timelineEndSeconds() const noexcept
    {
        return timelineStartSeconds + timelineDurationSeconds;
    }

    double projectTimelineTimeToContent(double timelineSeconds) const noexcept
    {
        const double normalized = (timelineSeconds - timelineStartSeconds) / timelineDurationSeconds;
        return contentStartSeconds + normalized * contentDurationSeconds;
    }

    double projectContentTimeToTimeline(double contentSeconds) const noexcept
    {
        const double normalized = (contentSeconds - contentStartSeconds) / contentDurationSeconds;
        return timelineStartSeconds + normalized * timelineDurationSeconds;
    }
};

} // namespace OpenTune
