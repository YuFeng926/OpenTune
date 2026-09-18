#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace OpenTune {

// Restores an active ASIO device to the Windows shared render rate before a
// transition closes it. Returns false when a required restore could not be
// verified; callers should keep the current device in that case.
bool restoreAsioSampleRateToSystem(juce::AudioDeviceManager& deviceManager,
                                   const juce::String& reason = {});

} // namespace OpenTune
