#include "../Source/Standalone/UI/TimelineViewportPolicy.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace OpenTune;

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cout << "[FAIL] " << message << "\n";
        return false;
    }
    return true;
}

bool expectNear(double actual, double expected, double tolerance, const char* message)
{
    if (std::abs(actual - expected) > tolerance) {
        std::cout << "[FAIL] " << message
                  << " expected=" << expected << " actual=" << actual
                  << " tolerance=" << tolerance << "\n";
        return false;
    }
    return true;
}

// ============================================================================
// clampStartSeconds
// ============================================================================

bool cont_centersTargetTime()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Cont;
    req.targetTime = 10.0;
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 100.0; // viewportDuration = 10s, half = 5s

    const auto cam = TimelineViewportPolicy::resolve(req);
    return expectNear(cam.visibleStartSeconds, 5.0, 1e-9,
                      "Cont: targetTime=10s, viewport=10s -> visibleStart=5s");
}

bool cont_clampsToZero()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Cont;
    req.targetTime = 2.0;
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 100.0; // viewportDuration = 10s, half = 5s

    const auto cam = TimelineViewportPolicy::resolve(req);
    return expectNear(cam.visibleStartSeconds, 0.0, 1e-9,
                      "Cont: targetTime=2s would give visibleStart=-3s -> clamped to 0");
}

bool cont_preservesPps()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Cont;
    req.targetTime = 20.0;
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 200.0;

    const auto cam = TimelineViewportPolicy::resolve(req);
    return expectNear(cam.pixelsPerSecond, 200.0, 1e-9,
                      "Cont: pixelsPerSecond must be preserved");
}

// ============================================================================
// Manual
// ============================================================================

bool manual_usesTargetAsVisibleStart()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Manual;
    req.targetTime = 30.0;
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 100.0;

    const auto cam = TimelineViewportPolicy::resolve(req);
    return expectNear(cam.visibleStartSeconds, 30.0, 1e-9,
                      "Manual: targetTime=30s -> visibleStart=30s");
}

bool manual_clampsToZero()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Manual;
    req.targetTime = -5.0;
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 100.0;

    const auto cam = TimelineViewportPolicy::resolve(req);
    return expectNear(cam.visibleStartSeconds, 0.0, 1e-9,
                      "Manual: negative targetTime -> clamped to 0");
}

// ============================================================================
// Page
// ============================================================================

bool page_centersTargetTime()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Page;
    req.targetTime = 15.0;
    req.viewportWidth = 800;
    req.pixelsPerSecond = 100.0; // viewportDuration = 8s, half = 4s

    const auto cam = TimelineViewportPolicy::resolve(req);
    return expectNear(cam.visibleStartSeconds, 11.0, 1e-9,
                      "Page: targetTime=15s, viewport=8s -> visibleStart=11s");
}

// ============================================================================
// Click
// ============================================================================

bool click_positionsAtAnchor()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Click;
    req.targetTime = 20.0;
    req.anchorViewportX = 300.0; // 点击位置在 viewport X=300px
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 100.0; // 300px = 3s before target

    const auto cam = TimelineViewportPolicy::resolve(req);
    return expectNear(cam.visibleStartSeconds, 17.0, 1e-9,
                      "Click: targetTime=20s anchorX=300 pps=100 -> visibleStart=17s");
}

bool click_atLeftEdge()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Click;
    req.targetTime = 10.0;
    req.anchorViewportX = 0.0; // 点击在 viewport 左边缘
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 100.0;

    const auto cam = TimelineViewportPolicy::resolve(req);
    return expectNear(cam.visibleStartSeconds, 10.0, 1e-9,
                      "Click: targetTime=10s anchorX=0 -> visibleStart=10s");
}

bool click_clampsToZero()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Click;
    req.targetTime = 2.0;
    req.anchorViewportX = 500.0; // 5s before target -> visibleStart=-3s
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 100.0;

    const auto cam = TimelineViewportPolicy::resolve(req);
    return expectNear(cam.visibleStartSeconds, 0.0, 1e-9,
                      "Click: anchor would produce negative -> clamped to 0");
}

