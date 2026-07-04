#pragma once

#include "TimelineViewportCamera.h"
#include <cmath>

namespace OpenTune {

struct TimelineViewportRequest
{
    enum class Kind
    {
        Cont,    // Continuous scroll — center targetTime
        Page,    // Page navigation — center targetTime
        Manual,  // Manual drag — targetTime is desired visibleStartSeconds
        Click,   // Click seeking — position targetTime at anchorViewportX
        Zoom     // Zoom at mouse
    };

    Kind kind = Kind::Manual;
    double targetTime = 0.0;          // 目标时间点（absolute timeline seconds）
    double anchorViewportX = 0.0;     // anchor 在 viewport 中的 X 像素位置
    int viewportWidth = 0;            // viewport 宽度（像素）
    double pixelsPerSecond = TimelineViewportCamera::kDefaultPixelsPerSecond;
};

struct TimelineViewportRange
{
    double absoluteStartSeconds = 0.0;
    double absoluteEndSeconds = 0.0;
    double visibleStartSeconds = 0.0;
    double visibleDuration = 0.0;     // seconds visible in viewport
    double currentPlayheadSeconds = 0.0;
    double scrollPercent = 0.0;       // 0.0-1.0 viewport start position in absolute range
    double thumbPercent = 0.0;        // 0.0-1.0 scrollbar thumb width relative to total range
    double pixelsPerSecond = 0.0;

    double absoluteStartPx() const noexcept { return static_cast<double>(std::llround(absoluteStartSeconds * pixelsPerSecond)); }
    double absoluteEndPx() const noexcept { return static_cast<double>(std::llround(absoluteEndSeconds * pixelsPerSecond)); }
    double visibleStartPx() const noexcept { return static_cast<double>(std::llround(visibleStartSeconds * pixelsPerSecond)); }
    double visibleWidthPx() const noexcept { return static_cast<double>(std::llround(visibleDuration * pixelsPerSecond)); }
};

class TimelineViewportPolicy
{
public:
    static constexpr double kMinPixelsPerSecond = 10.0;
    static constexpr double kMaxPixelsPerSecond = 1000.0;

    static double normalisePixelsPerSecond(double pps) noexcept;

    // 唯一入口：根据 request 计算 camera（含 Zoom）
    static TimelineViewportCamera resolve(const TimelineViewportRequest& request);

    // 计算 scrollbar 绝对范围（用于 scrollbar thumb 大小和位置）
    static TimelineViewportRange computeViewportRange(
        double absoluteStartSeconds,
        double absoluteEndSeconds,
        const TimelineViewportCamera& camera,
        int viewportWidth,
        double currentPlayheadSeconds);

    // 钳制 visibleStartSeconds >= 0
    static double clampStartSeconds(double startSeconds);

    // 便捷：从 camera + viewportWidth 计算 visibleEndSeconds
    static double visibleEndSeconds(const TimelineViewportCamera& camera, int viewportWidth);
};

} // namespace OpenTune
