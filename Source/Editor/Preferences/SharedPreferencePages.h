#pragma once

#include "TabbedPreferencesDialog.h"
#include "../Utils/AppPreferences.h"
#include "../Utils/VocoderModelWeight.h"

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
        std::function<void()> onPreferencesChanged,
        std::function<void(bool forceCpu)> onRenderingPriorityChanged,
        std::function<void(VocoderModelWeight)> onVocoderModelWeightChanged,
        std::function<bool(F0ModelType)> onF0ModelChanged,
        std::function<void(bool)> onLightPitchCorrectionChanged,
        bool isVst3Plugin);
};

} // namespace OpenTune