// ============================================================================
// Zoom at mouse
// ============================================================================

bool zoomAtMouse_keepsMouseTimeFixed()
{
    // 鼠标在 visibleStartSeconds + 3s 的位置（= 13.0s absolute）
    // mouseX = (13.0 - 10.0) * 100 = 300px
    const double mouseTime = 13.0;
    const double anchorViewportX = 300.0;

    // 放大到 200 pps
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Zoom;
    req.targetTime = mouseTime;
    req.anchorViewportX = anchorViewportX;
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 200.0;

    const auto newCam = TimelineViewportPolicy::resolve(req);

    // new visibleStart = mouseTime - anchorViewportX / newPps = 13.0 - 300/200 = 11.5
    return expectNear(newCam.visibleStartSeconds, 11.5, 1e-9,
                       "Zoom: zoom in at mouse -> visibleStart adjusts correctly")
        && expectNear(newCam.pixelsPerSecond, 200.0, 1e-9,
                       "Zoom: pps updated to 200");
}

bool zoomAtMouse_zoomOut()
{
    // mouseTime at center: 10.0 + 2.5 = 12.5s, mouseX = 2.5*200 = 500px
    const double mouseTime = 12.5;
    const double anchorViewportX = 500.0;

    // 缩小到 100 pps
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Zoom;
    req.targetTime = mouseTime;
    req.anchorViewportX = anchorViewportX;
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 100.0;

    const auto newCam = TimelineViewportPolicy::resolve(req);

    // new visibleStart = 12.5 - 500/100 = 7.5
    return expectNear(newCam.visibleStartSeconds, 7.5, 1e-9,
                       "Zoom: zoom out at mouse -> visibleStart adjusts correctly");
}

bool zoomAtMouse_clampsToZero()
{
    // mouseTime at left edge
    const double mouseTime = 1.0;
    // mouseX = (1.0 - 0.0) * 100 = 100px

    // 缩小到 25 pps -> newStart = 1.0 - 100/25 = -3.0
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Zoom;
    req.targetTime = mouseTime;
    req.anchorViewportX = 100.0;
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 25.0;

    const auto newCam = TimelineViewportPolicy::resolve(req);

    return expectNear(newCam.visibleStartSeconds, 0.0, 1e-9,
                       "Zoom: clamp negative visibleStart to 0");
}

// ============================================================================
// clampStartSeconds
// ============================================================================

bool clampStart_positive()
{
    return expectNear(TimelineViewportPolicy::clampStartSeconds(42.0), 42.0, 1e-9,
                      "clamp: positive unchanged");
}

bool clampStart_zero()
{
    return expectNear(TimelineViewportPolicy::clampStartSeconds(0.0), 0.0, 1e-9,
                      "clamp: zero unchanged");
}

bool clampStart_negative()
{
    return expectNear(TimelineViewportPolicy::clampStartSeconds(-10.0), 0.0, 1e-9,
                      "clamp: negative -> 0");
}

// ============================================================================
// visibleEndSeconds
// ============================================================================

bool visibleEnd_basic()
{
    TimelineViewportCamera cam;
    cam.visibleStartSeconds = 5.0;
    cam.pixelsPerSecond = 100.0;
    // viewportWidth=1000 -> visibleDuration = 10s

    return expectNear(TimelineViewportPolicy::visibleEndSeconds(cam, 1000), 15.0, 1e-9,
                      "visibleEnd: start=5 width=1000 pps=100 -> end=15");
}

bool visibleEnd_zeroViewportWidth()
{
    TimelineViewportCamera cam;
    cam.visibleStartSeconds = 5.0;
    cam.pixelsPerSecond = 100.0;

    return expectNear(TimelineViewportPolicy::visibleEndSeconds(cam, 0), 5.0, 1e-9,
                      "visibleEnd: zero viewportWidth -> end = start");
}

// ============================================================================
// computeViewportRange - scrollbar absolute range
// ============================================================================

