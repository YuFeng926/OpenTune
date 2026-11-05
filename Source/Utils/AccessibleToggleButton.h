#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ScreenReaderAnnouncer.h"

namespace OpenTune::a11y {

// Check box for screen readers. The stock JUCE ToggleButton has two problems (user test):
// its "On"/"Off" value is read-only, so NVDA says "read only" on every check box, and a
// key press raises no toggle event, so NVDA stays silent after Space. This handler has no
// value, the state comes from the toggle pattern, and a click reports the new state.
class AccessibleToggleButton : public juce::ToggleButton
{
public:
    using juce::ToggleButton::ToggleButton;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        class Handler final : public juce::AccessibilityHandler
        {
        public:
            explicit Handler(AccessibleToggleButton& owner)
                : juce::AccessibilityHandler(
                      owner, juce::AccessibilityRole::toggleButton,
                      juce::AccessibilityActions()
                          .addAction(juce::AccessibilityActionType::press, [&owner] { owner.triggerClick(); })
                          .addAction(juce::AccessibilityActionType::toggle,
                                     [&owner] { owner.setToggleState(!owner.getToggleState(), juce::sendNotification); })),
                  owner_(owner)
            {
            }

            juce::AccessibleState getCurrentState() const override
            {
                auto state = juce::AccessibilityHandler::getCurrentState().withCheckable();
                return owner_.getToggleState() ? state.withChecked() : state;
            }

            juce::String getTitle() const override
            {
                const auto title = juce::AccessibilityHandler::getTitle();
                return title.isNotEmpty() ? title : owner_.getButtonText();
            }

            juce::String getHelp() const override { return owner_.getTooltip(); }

        private:
            AccessibleToggleButton& owner_;
        };

        return std::make_unique<Handler>(*this);
    }

protected:
    void clicked() override
    {
        juce::ToggleButton::clicked();
        notifyToggleChanged(*this, getToggleState());
    }
};

} // namespace OpenTune::a11y
