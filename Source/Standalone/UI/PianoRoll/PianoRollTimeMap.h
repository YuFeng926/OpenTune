#pragma once

#include "Utils/ContentTimelineProjection.h"
#include "Utils/TimeGrid.h"
#include "UI/ViewMapper.h"

namespace OpenTune {

/**
 * PianoRollTimeMap - stateless single-content time conversion chain.
 *
 *   source --tauForward--> output(content) --projection--> timeline --ViewMapper--> screen x
 *
 * Notes / F0 / curves / anchors are stored in SOURCE time; TimeGrid owns
 * source <-> output; Projection owns output <-> timeline; ViewMapper owns
 * timeline <-> screen x.  The formulas are moved verbatim from the existing
 * call sites: absolute time values, sample/frame conversion, llround
 * rounding, clamps and invalid-path semantics are unchanged.
 *
 * timeGrid is bound by reference; the caller guarantees the snapshot
 * outlives the map.
 */
struct PianoRollTimeMap
{
    const ContentTimelineProjection projection;
    const TimeGridSnapshot* const timeGrid;
    const ViewMapper coords;

    PianoRollTimeMap(const ContentTimelineProjection& contentProjection,
                     const TimeGridSnapshot& grid,
                     ViewMapper mapper) noexcept
        : projection(contentProjection), timeGrid(&grid), coords(mapper) {}

    // source -> timeline: tauForward then projectContentTimeToTimeline.
    double sourceToTimeline(double sourceSeconds) const noexcept
    {
        return projection.projectContentTimeToTimeline(timeGrid->tauForward(sourceSeconds));
    }

    // timeline -> source: projectTimelineTimeToContent then tauInverse.
    double timelineToSource(double timelineSeconds) const noexcept
    {
        return timeGrid->tauInverse(projection.projectTimelineTimeToContent(timelineSeconds));
    }

    // source -> screen x: sourceToTimeline then ViewMapper::timeToX.
    int sourceToX(double sourceSeconds) const noexcept
    {
        return coords.timeToX(sourceToTimeline(sourceSeconds));
    }

    // screen x -> source: ViewMapper::xToTime then timelineToSource.
    double xToSource(int x) const noexcept
    {
        return timelineToSource(coords.xToTime(x));
    }
};

} // namespace OpenTune
