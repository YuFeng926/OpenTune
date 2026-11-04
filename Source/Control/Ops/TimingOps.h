// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * The time-grid methods of capability timing.v1, on the message thread:
 * - content.shiftTiming (methods/content.shiftTiming.schema.json): moves the handles of a section (a content or an
 *   output range; a content range is mapped through the current grid) by deltaSec, as a group drag of OpenTune's
 *   time tool (Edit/TimeGridOps.h).
 * - content.editTimeGrid (methods/content.editTimeGrid.schema.json):
 *   - op moveHandles moves named handles to output times, as drags of the time tool;
 *   - op seed places the initial handles, as OpenTune does when the time tool is picked
 *     (OpenTuneAudioProcessor::ensureTimeToolAnchorSeed()). It runs as a job of kind timeGridSeed: the job asks
 *     for OpenTune's timing analysis of the content (OpenTuneAudioProcessor::preheatReferenceFeatures(),
 *     the analysis ensureTimeToolAnchorSeed() waits for) and seeds once it is ready. The job is cancellable until
 *     it seeds; a cancelled job leaves no deferred seeding behind, because it never registers the content for the
 *     processor's own deferred seeding. As in the UI there is no undo entry (reason ui_parity, no revert recipe,
 *     IRREVERSIBLE); a grid that already has handles is not seeded again (a no-op).
 *
 * Moves are limited as the time tool limits them, in two passes, by the minimum spacing to the neighbouring
 * handles; a limited move is applied as far as allowed and reported in clamped and with the warning CLAMPED. One
 * call is one undo step: a TimeGridEditAction, as the piano roll pushes it, in a ControlOriginAction (aspect
 * TimeGrid). The project is marked as changed (the UI does not mark it after a time-grid edit), rendering is
 * queued again (reRenderQueued), and the editor is told through ControlUiBridge::onTimeGridChanged(). A grid change
 * never changes the content's revision, only its gridRevision.
 *
 * Every write checks, in order: the handle, the busy state, the readiness gate (PROTOCOL.md section 11),
 * expectedGridRevision (REVISION_CONFLICT(grid_changed)), that the grid has handles (NOT_READY(grid_not_seeded)),
 * then its handles (INVALID_ARGUMENT(duplicate_handle), NOT_FOUND(unknown_handle)).
 */
void registerTimingOps(ControlMethodTable& table);

/** Test seam for the headless tests: the longest time a timeGridSeed job waits for OpenTune's timing analysis
    (120 s); 0 restores it. The limit is read again at every wait step, so it also applies to jobs that are
    already waiting. Never used by OpenTune itself. Message thread. */
void controlSetSeedWaitLimitForTesting(int milliseconds);

} // namespace OpenTune
