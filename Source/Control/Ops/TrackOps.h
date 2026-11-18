// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * The track methods of capability tracks.v1, on the message thread, through the arrangement and the editor's
 * bridge. The API addresses tracks by their current position ("Track N"); history uses stable runtime identities.
 * The editor shows the first few of a fixed number of slots; hidden slots may hold clips.
 * - track.set: mute, solo, volume (dB or linear, limited to the volume knob's range
 *   of 0 to 4, with the warning CLAMPED) and colour of tracks, as the track panel's controls set them
 *   (trackMuteToggled, trackSoloToggled, trackVolumeChanged; the colour is set directly, without the colour dialog).
 *   As in the UI there is no undo entry (ui_parity); the revert recipe sets the previous values of the members that
 *   changed. The project is marked as changed. Soloing changes which other tracks are audible, which the result
 *   lists for every track.
 * - track.add: shows count more track slots, as the + button does
 *   (TrackPanelComponent::showMoreTracks). The number of shown tracks is project state: since upstream 2c01847 it is
 *   saved with the project, and the panel's change marks the project
 *   (OpenTuneAudioProcessorEditor::visibleTrackCountChanged), so with the editor the result's dirty flag turns true.
 *   The + button has no undo and no request restores the previous count: no_upstream_undo, revert null and the warning
 *   IRREVERSIBLE (subject: the first track shown).
 * - track.duplicate: the steps of the track menu's Duplicate Track
 *   (trackDuplicateRequested): mute, solo, volume and colour and a copy of every clip go to the first hidden slot,
 *   which is then shown. Mode share inserts the clips with the same contents, as OpenTune does (SHARED_CONTENT);
 *   mode clone first gives every clip a content of its own (OpenTuneAudioProcessor::cloneContent(), the function the
 *   clip duplicate of the arrangement uses). A clip waiting to be reclaimed (isRetired) is not copied. No undo entry
 *   (ui_parity); the revert recipe deletes the new track and needs the end user's consent.
 * - track.delete: the steps of the track menu's Delete Track
 *   (trackDeleteRequested): OpenTuneAudioProcessor::deleteTrack() removes the track's clips and moves every
 *   later shown track down, then the panel and the arrangement show one track fewer. Hidden slots keep their
 *   position, their clips and their mix. Actions tied to the deleted track are pruned by stable identity; content
 *   actions survive while an active placement or retained history owns their content. Unreferenced content is retired
 *   for reclamation.
 *   It cannot be undone (no_upstream_undo, IRREVERSIBLE) and needs confirm; the only shown track and hidden tracks
 *   cannot be deleted. The control API reports content that left the project as removed.
 *
 * Every write checks, in order: the trackId, the busy state, the expected arrangement
 * revision, then the method's own validation. The methods that change the number of shown tracks need the editor
 * (UNSUPPORTED(no_editor_window) without one).
 */
void registerTrackOps(ControlMethodTable& table);

} // namespace OpenTune
