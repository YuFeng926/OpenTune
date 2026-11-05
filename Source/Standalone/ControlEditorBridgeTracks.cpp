// SPDX-License-Identifier: AGPL-3.0-or-later
// The track and transport parts of the editor's side of ControlUiBridge: what the editor shows after the control API
// changed the number of tracks, the mix of tracks, the play state or the tempo (Control/Ops/TrackOps.cpp and
// Control/Ops/TransportOps.cpp change the processor and the arrangement themselves).
#include "ControlEditorBridge.h"

#include "PluginEditor.h"

#include <algorithm>

namespace OpenTune {

bool ControlEditorBridge::setVisibleTrackCount(int count)
{
    // The editor's own steps when it shows more or fewer tracks (trackAddRequested, trackDeleteRequested): the track
    // panel and the arrangement show the same count. The panel keeps 1 to MAX_TRACKS tracks. The panel's change also
    // reaches visibleTrackCountChanged(), which saves the count in the arrangement (project state) and marks the
    // project.
    count = std::clamp(count, 1, OpenTuneAudioProcessor::MAX_TRACKS);
    editor_.trackPanel_.setVisibleTrackCount(count);
    editor_.arrangementView_.setVisibleTrackCount(count);
    return true;
}

void ControlEditorBridge::onTrackMixChanged()
{
    // What trackDeleteRequested() does after the arrangement changed: the panel shows the mute, solo, volume and
    // colour of the arrangement for every track (a hidden track too, so that it is right when it is shown), and
    // the arrangement is redrawn with the colours and clips.
    auto* arrangement = editor_.processorRef_.getStandaloneArrangement();
    if (arrangement == nullptr)
        return;
    for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId) {
        editor_.trackPanel_.setTrackMuted(trackId, arrangement->isTrackMuted(trackId));
        editor_.trackPanel_.setTrackSolo(trackId, arrangement->isTrackSolo(trackId));
        editor_.trackPanel_.setTrackVolume(trackId, arrangement->getTrackVolume(trackId));
        editor_.trackPanel_.setTrackColour(trackId, arrangement->getTrackColour(trackId));
    }
    editor_.arrangementView_.requestContentRedraw();
}

void ControlEditorBridge::onTransportChanged()
{
    // The editor's playRequested(), pauseRequested(), stopRequested() and loopToggled() show the processor's state in
    // the transport bar. The playhead and the arrangement read the processor's play head state directly.
    editor_.transportBar_.setPlaying(editor_.processorRef_.isPlaying());
    editor_.transportBar_.setLoopEnabled(editor_.processorRef_.isLoopEnabled());
}

void ControlEditorBridge::onTempoChanged()
{
    // The editor's bpmChanged() and timeSignatureChanged(): the transport bar and the piano roll show the processor's
    // canonical values, and the editor's heartbeat does not set them again.
    const double bpm = editor_.processorRef_.getBpm();
    editor_.lastSyncedBpm_ = bpm;
    editor_.transportBar_.setBpm(bpm);
    editor_.pianoRoll_.setBpm(bpm);
    const int numerator = editor_.processorRef_.getTimeSigNumerator();
    const int denominator = editor_.processorRef_.getTimeSigDenominator();
    editor_.lastSyncedTimeSigNum_ = numerator;
    editor_.lastSyncedTimeSigDenom_ = denominator;
    editor_.transportBar_.setTimeSignature(numerator, denominator);
    editor_.pianoRoll_.setTimeSignature(numerator, denominator);
}

} // namespace OpenTune