bool viewportRange_basic()
{
    TimelineViewportCamera cam;
    cam.visibleStartSeconds = 20.0;
    cam.pixelsPerSecond = 100.0;

    const auto range = TimelineViewportPolicy::computeViewportRange(
        /*absoluteStart=*/0.0,
        /*absoluteEnd=*/100.0,
        cam,
        /*viewportWidth=*/1000, // visibleDuration = 10s
        /*playhead=*/50.0);

    // scrollPercent = (20 - 0) / 100 = 0.2
    // thumbPercent = 10 / 100 = 0.1
    return expectNear(range.scrollPercent, 0.2, 1e-9,
                      "range: scrollPercent = viewportStart / totalRange = 0.2")
        && expectNear(range.thumbPercent, 0.1, 1e-9,
                      "range: thumbPercent = visibleDuration / totalRange = 0.1")
        && expectNear(range.visibleStartSeconds, 20.0, 1e-9,
                      "range: visibleStartSeconds preserved")
        && expectNear(range.visibleDuration, 10.0, 1e-9,
                      "range: visibleDuration computed correctly")
        && expectNear(range.currentPlayheadSeconds, 50.0, 1e-9,
                      "range: playheadSeconds preserved");
}

bool viewportRange_clampedBounds()
{
    TimelineViewportCamera cam;
    cam.visibleStartSeconds = -5.0;  // 负值（已被 clampStart 处理，这里测试原始输入被钳制）
    cam.pixelsPerSecond = 100.0;

    const auto range = TimelineViewportPolicy::computeViewportRange(
        0.0, 100.0, cam, 1000, 50.0);

    // scrollPercent should clamp to 0.0 (not negative)
    return expect(range.scrollPercent >= 0.0, "range: scrollPercent clamped at 0")
        && expect(range.scrollPercent <= 1.0, "range: scrollPercent clamped at 1");
}

bool viewportRange_zeroRange()
{
    TimelineViewportCamera cam;
    cam.visibleStartSeconds = 0.0;
    cam.pixelsPerSecond = 100.0;

    const auto range = TimelineViewportPolicy::computeViewportRange(
        100.0, 100.0, cam, 1000, 50.0); // zero range

    return expectNear(range.scrollPercent, 0.0, 1e-9,
                      "range: zero total range -> scrollPercent=0")
        && expectNear(range.thumbPercent, 1.0, 1e-9,
                      "range: zero total range -> thumbPercent=1");
}

bool viewportRange_contentSmallerViewport()
{
    TimelineViewportCamera cam;
    cam.visibleStartSeconds = 0.0;
    cam.pixelsPerSecond = 10.0; // viewportDuration = 1000/10 = 100s

    const auto range = TimelineViewportPolicy::computeViewportRange(
        0.0, 50.0, cam, 1000, 30.0); // content 50s < viewport 100s

    return expectNear(range.thumbPercent, 1.0, 1e-9,
                      "range: viewport larger than content -> thumbPercent clamped to 1.0");
}

// ============================================================================
// Edge cases
// ============================================================================

bool resolve_preservesValidPps()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Manual;
    req.targetTime = 10.0;
    req.viewportWidth = 1000;
    req.pixelsPerSecond = TimelineViewportCamera::kDefaultPixelsPerSecond;

    const auto cam = TimelineViewportPolicy::resolve(req);
    return expectNear(cam.pixelsPerSecond, TimelineViewportCamera::kDefaultPixelsPerSecond, 1e-9,
                      "resolve: preserves valid pps from request");
}

bool resolve_zeroViewportWidth()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Cont;
    req.targetTime = 5.0;
    req.viewportWidth = 0;
    req.pixelsPerSecond = 100.0;

    const auto cam = TimelineViewportPolicy::resolve(req);
    // halfViewportDuration = 0, so visibleStart = clampStart(5.0) = 5.0
    return expectNear(cam.visibleStartSeconds, 5.0, 1e-9,
                      "resolve: zero viewportWidth -> visibleStart = targetTime");
}

