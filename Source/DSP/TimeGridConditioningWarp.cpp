#include "TimeGridConditioningWarp.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

std::vector<double> TimeGridConditioningWarp::sourceFrameForOutputFrames(
    const TimeGridSnapshot& grid,
    double hopSeconds,
    int outFrameBegin,
    int outFrameEnd,
    double totalSourceSeconds)
{
    std::vector<double> out;
    if (outFrameEnd <= outFrameBegin || hopSeconds <= 0.0)
        return out;

    out.resize(static_cast<size_t>(outFrameEnd - outFrameBegin));

    // Last representable fractional source frame. A frame whose mapped position
    // lands on the clip end is clamped here so the resampler never reads past the
    // analysed block.
    const double maxFrame = std::max(0.0, totalSourceSeconds / hopSeconds - 1.0);

    for (int i = 0; i < outFrameEnd - outFrameBegin; ++i)
    {
        const int frame = outFrameBegin + i;
        // Output frame centre (centre=False analysis convention).
        const double tOut = (static_cast<double>(frame) + 0.5) * hopSeconds;
        const double tSrc = grid.tauInverse(tOut);
        const double srcFrame = tSrc / hopSeconds - 0.5;
        out[static_cast<size_t>(i)] = std::clamp(srcFrame, 0.0, maxFrame);
    }
    return out;
}

bool TimeGridConditioningWarp::sourceFrameSpan(const std::vector<double>& sourceFrame,
                                               double& firstFrame,
                                               double& lastFrame)
{
    if (sourceFrame.empty())
        return false;

    firstFrame = sourceFrame.front();
    lastFrame = sourceFrame.front();
    for (double value : sourceFrame)
    {
        firstFrame = std::min(firstFrame, value);
        lastFrame = std::max(lastFrame, value);
    }
    return true;
}

std::vector<float> TimeGridConditioningWarp::resampleToOutputFrames(
    const float* source,
    int bins,
    int sourceFrames,
    const std::vector<double>& sourceFrame,
    int outFrameCount)
{
    if (source == nullptr || bins <= 0 || sourceFrames <= 0 || outFrameCount <= 0)
        return {};

    const int usable = std::min<int>(outFrameCount, static_cast<int>(sourceFrame.size()));
    std::vector<float> out(static_cast<size_t>(bins) * static_cast<size_t>(usable), 0.0f);

    const double maxIndex = static_cast<double>(sourceFrames - 1);

    for (int j = 0; j < usable; ++j)
    {
        const double pos = std::clamp(sourceFrame[static_cast<size_t>(j)], 0.0, maxIndex);
        const int i0 = static_cast<int>(std::floor(pos));
        const int i1 = std::min(i0 + 1, sourceFrames - 1);
        const float frac = static_cast<float>(pos - static_cast<double>(i0));

        const float* __restrict col0 = source + static_cast<size_t>(i0);
        const float* __restrict col1 = source + static_cast<size_t>(i1);
        float* __restrict dst = out.data() + static_cast<size_t>(j);

        for (int b = 0; b < bins; ++b)
        {
            const float a = col0[static_cast<size_t>(b) * static_cast<size_t>(sourceFrames)];
            const float c = col1[static_cast<size_t>(b) * static_cast<size_t>(sourceFrames)];
            dst[static_cast<size_t>(b) * static_cast<size_t>(usable)] = a + (c - a) * frac;
        }
    }
    return out;
}

bool TimeGridConditioningWarp::stage1BakesGrid(const TimeGridSnapshot* grid) noexcept
{
    if (grid == nullptr || grid->empty())
        return false;
    return !grid->isIdentity();
}

} // namespace OpenTune
