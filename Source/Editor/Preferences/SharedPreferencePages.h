#pragma once

#include "TabbedPreferencesDialog.h"
#include "../Utils/AppPreferences.h"
#include "../Utils/VocoderModelWeight.h"

namespace OpenTune {

struct SharedPreferencePages {
    static std::vector<TabbedPreferencesDialog::PageSpec> create(
        AppPreferences& appPreferences,
        std::function<void()> onPreferencesChanged,
        bool isVst3Plugin);

    static std::unique_ptr<juce::Component> createRenderingPriorityComponent(
        AppPreferences& appPreferences,
        std::function<void()> onPreferencesChanged,
        std::function<void(bool forceCpu)> onRenderingPriorityChanged,
        std::function<void(VocoderModelWeight)> onVocoderModelWeightChanged,
        bool isVst3Plugin);

    // 读取 createRenderingPriorityComponent 返回组件的 preferredHeight 属性
    static int getRenderingPriorityPageHeight(const juce::Component& component);
};

} // namespace OpenTune
