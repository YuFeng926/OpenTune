#include "DSP/TimeGridConditioningWarp.h"
#include "Utils/TimeGrid.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace OpenTune;

namespace {

int gFailures = 0;

void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", what);
        ++gFailures;
    }
}

void checkNear(double actual, double expected, double tol, const char* what)
{
    if (std::abs(actual - expected) > tol)
    {
        std::printf("FAIL: %s (actual=%.6f expected=%.6f tol=%.6f)\n",
                    what, actual, expected, tol);
        ++gFailures;
    }
}

TimeHandle anchor(double source, double output)
{
    TimeHandle h;
    h.source_seconds = source;
    h.output_seconds = output;
    h.kind = HandleKind::InternalOnset;
    h.confidence = Confidence::High;
    return h;
}

TimeHandle clipStart()
{
    TimeHandle h;
    h.source_seconds = 0.0;
    h.output_seconds = 0.0;
    h.kind = HandleKind::ClipStart;
    h.confidence = Confidence::Default;
    return h;
}

TimeHandle clipEnd(double total)
{
    TimeHandle h;
    h.source_seconds = total;
    h.output_seconds = total;
    h.kind = HandleKind::ClipEnd;
    h.confidence = Confidence::Default;
    return h;
}

std::shared_ptr<const TimeGridSnapshot> identityGrid(double total)
{
    return TimeGridSnapshot::makeFromHandles({clipStart(), clipEnd(total)});
}

/// Grid that compresses the first half and stretches the second half, endpoints fixed.
std::shared_ptr<const TimeGridSnapshot> warpedGrid(double total)
{
    return TimeGridSnapshot::makeFromHandles({
        clipStart(),
        anchor(total * 0.5, total * 0.25),
        clipEnd(total),
    });
}

constexpr double kHop = 512.0 / 44100.0;

void testIdentityGridMapsToItself()
{
    const double total = 4.0;
    auto grid = identityGrid(total);
    check(grid != nullptr, "identity grid constructed");
    if (grid == nullptr) return;

    const int frames = 40;
    auto mapping = TimeGridConditioningWarp::sourceFrameForOutputFrames(
        *grid, kHop, 0, frames, total);
    check(mapping.size() == static_cast<size_t>(frames), "identity mapping size");

    for (int i = 0; i < frames; ++i)
        checkNear(mapping[static_cast<size_t>(i)], static_cast<double>(i), 1e-9,
                  "identity grid maps frame to itself");
}

void testMappingFollowsTauInverseAtEveryFrameCentre()
{
    const double total = 7.0;
    auto grid = warpedGrid(total);
    check(grid != nullptr, "warped grid constructed");
    if (grid == nullptr) return;

    const int totalFrames = static_cast<int>(std::floor(total / kHop));
    auto mapping = TimeGridConditioningWarp::sourceFrameForOutputFrames(
        *grid, kHop, 0, totalFrames, total);
    check(mapping.size() == static_cast<size_t>(totalFrames), "warped mapping size");
    if (mapping.size() != static_cast<size_t>(totalFrames)) return;

    // Every output frame must read the source frame that tauInverse places at that
    // frame's centre. The frame centre is conventionally halfway into the hop.
    const double maxFrame = std::max(0.0, total / kHop - 1.0);
    bool allMatch = true;
    double worst = 0.0;
    for (int i = 0; i < totalFrames; ++i)
    {
        const double tOut = (static_cast<double>(i) + 0.5) * kHop;
        const double expected = std::clamp(grid->tauInverse(tOut) / kHop - 0.5, 0.0, maxFrame);
        const double diff = std::abs(mapping[static_cast<size_t>(i)] - expected);
        worst = std::max(worst, diff);
        allMatch = allMatch && diff <= 1e-9;
    }
    check(allMatch, "every frame follows tauInverse");
    if (!allMatch)
        std::printf("  worst deviation from tauInverse: %.9f frames\n", worst);

    // Sanity on the shape of the map: the first half is compressed (source advances
    // twice as fast as output), so frame i reads source frame 2i+0.5 before the
    // midpoint anchor.
    checkNear(mapping[0], 0.5, 1e-9, "compressed segment start reads source frame 0.5");
}

void testMappingReachesTheClipEnd()
{
    const double total = 4.0;
    auto grid = warpedGrid(total);
    if (grid == nullptr) return;

    const int frames = static_cast<int>(std::floor(total / kHop));
    auto mapping = TimeGridConditioningWarp::sourceFrameForOutputFrames(
        *grid, kHop, 0, frames, total);
    check(!mapping.empty(), "clip-end mapping not empty");
    if (mapping.empty()) return;

    // The grid's endpoint invariant keeps the last output frame inside the source span.
    check(mapping.back() <= total / kHop, "last mapping stays inside the source span");
    check(mapping.back() >= mapping.front(), "last mapping is not before the first");
}

void testMappingIsMonotonic()
{
    const double total = 6.0;
    auto grid = warpedGrid(total);
    if (grid == nullptr) return;

    const int frames = static_cast<int>(total / kHop);
    auto mapping = TimeGridConditioningWarp::sourceFrameForOutputFrames(
        *grid, kHop, 0, frames, total);
    check(mapping.size() == static_cast<size_t>(frames), "monotonic mapping size");

    bool monotonic = true;
    for (size_t i = 1; i < mapping.size(); ++i)
        monotonic = monotonic && mapping[i] >= mapping[i - 1];
    check(monotonic, "mapping is monotonically non-decreasing");
}

