#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "UIColors.h"

namespace OpenTune {

// ============================================================================
//  OpenTuneTooltipWindow — dark-styled TooltipWindow with shortcut badge support.
//  Text format: "Label text\nShortcut" (second line = shortcut badge).
// ============================================================================
class OpenTuneTooltipWindow : public juce::TooltipWindow
{
public:
    OpenTuneTooltipWindow(juce::Component* parent = nullptr, int delayMs = 600)
        : juce::TooltipWindow(parent, delayMs)
    {
        setMillisecondsBeforeTipAppears(600);
        setLookAndFeel(&tooltipLookAndFeel_);
    }

    ~OpenTuneTooltipWindow() override
    {
        setLookAndFeel(nullptr);
    }

    // Computes the proper pixel bounds for the tip text (1 or 2 lines).
    // Padding: 10 px horizontal, 6 px vertical.
    static juce::Rectangle<int> getTooltipSize(const juce::String& tip)
    {
        juce::StringArray lines;
        lines.addTokens(tip, "\n", "");
        const juce::String label    = (lines.size() > 0) ? lines[0] : juce::String();
        const juce::String shortcut = (lines.size() > 1) ? lines[1] : juce::String();

        const auto labelFont    = UIColors::getUIFont(12.5f);
        const auto shortcutFont = UIColors::getUIFont(11.0f);

        auto measureTextWidth = [](const juce::Font& f, const juce::String& s) -> float
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText(f, s, 0.0f, 0.0f);
            return ga.getBoundingBox(0, ga.getNumGlyphs(), false).getWidth();
        };

        float maxWidth   = label.isNotEmpty() ? measureTextWidth(labelFont, label) : 0.0f;
        float totalHeight = label.isNotEmpty() ? labelFont.getHeight() : 0.0f;

        if (shortcut.isNotEmpty())
        {
            const float shortcutTextWidth = measureTextWidth(shortcutFont, shortcut);
            const float badgeWidth        = shortcutTextWidth + 16.0f;  // 8 px pad on each side
            maxWidth = juce::jmax(maxWidth, badgeWidth);

            totalHeight += shortcutFont.getHeight() + 3.0f + 8.0f;  // gap + badgePadV*2
        }

        constexpr float padH = 14.0f;
        constexpr float padV = 8.0f;
        // Extra safety margin to prevent JUCE text clipping
        constexpr float extraW = 4.0f;

        return juce::Rectangle<int>(
            0, 0,
            static_cast<int>(std::ceil(maxWidth + padH * 2.0f + extraW)),
            static_cast<int>(std::ceil(totalHeight + padV * 2.0f))
        );
    }

private:
    // ========================================================================
    //  Inner LookAndFeel — handles all tooltip rendering
    // ========================================================================
    class TooltipLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        juce::Rectangle<int> getTooltipBounds(const juce::String& tipText,
                                              juce::Point<int> screenPos,
                                              juce::Rectangle<int> parentArea) override
        {
            auto size = OpenTuneTooltipWindow::getTooltipSize(tipText);
            // Position below cursor, offset by 12 px
            return juce::Rectangle<int>(screenPos.x, screenPos.y + 12,
                                        size.getWidth(), size.getHeight())
                       .constrainedWithin(parentArea);
        }

        void drawTooltip(juce::Graphics& g, const juce::String& text,
                         int width, int height) override
        {
            juce::StringArray lines;
            lines.addTokens(text, "\n", "");
            const juce::String label    = (lines.size() > 0) ? lines[0] : juce::String();
            const juce::String shortcut = (lines.size() > 1) ? lines[1] : juce::String();

            const auto bounds        = juce::Rectangle<float>(0.0f, 0.0f,
                                            static_cast<float>(width),
                                            static_cast<float>(height));
            constexpr float cornerRadius = 6.0f;

            // ── Background (semi-transparent near-black) ──
            g.setColour(juce::Colour(0xED141419));
            g.fillRoundedRectangle(bounds, cornerRadius);

            // ── Border (1 px, subtle white) ──
            g.setColour(juce::Colour(0x40FFFFFF));
            g.drawRoundedRectangle(bounds.reduced(0.5f), cornerRadius, 1.0f);

            constexpr float padH = 14.0f;
            constexpr float padV = 8.0f;
            const auto inner = bounds.reduced(padH, padV);

            if (shortcut.isNotEmpty())
            {
                // ── Two-line layout ──

                const auto labelFont = UIColors::getUIFont(12.5f);
                g.setFont(labelFont);
                g.setColour(juce::Colour(0xFFECF0F1));  // white

                const auto labelRect = inner.withHeight(labelFont.getHeight());
                g.drawText(label, labelRect,
                           juce::Justification::centredLeft, false);

                // Shortcut badge (below the label)
                const auto shortcutFont    = UIColors::getUIFont(11.0f);
                g.setFont(shortcutFont);

                constexpr float badgePadH   = 8.0f;   // horizontal badge padding
                constexpr float badgePadV   = 4.0f;   // vertical badge padding
                auto measureWidth = [](const juce::Font& f, const juce::String& s) -> float
                {
                    juce::GlyphArrangement ga;
                    ga.addLineOfText(f, s, 0.0f, 0.0f);
                    return ga.getBoundingBox(0, ga.getNumGlyphs(), false).getWidth();
                };
                const float badgeTextWidth  = measureWidth(shortcutFont, shortcut);
                const float badgeWidth      = badgeTextWidth + badgePadH * 2.0f;
                const float badgeHeight     = shortcutFont.getHeight() + badgePadV * 2.0f;

                const auto badgeRect = juce::Rectangle<float>(
                    inner.getX(),
                    labelRect.getBottom() + 3.0f,
                    badgeWidth,
                    badgeHeight
                );

                // Badge background (subtle translucent fill)
                g.setColour(juce::Colour(0x30FFFFFF));
                g.fillRoundedRectangle(badgeRect, 3.0f);

                // Badge text (grey)
                g.setColour(juce::Colour(0xFF95A5A6));
                g.drawText(shortcut, badgeRect.reduced(badgePadH, badgePadV),
                           juce::Justification::centred, false);
            }
            else
            {
                // ── Single-line layout ──
                g.setColour(juce::Colour(0xFFECF0F1));
                g.setFont(UIColors::getUIFont(12.5f));
                g.drawText(label, inner,
                           juce::Justification::centredLeft, false);
            }
        }
    };

    TooltipLookAndFeel tooltipLookAndFeel_;
};

} // namespace OpenTune
