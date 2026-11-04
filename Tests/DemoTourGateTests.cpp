#include "Utils/DemoTourGate.h"

#include <cstdio>

using OpenTune::DemoTourLoadPhase;
using OpenTune::resolveDemoTourLoad;

namespace {

int failures = 0;

void check(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void testNotStartedOpenIsNotAFailure()
{
    const auto phase = resolveDemoTourLoad({ false, false, false });
    check(phase == DemoTourLoadPhase::WaitingForOpen,
          "a loading step evaluated before the open starts must keep waiting");
}

void testOpenInProgressKeepsWaiting()
{
    const auto phase = resolveDemoTourLoad({ true, true, false });
    check(phase == DemoTourLoadPhase::WaitingForOpen,
          "an open that is still running must not cancel the tour");
}

void testFinishedEmptyOpenCanRetry()
{
    const auto phase = resolveDemoTourLoad({ true, false, false });
    check(phase == DemoTourLoadPhase::OpenFailed,
          "a finished open with no audio is a real failure and must not be consumed");
}

void testLoadedDemoStartsTheTour()
{
    const auto phase = resolveDemoTourLoad({ true, false, true });
    check(phase == DemoTourLoadPhase::TourReady,
          "audio on the timeline is the moment the tour starts and the first run is consumed");
}

void testPlacementDuringOpenStillWaits()
{
    const auto phase = resolveDemoTourLoad({ true, true, true });
    check(phase == DemoTourLoadPhase::WaitingForOpen,
          "placements observed while the open is still in progress stay on the loading step");
}

} // namespace

int main()
{
    testNotStartedOpenIsNotAFailure();
    testOpenInProgressKeepsWaiting();
    testFinishedEmptyOpenCanRetry();
    testLoadedDemoStartsTheTour();
    testPlacementDuringOpenStillWaits();

    if (failures != 0) {
        std::fprintf(stderr, "%d demo tour gate checks failed\n", failures);
        return 1;
    }

    std::printf("DemoTourGateTests passed\n");
    return 0;
}
