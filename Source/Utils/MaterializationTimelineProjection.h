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

struct MaterializationTimelineProjection {
    double timelineStartSeconds{0.0};
    double timelineDurationSeconds{0.0};
    double materializationDurationSeconds{0.0};      // local 起点恒 0，故只需 duration

    bool isValid() const noexcept
    {
        return timelineDurationSeconds > 0.0 && materializationDurationSeconds > 0.0;
    }

    double timelineEndSeconds() const noexcept
    {
        return timelineStartSeconds + timelineDurationSeconds;
    }

    // 旧 materializationEndSeconds() 被 materializationDurationSeconds 替代；
    // 任何需要 "local 区间末尾" 的调用点直接用 materializationDurationSeconds。

    double projectTimelineTimeToMaterialization(double timelineSeconds) const noexcept
    {
        if (!isValid()) {
            return 0.0;
        }

        const double normalized = (timelineSeconds - timelineStartSeconds) / timelineDurationSeconds;
        return normalized * materializationDurationSeconds;
    }

    double projectMaterializationTimeToTimeline(double materializationSeconds) const noexcept
    {
        if (!isValid()) {
            return timelineStartSeconds;
        }

        const double normalized = materializationSeconds / materializationDurationSeconds;
        return timelineStartSeconds + normalized * timelineDurationSeconds;
    }

    double clampTimelineTime(double timelineSeconds) const noexcept
    {
        if (!isValid()) {
            return timelineStartSeconds;
        }

        return detail::clampProjectionValue(timelineSeconds, timelineStartSeconds, timelineEndSeconds());
    }

    double clampMaterializationTime(double materializationSeconds) const noexcept
    {
        if (!isValid()) {
            return 0.0;
        }

        return detail::clampProjectionValue(materializationSeconds, 0.0, materializationDurationSeconds);
    }
};

} // namespace OpenTune
