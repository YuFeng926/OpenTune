// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsSupport.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace OpenTune {

/**
 * What the methods that work on tracks share (track.set, track.add, track.duplicate, track.delete, and the
 * clip methods that address a track). OpenTune identifies a track by its position only, "Track N", and has a
 * fixed number of track slots (OpenTuneAudioProcessor::MAX_TRACKS), of which the editor shows the first
 * `visible` ones; hidden slots may hold clips (a project that was opened keeps the number of shown tracks).
 * Message thread.
 */

/** One track slot as the protocol reports it (common.schema.json#/$defs/track). */
struct ControlTrackState
{
    int trackId = 0;
    bool muted = false;
    bool solo = false;
    /** The linear gain of the track's volume knob; 0 is silence. */
    double volumeLinear = 1.0;
    /** The colour as 0xRRGGBB. */
    std::uint32_t colourRgb = 0;
    bool visible = false;
    /** Not muted, and soloed or nobody soloes: what OpenTune plays. */
    bool audible = true;
};

/** The number of track slots, visible or not; 0 without an arrangement. */
int controlTrackSlotCount(ControlOpContext& context);

/** NOT_FOUND(unknown_handle) unless trackId names a track slot (PROTOCOL.md section 9: the handle check of a
    method that takes a trackId). nullopt when it does. */
std::optional<Error> controlCheckTrackSlot(ControlOpContext& context, int trackId);

/** Every slot in order, with visibility from the first visibleCount slots and audibility from the solo flags. */
std::vector<ControlTrackState> controlCaptureTracks(ControlOpContext& context, int visibleCount);

/**
 * The number of track slots the editor shows, for a method that changes it (track.add, track.duplicate,
 * track.delete). Without an editor the method fails with UNSUPPORTED(no_editor_window) (PROTOCOL.md section 7 rule 6);
 * with an editor that cannot tell, the number is not known and cannot be changed: UNSUPPORTED(capability_missing).
 */
Result<int> controlEditorVisibleTrackCount(ControlOpContext& context);

/** The live clips on one track, in the order of the track. */
std::vector<ControlPlacementRef> controlPlacementsOnTrack(ControlOpContext& context, int trackId);

/** {trackId} (common.schema.json#/$defs/entityRef). */
JsonValue controlTrackRef(int trackId);
/** "Track N", the label OpenTune gives a track and the protocol reports (PROTOCOL.md section 15.4 rule 6). */
JsonValue controlTrackLabel(int trackId);
/** "#RRGGBB" in upper case. */
std::string controlColourText(std::uint32_t colourRgb);

} // namespace OpenTune
