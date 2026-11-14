#pragma once

#include <functional>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#include "Utils/AppPreferences.h"
#include "Editor/Preferences/TabbedPreferencesDialog.h"

namespace OpenTune {

struct StandalonePreferencePages {
    static std::vector<TabbedPreferencesDialog::PageSpec> createAudioPages(
        juce::AudioDeviceManager* audioDeviceManager,
        AppPreferences& appPreferences,
        ProcessingPreferenceRuntimeOperations runtimeOperations,
        std::function<void()> onPreferencesChanged,
        std::function<void(const juce::String&)> onF0ModelChangeFailed,
        std::function<void()> onReplayOnboarding);
    static std::vector<TabbedPreferencesDialog::PageSpec> createStandaloneOnlyPages(AppPreferences& appPreferences,
                                                                                    std::function<void()> onPreferencesChanged);
};

} // namespace OpenTune
