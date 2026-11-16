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

/** One track slot as the protocol reports it. */
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

/** Returns NOT_FOUND(unknown_handle) when trackId does not name a track slot, and nullopt otherwise. */
std::optional<Error> controlCheckTrackSlot(ControlOpContext& context, int trackId);

/** Every slot in order, with visibility from the first visibleCount slots and audibility from the solo flags. */
std::vector<ControlTrackState> controlCaptureTracks(ControlOpContext& context, int visibleCount);

/**
 * The number of track slots the editor shows, for a method that changes it (track.add, track.duplicate,
 * track.delete). Without an editor, the method fails with UNSUPPORTED(no_editor_window). With an editor that cannot
 * report the count, it fails with UNSUPPORTED(capability_missing).
 */
Result<int> controlEditorVisibleTrackCount(ControlOpContext& context);

/** The live clips on one track, in the order of the track. */
std::vector<ControlPlacementRef> controlPlacementsOnTrack(ControlOpContext& context, int trackId);

/** {trackId}. */
JsonValue controlTrackRef(int trackId);
/** "Track N", the label OpenTune gives a track and the protocol reports. */
JsonValue controlTrackLabel(int trackId);
/** "#RRGGBB" in upper case. */
std::string controlColourText(std::uint32_t colourRgb);

} // namespace OpenTune
