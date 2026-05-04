#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <utility>
#include <vector>

#include "Utils/LocalizationManager.h"
#include "Standalone/UI/UIColors.h"

namespace OpenTune {

class TabbedPreferencesDialog : public juce::Component
{
public:
    struct PageSpec {
        juce::String title;
        std::unique_ptr<juce::Component> content;
    };

    explicit TabbedPreferencesDialog(std::vector<PageSpec> pages)
    {
        addAndMakeVisible(tabbedComponent_);
        tabbedComponent_.setColour(juce::TabbedComponent::backgroundColourId, UIColors::backgroundDark);
        tabbedComponent_.setColour(juce::TabbedButtonBar::tabOutlineColourId, UIColors::panelBorder);
        tabbedComponent_.setTabBarDepth(32);

        for (auto& page : pages) {
            jassert(page.content != nullptr);
            if (page.content == nullptr) {
                continue;
            }

            tabbedComponent_.addTab(page.title, UIColors::backgroundDark, page.content.release(), true);
        }

        closeButton_.setButtonText(LOC(kClose));
        closeButton_.setColour(juce::TextButton::buttonColourId, UIColors::accent);
        closeButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        closeButton_.onClick = [this] {
            if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) {
                window->exitModalState(0);
            }
        };
        addAndMakeVisible(closeButton_);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(16);
        tabbedComponent_.setBounds(bounds.removeFromTop(bounds.getHeight() - 40));
        closeButton_.setBounds(bounds.removeFromRight(96).withTrimmedTop(6));
    }

private:
    juce::TabbedComponent tabbedComponent_{juce::TabbedButtonBar::TabsAtTop};
    juce::TextButton closeButton_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TabbedPreferencesDialog)
};

} // namespace OpenTune
