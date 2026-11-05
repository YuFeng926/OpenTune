// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TrackSupport.h"

#include "Control/ControlUiBridge.h"
#include "PluginProcessor.h"

#include <algorithm>

namespace OpenTune {

int controlTrackSlotCount(ControlOpContext& context)
{
    const auto* arrangement = context.processor().getStandaloneArrangement();
    return arrangement != nullptr ? arrangement->getNumTracks() : 0;
}

std::optional<Error> controlCheckTrackSlot(ControlOpContext& context, int trackId)
{
    if (trackId < 0 || trackId >= controlTrackSlotCount(context))
        return controlOpError("NOT_FOUND", "unknown_handle", "the trackId names no track slot");
    return std::nullopt;
}

std::vector<ControlTrackState> controlCaptureTracks(ControlOpContext& context, int visibleCount)
{
    std::vector<ControlTrackState> tracks;
    const auto* arrangement = context.processor().getStandaloneArrangement();
    if (arrangement == nullptr)
        return tracks;
    bool anySolo = false;
    for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId)
        anySolo = anySolo || arrangement->isTrackSolo(trackId);
    for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId) {
        ControlTrackState track;
        track.trackId = trackId;
        track.muted = arrangement->isTrackMuted(trackId);
        track.solo = arrangement->isTrackSolo(trackId);
        track.volumeLinear = std::max(0.0, static_cast<double>(arrangement->getTrackVolume(trackId)));
        track.colourRgb = arrangement->getTrackColour(trackId).getARGB() & 0xffffffu;
        track.visible = trackId < visibleCount;
        // As project.get reports it: a track is audible unless it is muted, or another track solos and it does not.
        track.audible = !track.muted && (track.solo || !anySolo);
        tracks.push_back(track);
    }
    return tracks;
}

Result<int> controlEditorVisibleTrackCount(ControlOpContext& context)
{
    auto* bridge = context.bridge();
    // PROTOCOL.md section 7 rule 6: without an editor window the track methods fail with no_editor_window.
    if (bridge == nullptr)
        return controlOpError("UNSUPPORTED", "no_editor_window",
                              "the number of shown tracks belongs to the editor window, and no editor exists");
    const auto count = bridge->visibleTrackCount();
    if (!count)
        return controlOpError("UNSUPPORTED", "capability_missing",
                              "the number of shown tracks belongs to the editor, and the editor cannot report it");
    return std::max(0, *count);
}

std::vector<ControlPlacementRef> controlPlacementsOnTrack(ControlOpContext& context, int trackId)
{
    auto placements = controlLivePlacements(context);
    placements.erase(std::remove_if(placements.begin(), placements.end(),
                                    [trackId](const ControlPlacementRef& ref) { return ref.trackId != trackId; }),
                     placements.end());
    return placements;
}

JsonValue controlTrackRef(int trackId)
{
    auto ref = JsonValue::object();
    ref.appendMember("trackId", JsonValue::integer(trackId));
    return ref;
}

JsonValue controlTrackLabel(int trackId)
{
    return JsonValue::string("Track " + std::to_string(trackId + 1));
}

std::string controlColourText(std::uint32_t colourRgb)
{
    static const char* const digits = "0123456789ABCDEF";
    std::string text = "#";
    for (int shift = 20; shift >= 0; shift -= 4)
        text += digits[(colourRgb >> shift) & 0xf];
    return text;
}

} // namespace OpenTune
