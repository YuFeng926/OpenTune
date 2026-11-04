// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The control API's handling of an OpenTune defect in the numbering of clips.
//
// StandaloneContentRepository::createClip() takes the id of a new clip from a counter and refuses (it returns an
// invalid key) when a clip with that id exists. Opening a project calls StandaloneContentRepository::clear(), which
// restarts the counter at 1 while the clips of the opened project keep their saved ids. Every function that makes a
// clip (importing, splitting, merging, copying, duplicating) then fails once for each id in use that the counter
// reaches, and a failed attempt still uses up the id. Nothing in OpenTune handles this; the editor's own
// split, merge, paste and duplicate fail the same way after a project was opened.
//
// OpenTune is not changed for this. A method of the control API that makes clips calls
// controlSkipUsedClipIds() with the number of clips it is about to make, before it calls the OpenTune function, and
// controlRecordCommittedClipId() for each clip that the function made. The import job, which was written first,
// repeats a commit that failed instead (ImportOps.cpp); it shares the other two functions so that both ways agree on
// which ids are behind the counter.
//
// The ids of a project come from its file as 64-bit integers and createClip() does not check them, so a clip can have
// any id, the largest value (a stored -1) included. The skip therefore looks for a place where the next ids are free;
// it does not move the counter past the highest id, which could take longer than anyone can wait.
//
// Why the other methods do not repeat a failed call: splitPlacementAtSeconds() makes two clips, and when only one
// of the two ids is in use it gives up after the other clip was made and published. Its clean-up,
// StandaloneContentRepository::releaseClip(), deletes a clip only after retireClip(), so the clip stays: a content that
// no clip shows, listed by project.get. Skipping the ids first means the call never meets an id in use.

#include "OpsRegistry.h"

#include "Content/ContentKey.h"

#include <cstddef>

namespace OpenTune {

/**
 * How many clips of the repository can still make a creation fail because of their ids: the clips whose id lies
 * above the highest id that this instance has seen the counter pass since the last StandaloneContentRepository::clear()
 * (see controlRecordCommittedClipId()). 0 without a content epoch (no project was opened, so the counter is past every
 * id), and 0 right after controlSkipUsedClipIds() and the creations that followed it were recorded. It never
 * undercounts; clips that the editor made in the meantime are counted too, which costs a pass of
 * controlSkipUsedClipIds() but is never wrong. Message thread.
 */
std::size_t controlPossibleClipIdCollisions(ControlOpContext& context);

/**
 * Records that the counter has passed the id of a clip that was just made (by an import commit or by one of
 * the OpenTune functions that controlSkipUsedClipIds() prepares), so that controlPossibleClipIdCollisions() stops
 * counting the ids up to it. Only raises the record; starts again from 0 when the content epoch changed. Message
 * thread.
 */
void controlRecordCommittedClipId(ControlOpContext& context, const ContentKey& key);

/**
 * Moves OpenTune's clip id counter to a place where the clipsToMake clips that the caller is about to make find their
 * ids free: the next clipsToMake ids from the counter are not the id of any clip the repository holds, retired clips
 * that have not been reclaimed yet included (createClip() checks all of them). Does nothing (and takes nothing from
 * the counter) when clipsToMake is 0 or controlPossibleClipIdCollisions() is 0, so a call without a project opened
 * before has no effect on OpenTune. Call it only where the method has passed its checks and is about to change state:
 * the counter is state, a dry run and a refused call leave it alone. clipsToMake is the number of clips the OpenTune
 * function makes in this call (2 for a split, 1 for a merge, one per copied clip), no fewer: a clip beyond that number
 * may still meet an id in use.
 *
 * The counter cannot be read, so it is moved the way OpenTune moves it: by createClip(). Each call takes the next id
 * from the counter; a call that meets an id in use returns an invalid key and has still used up the id. A call that
 * returns a key made an empty clip, which is retired and released again at once: the probe has no content, is not in
 * the render service, is not shown by any clip and does not reach the undo history, and releaseClip() deletes it
 * (a retired clip, unlike a new one, is deleted). The probes go on until one with the id p is free and the ids
 * p + 1 ... p + clipsToMake are free as well; the creations that follow take exactly those. The id p is recorded, so
 * that import and the other methods do not probe again for ids up to it. A clip with an id above p (a project can hold
 * any id, the largest 64-bit value included) keeps controlPossibleClipIdCollisions() above 0, and every later call
 * costs one probe more; it never costs more than the search for a free run, which is bounded by the number of clips
 * (each id in use blocks clipsToMake + 1 probes at most), not by the value of their ids.
 *
 * Message thread only, like every access to the repository. operation names the method for the log line that is
 * written when more than the first probe was needed (for example "arrangement.edit split"). Returns the number of ids
 * the counter was moved past: the probes made, 0 when nothing was done. If the bound on the probes is reached without
 * a free run (not expected; only a counter close to the largest value can do it), a log line says so and the caller
 * goes on, as it would without this function.
 */
std::size_t controlSkipUsedClipIds(ControlOpContext& context, const char* operation, std::size_t clipsToMake);

} // namespace OpenTune