bool zoomAtMouse_zeroOldPps()
{
    // Caller computes anchorViewportX using kDefault when oldPps is 0
    const double mouseTime = 5.0;
    const double anchorViewportX = (mouseTime - 0.0) * TimelineViewportCamera::kDefaultPixelsPerSecond;

    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Zoom;
    req.targetTime = mouseTime;
    req.anchorViewportX = anchorViewportX;
    req.viewportWidth = 1000;
    req.pixelsPerSecond = 100.0;

    const auto newCam = TimelineViewportPolicy::resolve(req);

    return expectNear(newCam.pixelsPerSecond, 100.0, 1e-9,
                       "zoomAtMouse: zero old pps -> uses kDefault for computation")
        && expect(newCam.visibleStartSeconds >= 0.0,
                  "zoomAtMouse: visibleStartSeconds >= 0");
}

// ============================================================================
// PPS Normalisation (Policy clamp)
// ============================================================================

bool resolve_normalisesPixelsPerSecond()
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Manual;
    req.targetTime = 2.0;
    req.viewportWidth = 800;

    req.pixelsPerSecond = 1.0;
    const auto minCam = TimelineViewportPolicy::resolve(req);
    if (!expectNear(minCam.pixelsPerSecond, TimelineViewportPolicy::kMinPixelsPerSecond, 1e-9,
                    "resolve: pps below minimum clamps to policy minimum"))
        return false;

    req.pixelsPerSecond = 2000.0;
    const auto maxCam = TimelineViewportPolicy::resolve(req);
    return expectNear(maxCam.pixelsPerSecond, TimelineViewportPolicy::kMaxPixelsPerSecond, 1e-9,
                      "resolve: pps above maximum clamps to policy maximum");
}

} // namespace

int main()
{
    int failed = 0;

    auto run = [&](bool (*testFn)(), const char* name) {
        try {
            if (!testFn()) {
                ++failed;
            }
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << name << " uncaught exception: " << e.what() << "\n";
            ++failed;
        }
    };

    // --- clampStartSeconds ---
    run(clampStart_positive,              "clampStart_positive");
    run(clampStart_zero,                  "clampStart_zero");
    run(clampStart_negative,              "clampStart_negative");

    // --- Cont ---
    run(cont_centersTargetTime,           "cont_centersTargetTime");
    run(cont_clampsToZero,                "cont_clampsToZero");
    run(cont_preservesPps,                "cont_preservesPps");

    // --- Manual ---
    run(manual_usesTargetAsVisibleStart,  "manual_usesTargetAsVisibleStart");
    run(manual_clampsToZero,              "manual_clampsToZero");

    // --- Page ---
    run(page_centersTargetTime,           "page_centersTargetTime");

    // --- Click ---
    run(click_positionsAtAnchor,          "click_positionsAtAnchor");
    run(click_atLeftEdge,                 "click_atLeftEdge");
    run(click_clampsToZero,               "click_clampsToZero");

    // --- Zoom ---
    run(zoomAtMouse_keepsMouseTimeFixed,  "zoomAtMouse_keepsMouseTimeFixed");
    run(zoomAtMouse_zoomOut,              "zoomAtMouse_zoomOut");
    run(zoomAtMouse_clampsToZero,         "zoomAtMouse_clampsToZero");

    // --- visibleEndSeconds ---
    run(visibleEnd_basic,                 "visibleEnd_basic");
    run(visibleEnd_zeroViewportWidth,     "visibleEnd_zeroViewportWidth");

    // --- computeViewportRange ---
    run(viewportRange_basic,              "viewportRange_basic");
    run(viewportRange_clampedBounds,      "viewportRange_clampedBounds");
    run(viewportRange_zeroRange,          "viewportRange_zeroRange");
    run(viewportRange_contentSmallerViewport, "viewportRange_contentSmallerViewport");

    // --- Edge cases ---
    run(resolve_preservesValidPps,        "resolve_preservesValidPps");
    run(resolve_zeroViewportWidth,          "resolve_zeroViewportWidth");
    run(zoomAtMouse_zeroOldPps,             "zoomAtMouse_zeroOldPps");

    // --- PPS Normalisation ---
    run(resolve_normalisesPixelsPerSecond,  "resolve_normalisesPixelsPerSecond");

    if (failed != 0) {
        std::cout << failed << " TimelineViewportPolicy tests failed.\n";
        return 1;
    }

    std::cout << "All 23 TimelineViewportPolicy tests passed.\n";
    return 0;
}
