#include "TimelineViewportPolicy.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace OpenTune {

static double ppsMinFor(TimelineViewportRequest::ViewKind viewKind) noexcept
{
    switch (viewKind)
    {
    case TimelineViewportRequest::ViewKind::PianoRoll:
        return 10.0;
    case TimelineViewportRequest::ViewKind::Arrangement:
        return 10.0;
    }
    return 10.0;
}

static double ppsMaxFor(TimelineViewportRequest::ViewKind viewKind) noexcept
{
    switch (viewKind)
    {
    case TimelineViewportRequest::ViewKind::PianoRoll:
        return 500.0;
    case TimelineViewportRequest::ViewKind::Arrangement:
        return 1000.0;
    }
    return 1000.0;
}

double TimelineViewportPolicy::normalisePixelsPerSecond(
    double pps,
    TimelineViewportRequest::ViewKind viewKind) noexcept
{
    return std::clamp(pps, ppsMinFor(viewKind), ppsMaxFor(viewKind));
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
    const double pps = normalisePixelsPerSecond(request.pixelsPerSecond, request.viewKind);

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
    {
        const double visibleDuration = vw / pps;
        camera.visibleStartSeconds = clampStartSeconds(request.targetTime - visibleDuration * 0.5);
        break;
    }

    case TimelineViewportRequest::Kind::Page:
    {
        const double visibleDuration = vw / pps;
        const double currentStart = request.currentVisibleStartSeconds;
        const double currentEnd = currentStart + visibleDuration;
        double pageStart = currentStart;
        // 越界后放在运动方向进入边缘，避免下一帧重触发
        if (request.targetTime < currentStart)
            pageStart = request.targetTime - visibleDuration;
        else if (request.targetTime > currentEnd)
            pageStart = request.targetTime;
        camera.visibleStartSeconds = clampStartSeconds(pageStart);
        break;
    }

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

TimelinePlayheadPresentation TimelineViewportPolicy::computePlayheadPresentation(
    int timeDerivedX,
    int viewportCentreX,
    int viewportRight,
    int viewLeftGuardX,
    bool playing,
    bool continuousMode) noexcept
{
    TimelinePlayheadPresentation pres;

    // 居中判定：resolved camera 真正满足居中（像素舍入容差 1px）
    const bool centreSatisfied = playing
        && continuousMode
        && std::abs(timeDerivedX - viewportCentreX) <= 1;

    pres.anchorX = centreSatisfied ? viewportCentreX : timeDerivedX;
    pres.visible = pres.anchorX >= viewLeftGuardX && pres.anchorX <= viewportRight;
    pres.fixedCentre = centreSatisfied;

    return pres;
}

} // namespace OpenTune
