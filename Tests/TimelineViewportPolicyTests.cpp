#include "../Source/Standalone/UI/TimelineViewportPolicy.h"

#include <cmath>
#include <cstdio>

using namespace OpenTune;

static int failures = 0;

static void check(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static bool nearEquals(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

// Zoom must keep the requested targetTime pinned at anchorViewportX
// after the resolution, and the resulting PPS must equal the
// requested (normalised) pixelsPerSecond.
static void testZoomKeepsTargetAtAnchor()
{
    TimelineViewportRequest request;
    request.kind = TimelineViewportRequest::Kind::Zoom;
    request.viewKind = TimelineViewportRequest::ViewKind::PianoRoll;
    request.targetTime = 37.5;
    request.currentVisibleStartSeconds = 25.0;
    request.anchorViewportX = 412.0; // non-centre anchor
    request.viewportWidth = 1000;
    request.pixelsPerSecond = 240.0;

    const TimelineViewportCamera camera = TimelineViewportPolicy::resolve(request);

    check(nearEquals(camera.pixelsPerSecond, 240.0, 1e-9),
          "resulting PPS equals the requested PPS");
    check(nearEquals(camera.visibleStartSeconds, request.targetTime - request.anchorViewportX / 240.0, 1e-9),
          "zoom positions targetTime at the anchor viewport x");

    const double anchorAfter =
        (request.targetTime - camera.visibleStartSeconds) * camera.pixelsPerSecond;
    check(nearEquals(anchorAfter, request.anchorViewportX, 1e-9),
          "targetTime remains at anchorViewportX after zoom");

    // PPS normalisation for PianoRoll: 5000 is clamped to the max of 500.
    request.pixelsPerSecond = 5000.0;
    const TimelineViewportCamera clamped = TimelineViewportPolicy::resolve(request);
    check(nearEquals(clamped.pixelsPerSecond, 500.0, 1e-9),
          "resulting PPS equals the requested normalised PPS");
}

// Playhead presentation: playing without continuousMode is an
// intention, not a result - it must not be fixed-centre.
static void testPlayheadNotFixedCentreWhenNotContinuous()
{
    const TimelinePlayheadPresentation pres = TimelineViewportPolicy::computePlayheadPresentation(
        500, 500, 1000, 0, true, false);
    check(!pres.fixedCentre,
          "playhead is not fixed-centre when playing without continuousMode");
    check(pres.anchorX == 500 && pres.visible,
          "non-centred playhead stays anchored at its time-derived x while visible");
}

int main()
{
    testZoomKeepsTargetAtAnchor();
    testPlayheadNotFixedCentreWhenNotContinuous();

    if (failures > 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stdout, "TimelineViewportPolicyTests passed\n");
    return 0;
}
