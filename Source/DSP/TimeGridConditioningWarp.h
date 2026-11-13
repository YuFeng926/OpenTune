#pragma once

#include "../Utils/TimeGrid.h"
#include <vector>

namespace OpenTune {

/// Bakes TimeGrid mapping into vocoder conditioning to avoid WSOLA phase-join tremor
/// on short segments. Frame j uses center=False time (j + 0.5) * hopSeconds,
/// mapped via srcFrame = tauInverse(t_out) / hopSeconds - 0.5.
struct TimeGridConditioningWarp
{
    /// Maps output frames [outFrameBegin, outFrameEnd) to fractional source frames
    /// clamped to [0, totalSourceFrames - 1].
    static std::vector<double> sourceFrameForOutputFrames(
        const TimeGridSnapshot& grid,
        double hopSeconds,
        int outFrameBegin,
        int outFrameEnd,
        double totalSourceSeconds);

    /// Linearly resamples row-major [bins x sourceFrames] conditioning at sourceFrame
    /// positions into [bins x outFrameCount], clamping out-of-range indices to boundaries.
    static std::vector<float> resampleToOutputFrames(
        const float* source,
        int bins,
        int sourceFrames,
        const std::vector<double>& sourceFrame,
        int outFrameCount);

    /// Lowest / highest fractional source frame a mapping touches, for sizing the
    /// source analysis block (returns false when nothing is mapped).
    static bool sourceFrameSpan(const std::vector<double>& sourceFrame,
                                double& firstFrame, double& lastFrame);

    /// True for non-identity grids where Stage 1 bakes the TimeGrid into conditioning.
    /// Conditioning/fast-path suppression, Stage 2 skip, and playback/export reads must agree.
    static bool stage1BakesGrid(const TimeGridSnapshot* grid) noexcept;

private:
    TimeGridConditioningWarp() = delete;
};

} // namespace OpenTune
