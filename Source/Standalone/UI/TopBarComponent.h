#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "MenuBarComponent.h"
#include "TransportBarComponent.h"
#include "TouchTooltipPresenter.h"
#include "UIColors.h"
#include "../../Utils/LocalizationManager.h"

namespace OpenTune {

class TopBarComponent : public juce::Component
{
public:
    TopBarComponent(MenuBarComponent& menuBar, TransportBarComponent& transportBar);

    void paint(juce::Graphics& g) override;
    void resized() override;

    void applyTheme();

    // 外部回调：由宿主（PluginEditor）接管布局与可见性
    std::function<void()> onToggleTrackPanel;
    std::function<void()> onToggleParameterPanel;

    // 同步按钮状态（避免 ToggleState 与真实可见性不一致）
    void setSidePanelsVisible(bool trackPanelVisible, bool parameterPanelVisible);
    void setTrackPanelToggleVisible(bool visible);
    // Accessibility: false takes both toggles out of the Tab order and the screen reader tree
    // (for an editor where they have no effect); they stay visible.
    void setSidePanelTogglesAccessible(bool accessible);
    juce::Component& getParameterPanelButton() { return parameterPanelToggleButton_; }

    void refreshLocalizedText();  // 刷新本地化文本

private:
    MenuBarComponent& menuBar_;
    TransportBarComponent& transportBar_;

    // 侧边栏开关（统一图标按钮）
    UnifiedToolbarButton trackPanelToggleButton_ { LOC(kTracks), {} }; // Icon set in constructor
    UnifiedToolbarButton parameterPanelToggleButton_ { LOC(kProps), {} };
    bool trackPanelToggleVisible_{true};

    TouchTooltipPresenter touchTooltips_ { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TopBarComponent)
};

} // namespace OpenTune
