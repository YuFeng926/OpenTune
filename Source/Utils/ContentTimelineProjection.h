#pragma once

#include <algorithm>

namespace OpenTune {

namespace detail {

template <typename Value>
Value clampProjectionValue(Value value, Value low, Value high) noexcept
{
    return std::min(high, std::max(low, value));
}

} // namespace detail

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

    // 旧 contentEndSeconds() 被 contentDurationSeconds 替代；
    // 任何需要 "local 区间末尾" 的调用点直接用 contentDurationSeconds。

    double projectTimelineTimeToContent(double timelineSeconds) const noexcept
    {
        if (!isValid()) {
            return 0.0;
        }

        const double normalized = (timelineSeconds - timelineStartSeconds) / timelineDurationSeconds;
        return normalized * contentDurationSeconds;
    }

    double projectContentTimeToTimeline(double contentSeconds) const noexcept
    {
        if (!isValid()) {
            return timelineStartSeconds;
        }

        const double normalized = contentSeconds / contentDurationSeconds;
        return timelineStartSeconds + normalized * timelineDurationSeconds;
    }

    double clampTimelineTime(double timelineSeconds) const noexcept
    {
        if (!isValid()) {
            return timelineStartSeconds;
        }

        return detail::clampProjectionValue(timelineSeconds, timelineStartSeconds, timelineEndSeconds());
    }

    double clampContentTime(double contentSeconds) const noexcept
    {
        if (!isValid()) {
            return 0.0;
        }

        return detail::clampProjectionValue(contentSeconds, 0.0, contentDurationSeconds);
    }
};

} // namespace OpenTune
