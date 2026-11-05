// SPDX-License-Identifier: AGPL-3.0-or-later
// Which track a colour picker is for (see ControlEditorBridgeModals.cpp).
#pragma once

#include "UI/TrackPanelComponent.h"

namespace OpenTune {

/**
 * Remembers the track of the last request for a colour picker. OpenTune opens the picker
 * (OpenTuneAudioProcessorEditor::trackColorChangeRequested) as a call-out box that holds a juce::ColourSelector and
 * nothing that names the track, and the editor keeps the track only inside the call-out's content, a class local to
 * that function. The track panel tells every listener when the user asks for the picker, so the bridge listens as well,
 * and the call-out that is open then belongs to the track of the last request. Message thread.
 */
class ControlTrackColourWatch final : public TrackPanelComponent::Listener
{
public:
    explicit ControlTrackColourWatch(TrackPanelComponent& panel) : panel_(panel) { panel_.addListener(this); }
    ~ControlTrackColourWatch() override { panel_.removeListener(this); }

    ControlTrackColourWatch(const ControlTrackColourWatch&) = delete;
    ControlTrackColourWatch& operator=(const ControlTrackColourWatch&) = delete;

    /** The track of the last request for a colour picker, or -1 when there was none. */
    int lastRequestedTrack() const noexcept { return lastTrack_; }

    void trackColorChangeRequested(int trackId) override { lastTrack_ = trackId; }

    // The rest of the listener, which the bridge does not use.
    void trackSelected(int) override {}
    void trackMuteToggled(int, bool) override {}
    void trackSoloToggled(int, bool) override {}
    void trackVolumeChanged(int, float) override {}

private:
    TrackPanelComponent& panel_;
    int lastTrack_ = -1;
};

} // namespace OpenTune
