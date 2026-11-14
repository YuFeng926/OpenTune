// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers export.start (capability export.v1): writes a WAV file of a clip, a track or the master mix as
 * a job of kind export under the export contract of PROTOCOL.md section 19 (methods/export.start.schema.json).
 * The job waits until every content of the export is analysed and its render has settled, records the
 * revisions, has OpenTune write a temporary file in the target directory, checks the revisions again on the
 * message thread and moves the file into place with one rename. The message-thread export busy state
 * that the editor's export shares (OpenTuneAudioProcessor::tryBeginExport) is taken in export.start after
 * every check and held from there to the end of the job.
 */
void registerExportOps(ControlMethodTable& table);

} // namespace OpenTune
