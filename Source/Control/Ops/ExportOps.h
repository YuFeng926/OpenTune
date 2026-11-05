// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers export.start (capability export.v1): writes a WAV file of a clip, a track or the master mix as
 * a job of kind export under the export contract of PROTOCOL.md section 19 (methods/export.start.schema.json).
 * The job waits until every content of the export is analysed and its render has settled, records the
 * revisions, has OpenTune write a temporary file in the target directory, checks the revisions again on the
 * message thread and moves the file into place with one rename. The export lock that the editor's export
 * shares (OpenTuneAudioProcessor::tryAcquireExportLock) is held from export.start to the end of the job.
 */
void registerExportOps(ControlMethodTable& table);

/** How many times an export job asks for the time-stretch rebuild of one content at the same content and time-grid
    revisions before it gives up with NOT_READY (a rebuild that OpenTune's render worker dropped is asked for again). */
inline constexpr int kControlExportMaxStretchRequests = 3;

/** How long an export job must see the pending render blocks of a content out of reach of OpenTune's render worker
    before it takes them for blocks that will not be rendered; a block between two states is never seen so this long.
    Here so that the tests can wait past it. */
inline constexpr int kControlExportStuckBlocksMs = 500;

/** Test seam for the headless tests: the longest time an export job waits for analysis and rendering
    (section 19 rule 7, 120 s), for the jobs started later; 0 restores it. Never used by OpenTune itself.
    Message thread. */
void controlSetExportWaitLimitForTesting(int milliseconds);

} // namespace OpenTune
