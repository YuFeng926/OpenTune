#pragma once

namespace OpenTune {

struct ContentTimelineProjection {
    double timelineStartSeconds{0.0};
    double timelineDurationSeconds{0.0};
    double contentDurationSeconds{0.0};      // local 起点恒 0，故只需 duration

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
        return normalized * contentDurationSeconds;
    }

    double projectContentTimeToTimeline(double contentSeconds) const noexcept
    {
        const double normalized = contentSeconds / contentDurationSeconds;
        return timelineStartSeconds + normalized * timelineDurationSeconds;
    }
};

} // namespace OpenTune
