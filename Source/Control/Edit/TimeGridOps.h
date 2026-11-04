// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlOriginAction.h"

#include "Content/ContentKey.h"
#include "Utils/TimeGrid.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace OpenTune {

class OpenTuneAudioProcessor;

/**
 * The time-grid edits of the control API, computed as OpenTune's time tool computes them, without the piano roll
 * (PianoRollToolHandler and PianoRollComponent stay untouched): moving handles to new output times, and moving the
 * handles of a section by the same distance.
 *
 * A handle moves between its neighbours as the time tool lets it, by the function that the time tool calls for
 * each step of a drag (planTimeGridDrag(), Source/Utils/TimeGridEdit.h), in two passes:
 * 1. the moved handle is limited to its neighbours plus and minus the minimum spacing (kMinSpacingSec, the
 *    TimeGridSnapshot::kMinOutputSpacingSeconds the time tool keeps unless Alt is held, which the API has no
 *    equivalent of);
 * 2. when several handles move together, every inner handle is limited, from left to right, to its already
 *    limited left neighbour plus the spacing and its right neighbour minus the spacing (the time tool's group
 *    drag). When a handle has no room between its neighbours, nothing moves, as the time tool keeps the grid as it
 *    was. The test for that room before the first limit is ours (planTimeGridDrag() limits with juce::jlimit(),
 *    which asserts on a lower bound above the upper); the moves of several handles to times of their own
 *    (content.editTimeGrid moveHandles with several moves) are planned here, with the same limits, because
 *    planTimeGridDrag() moves a selection by one distance only.
 *
 * A move that was limited is applied as far as it was allowed. The new grid is validated by
 * TimeGridSnapshot::makeFromHandles(), like every grid the time tool makes; a grid it rejects is not applied.
 *
 * The upstream functions that are still mirrored (the commit and its description) are registered in
 * Source/Control/mirror-map.json (see MIRRORS.md) and listed in Source/Control/README.md (Time grid).
 *
 * Threading: the planning functions are pure; commit() runs on the message thread.
 */
class TimeGridOps
{
public:
    /** The smallest distance the time tool keeps between the output times of neighbouring handles. */
    static constexpr double kMinSpacingSec = TimeGridSnapshot::kMinOutputSpacingSeconds;

    /** One requested move: the handle and the output time it should move to. */
    struct Move
    {
        std::uint64_t handleId = 0;
        double outputSec = 0.0;
    };

    /** What an edit does to one inner handle that it names or moves. */
    struct HandleResult
    {
        std::uint64_t handleId = 0;
        /** Position among the inner handles (0 is the first after ClipStart). */
        std::size_t index = 0;
        /** The handle's fixed point in the source audio (TimeHandle::source_seconds). */
        double contentSec = 0.0;
        double beforeOutputSec = 0.0;
        double afterOutputSec = 0.0;
        /** The output time the request asked for; nullopt for a handle the request did not name that the second
            pass moved (it re-spaces neighbours that are closer than the minimum spacing). */
        std::optional<double> requestedOutputSec;

        /** The handle ends elsewhere than requested. */
        bool clamped() const noexcept;
        /** The handle's output time changes. */
        bool moved() const noexcept { return afterOutputSec != beforeOutputSec; }
    };

    /** The result of planning an edit. */
    struct Plan
    {
        std::shared_ptr<const TimeGridSnapshot> before;
        /** The grid after the edit; nullptr when no handle moves (also when a handle had no room). */
        std::shared_ptr<const TimeGridSnapshot> after;
        /** The handles the request named and the other handles that move, in grid order. */
        std::vector<HandleResult> handles;
    };

    /** The handles between ClipStart and ClipEnd, in order: the handles the API reports and edits. */
    static std::vector<TimeHandle> innerHandles(const TimeGridSnapshot& grid);
    /** A grid is seeded when it has a handle between its end points (content.getDetails timeGrid.seeded). */
    static bool isSeeded(const TimeGridSnapshot* grid);
    /** The handleId of a handle: its stable id in decimal, as content.getDetails reports it. */
    static std::string handleIdText(std::uint64_t id);
    /** The inner handle a handleId names, or nullopt (also for the end points, which the API does not report).
        Only the exact decimal form handleIdText() writes is accepted. */
    static std::optional<std::uint64_t> findInnerHandle(const TimeGridSnapshot& grid, const std::string& handleId);

    /**
     * content.shiftTiming: every inner handle whose output time lies in [startOutputSec, endOutputSec] moves by
     * deltaSec, the other handles stay. As a group drag of the time tool whose dragged handle is the one that
     * leads in the direction of the move: that handle is limited by its neighbour (first pass), every handle of
     * the section moves by the distance it was allowed, and the second pass keeps the spacing. With one handle in
     * the section the move is the time tool's drag of that handle. No handle in the section, or deltaSec 0:
     * nothing moves.
     */
    static Plan planShift(const std::shared_ptr<const TimeGridSnapshot>& grid, double startOutputSec,
                          double endOutputSec, double deltaSec);

    /**
     * content.editTimeGrid op moveHandles: each named inner handle moves to its requested output time. One move is
     * the time tool's drag of that handle (limited by its neighbours). Several moves: the first pass limits each
     * moved handle by its neighbours as they are requested to be (the requested time of a moved neighbour, the
     * current time of the others), then the second pass of the group drag. The handles must be inner handles of
     * the grid, each named once (the caller checks it).
     */
    static Plan planMoves(const std::shared_ptr<const TimeGridSnapshot>& grid, const std::vector<Move>& moves);

    /** The description of the undo entry of a handle move, the text the time tool gives its drag
        (PianoRollToolHandler: LOC_RAW("Drag time handle"), in the language of the window). Message thread. */
    static juce::String moveDescription();

    /**
     * Applies a plan with a grid after it, as the piano roll commits a time-grid edit (PianoRollComponent's
     * commitTimeGrid): a TimeGridEditAction with the grid before and after, then ContentEditCommands::setTimeGrid()
     * (OpenTuneAudioProcessor::setContentTimeGrid(), which publishes the grid for playback and queues the time-stretch
     * rebuild). Returns the action for the caller's undo entry (aspect TimeGrid), or nullopt when OpenTune did not
     * accept the grid (nothing changed then).
     */
    static std::optional<ControlOriginAction::Inner> commit(OpenTuneAudioProcessor& processor, const ContentKey& key,
                                                            const Plan& plan, const juce::String& description);
};

} // namespace OpenTune
