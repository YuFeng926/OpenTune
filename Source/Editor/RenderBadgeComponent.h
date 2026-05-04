#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

class RenderBadgeComponent : public juce::Component
{
public:
    RenderBadgeComponent()
    {
        setInterceptsMouseClicks(false, false);
        setAlwaysOnTop(true);
    }

    void setMessageText(const juce::String& text)
    {
        if (text_ != text) {
            text_ = text;
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        if (text_.isEmpty()) return;

        auto area = getLocalBounds().toFloat().reduced(2.0f);
        g.setColour(juce::Colour(0xCC000000));
        g.fillRoundedRectangle(area, 6.0f);
        g.setColour(juce::Colours::white);
        g.setFont(13.0f);
        g.drawText(text_, area, juce::Justification::centred, false);
    }

private:
    juce::String text_;
};
