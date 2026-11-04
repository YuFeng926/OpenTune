#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Standalone/UI/UIColors.h"
#include "../Utils/LocalizationManager.h"

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
#if JUCE_IOS
        auto textArea = area.reduced(8.0f, 4.0f);
        g.setFont(OpenTune::UIColors::getUIFontExact(17.0f));
        g.drawText(text_, textArea.removeFromTop(textArea.getHeight() * 0.55f), juce::Justification::centred, false);
        g.setColour(juce::Colours::white.withAlpha(0.75f));
        g.setFont(OpenTune::UIColors::getUIFontExact(12.0f));
        g.drawFittedText(LOC_RAW("Playback uses the original pitch until rendering finishes."),
                         textArea.toNearestInt(), juce::Justification::centred, 2, 1.0f);
#else
        g.setFont(OpenTune::UIColors::getUIFontExact(13.0f));
        g.drawText(text_, area, juce::Justification::centred, false);
#endif
    }

    // Accessibility: readable by NVDA object navigation ("Rendering (2/5)") instead of an
    // unnamed object; hidden while empty. Not spoken by itself: that was too talkative.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        class BadgeAccessibilityHandler final : public juce::AccessibilityHandler {
        public:
            explicit BadgeAccessibilityHandler(RenderBadgeComponent& owner)
                : juce::AccessibilityHandler(owner, juce::AccessibilityRole::staticText), owner_(owner) {}

            juce::String getTitle() const override { return owner_.text_; }

            juce::AccessibleState getCurrentState() const override
            {
                auto state = juce::AccessibilityHandler::getCurrentState();
                return owner_.text_.isEmpty() ? state.withIgnored() : state;
            }

        private:
            RenderBadgeComponent& owner_;
        };

        return std::make_unique<BadgeAccessibilityHandler>(*this);
    }
#if JUCE_IOS
    static constexpr int kWidth = 280;
    static constexpr int kHeight = 58;
#else
    static constexpr int kWidth = 140;
    static constexpr int kHeight = 28;
#endif

private:
    juce::String text_;
};
