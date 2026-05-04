#pragma once

#include "TabbedPreferencesDialog.h"
#include "../Utils/AppPreferences.h"

namespace OpenTune {

struct SharedPreferencePages {
    static std::vector<TabbedPreferencesDialog::PageSpec> create(
        AppPreferences& appPreferences,
        std::function<void()> onPreferencesChanged);

    static std::unique_ptr<juce::Component> createRenderingPriorityComponent(
        AppPreferences& appPreferences,
        std::function<void()> onPreferencesChanged,
        std::function<void(bool forceCpu)> onRenderingPriorityChanged);
};

} // namespace OpenTune
