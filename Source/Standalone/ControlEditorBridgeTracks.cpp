// SPDX-License-Identifier: AGPL-3.0-or-later
// Track and transport API calls enter the same complete editor commands as the UI listeners.
#include "ControlEditorBridge.h"

#include "PluginEditor.h"

namespace OpenTune {

ControlTrackCommandResult ControlEditorBridge::addTracks(int count)
{
    return editor_.addTrackSlots(count);
}

ControlTrackCommandResult ControlEditorBridge::duplicateTrack(int trackId, bool shareContent)
{
    return editor_.duplicateTrack(trackId, shareContent);
}

ControlTrackCommandResult ControlEditorBridge::deleteTrack(int trackId)
{
    return editor_.deleteTrack(trackId);
}

bool ControlEditorBridge::setTrackMix(int trackId, std::optional<bool> muted, std::optional<bool> solo,
                                      std::optional<double> volumeLinear, std::optional<std::uint32_t> colourRgb)
{
    return editor_.setTrackMix(trackId, muted, solo, volumeLinear, colourRgb);
}

bool ControlEditorBridge::transportPlay()
{
    editor_.applyTransportPlay();
    return true;
}

bool ControlEditorBridge::transportPause()
{
    editor_.applyTransportPause();
    return true;
}

bool ControlEditorBridge::transportStop()
{
    editor_.applyTransportStop();
    return true;
}

bool ControlEditorBridge::transportSeek(double positionSeconds)
{
    editor_.applyTransportSeek(positionSeconds);
    return true;
}

bool ControlEditorBridge::transportPlayFromLastStart()
{
    editor_.applyTransportPlayFromLastStart();
    return true;
}

bool ControlEditorBridge::transportPauseAtLastStart()
{
    editor_.applyTransportPauseAtLastStart();
    return true;
}

bool ControlEditorBridge::setLoopEnabled(bool enabled)
{
    editor_.applyLoopEnabled(enabled);
    return true;
}

bool ControlEditorBridge::setBpm(double bpm)
{
    editor_.applyBpm(bpm);
    return true;
}

bool ControlEditorBridge::setTimeSignature(int numerator, int denominator)
{
    editor_.applyTimeSignature(numerator, denominator);
    return true;
}

} // namespace OpenTune