void testResampleReproducesSourceWhenMappingIsIdentity()
{
    const int bins = 4;
    const int frames = 8;
    std::vector<float> source(static_cast<size_t>(bins) * static_cast<size_t>(frames));
    for (int b = 0; b < bins; ++b)
        for (int f = 0; f < frames; ++f)
            source[static_cast<size_t>(b) * frames + static_cast<size_t>(f)]
                = static_cast<float>(b * 100 + f);

    std::vector<double> mapping(static_cast<size_t>(frames));
    for (int f = 0; f < frames; ++f)
        mapping[static_cast<size_t>(f)] = static_cast<double>(f);

    auto out = TimeGridConditioningWarp::resampleToOutputFrames(
        source.data(), bins, frames, mapping, frames);
    check(out.size() == source.size(), "identity resample size");

    bool same = true;
    for (size_t i = 0; i < std::min(out.size(), source.size()); ++i)
        same = same && std::abs(out[i] - source[i]) < 1e-6f;
    check(same, "identity mapping reproduces the source conditioning exactly");
}

void testResampleInterpolatesBetweenAdjacentFrames()
{
    const int bins = 1;
    const int frames = 4;
    const std::vector<float> source = {0.0f, 10.0f, 20.0f, 30.0f};

    const std::vector<double> mapping = {1.5};
    auto out = TimeGridConditioningWarp::resampleToOutputFrames(
        source.data(), bins, frames, mapping, 1);
    check(out.size() == 1, "single interpolated frame produced");
    if (out.size() == 1)
        checkNear(out[0], 15.0, 1e-5, "linear interpolation between frames");
}

void testResampleClampsInsteadOfInventingContent()
{
    const int bins = 1;
    const int frames = 4;
    const std::vector<float> source = {0.0f, 10.0f, 20.0f, 30.0f};

    const std::vector<double> mapping = {-5.0, 99.0};
    auto out = TimeGridConditioningWarp::resampleToOutputFrames(
        source.data(), bins, frames, mapping, 2);
    check(out.size() == 2, "clamped frames produced");
    if (out.size() == 2)
    {
        checkNear(out[0], 0.0, 1e-6, "below-range mapping clamps to first frame");
        checkNear(out[1], 30.0, 1e-6, "above-range mapping clamps to last frame");
    }
}

void testResampleLayoutIsPlanarPerBin()
{
    const int bins = 2;
    const int frames = 2;
    const std::vector<float> source = {1.0f, 2.0f, 3.0f, 4.0f};
    const std::vector<double> mapping = {0.0, 1.0};

    auto out = TimeGridConditioningWarp::resampleToOutputFrames(
        source.data(), bins, frames, mapping, 2);
    check(out.size() == 4, "planar output size");
    if (out.size() == 4)
    {
        checkNear(out[0], 1.0, 1e-6, "bin 0 frame 0");
        checkNear(out[1], 2.0, 1e-6, "bin 0 frame 1");
        checkNear(out[2], 3.0, 1e-6, "bin 1 frame 0");
        checkNear(out[3], 4.0, 1e-6, "bin 1 frame 1");
    }
}

void testSourceFrameSpan()
{
    const std::vector<double> mapping = {3.0, 1.0, 7.5};
    double first = 0.0, last = 0.0;
    check(TimeGridConditioningWarp::sourceFrameSpan(mapping, first, last),
          "span of non-empty mapping");
    checkNear(first, 1.0, 1e-9, "span first");
    checkNear(last, 7.5, 1e-9, "span last");

    check(!TimeGridConditioningWarp::sourceFrameSpan({}, first, last),
          "empty mapping has no span");
}

void testEmptyRequestIsEmpty()
{
    auto grid = identityGrid(4.0);
    if (grid == nullptr) return;
    check(TimeGridConditioningWarp::sourceFrameForOutputFrames(*grid, kHop, 5, 5, 4.0).empty(),
          "empty frame range yields empty mapping");
    check(TimeGridConditioningWarp::sourceFrameForOutputFrames(*grid, 0.0, 0, 10, 4.0).empty(),
          "zero hop yields empty mapping");
    const std::vector<double> mapping = {0.0};
    check(TimeGridConditioningWarp::resampleToOutputFrames(nullptr, 2, 2, mapping, 1).empty(),
          "null source yields empty output");
}

} // namespace

int main()
{
    std::printf("TimeGridConditioningWarp tests\n");
    testIdentityGridMapsToItself();
    testMappingFollowsTauInverseAtEveryFrameCentre();
    testMappingReachesTheClipEnd();
    testMappingIsMonotonic();
    testResampleReproducesSourceWhenMappingIsIdentity();
    testResampleInterpolatesBetweenAdjacentFrames();
    testResampleClampsInsteadOfInventingContent();
    testResampleLayoutIsPlanarPerBin();
    testSourceFrameSpan();
    testEmptyRequestIsEmpty();

    if (gFailures == 0)
    {
        std::printf("All TimeGridConditioningWarp tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", gFailures);
    return 1;
}
