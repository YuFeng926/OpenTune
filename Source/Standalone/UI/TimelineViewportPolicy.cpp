#include "TimelineViewportPolicy.h"
#include <algorithm>
#include <cmath>

namespace OpenTune {

double TimelineViewportPolicy::normalisePixelsPerSecond(double pps) noexcept
{
    return std::clamp(pps, kMinPixelsPerSecond, kMaxPixelsPerSecond);
}

double TimelineViewportPolicy::clampStartSeconds(double startSeconds)
{
    return std::max(0.0, startSeconds);
}

double TimelineViewportPolicy::visibleEndSeconds(const TimelineViewportCamera& camera, int viewportWidth)
{
    if (viewportWidth <= 0 || camera.pixelsPerSecond <= 0.0)
        return camera.visibleStartSeconds;
    return camera.visibleStartSeconds + viewportWidth / camera.pixelsPerSecond;
}

TimelineViewportCamera TimelineViewportPolicy::resolve(const TimelineViewportRequest& request)
{
    const double pps = normalisePixelsPerSecond(request.pixelsPerSecond);

    const int vw = request.viewportWidth;

    TimelineViewportCamera camera;
    camera.pixelsPerSecond = pps;

    switch (request.kind)
    {
    case TimelineViewportRequest::Kind::Manual:
        // Manual drag: targetTime IS the desired visibleStartSeconds
        camera.visibleStartSeconds = clampStartSeconds(request.targetTime);
        break;

    case TimelineViewportRequest::Kind::Cont:
    case TimelineViewportRequest::Kind::Page:
        // Cont / Page: center targetTime in viewport
        {
            const double halfViewportDuration = (vw > 0) ? (vw / pps) / 2.0 : 0.0;
            camera.visibleStartSeconds = clampStartSeconds(request.targetTime - halfViewportDuration);
        }
        break;

    case TimelineViewportRequest::Kind::Click:
    case TimelineViewportRequest::Kind::Zoom:
        // Click / Zoom: position targetTime at anchorViewportX
        {
            const double anchorSeconds = (vw > 0) ? request.anchorViewportX / pps : 0.0;
            camera.visibleStartSeconds = clampStartSeconds(request.targetTime - anchorSeconds);
        }
        break;
    }

    return camera;
}

TimelineViewportRange TimelineViewportPolicy::computeViewportRange(
    double absoluteStartSeconds,
    double absoluteEndSeconds,
    const TimelineViewportCamera& camera,
    int viewportWidth,
    double currentPlayheadSeconds)
{
    TimelineViewportRange range;
    range.pixelsPerSecond = camera.pixelsPerSecond;
    range.absoluteStartSeconds = absoluteStartSeconds;
    range.absoluteEndSeconds = absoluteEndSeconds;
    range.visibleStartSeconds = camera.visibleStartSeconds;
    range.currentPlayheadSeconds = currentPlayheadSeconds;

    const double rangeDuration = absoluteEndSeconds - absoluteStartSeconds;
    const double visDuration = (camera.pixelsPerSecond > 0.0 && viewportWidth > 0)
        ? viewportWidth / camera.pixelsPerSecond
        : 0.0;
    range.visibleDuration = visDuration;

    if (rangeDuration > 0.0)
    {
        // scrollPercent: where the viewport start is in the absolute range [0..1]
        const double rawPercent = (camera.visibleStartSeconds - absoluteStartSeconds) / rangeDuration;
        range.scrollPercent = std::max(0.0, std::min(1.0, rawPercent));

        // thumbPercent: viewport width relative to total range
        const double rawThumb = visDuration / rangeDuration;
        range.thumbPercent = std::max(0.0, std::min(1.0, rawThumb));
    }
    else
    {
        range.scrollPercent = 0.0;
        range.thumbPercent = 1.0;
    }

    return range;
}

} // namespace OpenTune
