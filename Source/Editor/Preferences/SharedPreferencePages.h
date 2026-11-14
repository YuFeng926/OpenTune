#pragma once

#include "TabbedPreferencesDialog.h"
#include "Utils/AppPreferences.h"

namespace OpenTune {

struct RenderingPriorityPage {
    std::unique_ptr<juce::Component> component;
    int height = 0;
};

struct SharedPreferencePages {
    static std::vector<TabbedPreferencesDialog::PageSpec> create(
        AppPreferences& appPreferences,
        std::function<void()> onPreferencesChanged,
        bool isVst3Plugin);

    static RenderingPriorityPage createRenderingPriorityComponent(
        AppPreferences& appPreferences,
        ProcessingPreferenceRuntimeOperations runtimeOperations,
        std::function<void()> onPreferencesChanged,
        std::function<void(const juce::String&)> onF0ModelChangeFailed,
        std::function<void()> onReplayOnboarding,
        bool isVst3Plugin);
};

} // namespace OpenTune
