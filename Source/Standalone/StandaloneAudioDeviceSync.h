#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace OpenTune {

// Best-effort: moves an active ASIO device to the Windows shared render rate
// before a transition closes it, so firmware does not retain the ASIO rate.
// Returns false when the restore could not be verified. Callers continue the
// user's transition and report the failure; this is never a precondition.
// Does not overwrite the user's saved device settings.
// attemptedSampleRate receives the requested rate; an unavailable system rate leaves the current device rate.
bool restoreAsioSampleRateToSystem(juce::AudioDeviceManager& deviceManager,
                                   const juce::String& reason = {},
                                   double* attemptedSampleRate = nullptr);

} // namespace OpenTune
