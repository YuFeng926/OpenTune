#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "UIColors.h"

namespace OpenTune {

/**
 * 首次运行演示引导的提示卡片。
 *
 * 组件铺满父组件，只绘制目标高亮框和卡片，且只有卡片本身接收触摸，
 * 因此用户仍可直接操作被指向的界面（例如双击音频片段）。
 */
class DemoTourCallout : public juce::Component
{
public:
    std::function<void()> onPrimary;
    std::function<void()> onSkip;

    DemoTourCallout()
    {
        setAlwaysOnTop(true);
        styleButton(primaryButton_, true);
        styleButton(skipButton_, false);
        primaryButton_.onClick = [this] { if (onPrimary) onPrimary(); };
        skipButton_.onClick = [this] { if (onSkip) onSkip(); };
        addChildComponent(primaryButton_);
        addAndMakeVisible(skipButton_);
    }

    /** primaryText 为空时只显示“跳过”按钮（等待用户完成指向的操作）。 */
    void show(const juce::String& title, const juce::String& body,
              const juce::String& primaryText, const juce::String& skipText)
    {
        title_ = title;
        body_ = body;
        primaryButton_.setButtonText(primaryText);
        primaryButton_.setVisible(primaryText.isNotEmpty());
        skipButton_.setButtonText(skipText);
        setVisible(true);
        toFront(false);
        layoutCard();
    }

    /** 目标区域，使用父组件坐标。 */
    void setTarget(juce::Rectangle<int> target)
    {
        if (target_ == target)
            return;
        target_ = target;
        layoutCard();
    }

    void parentSizeChanged() override { layoutCard(); }

    bool hitTest(int x, int y) override { return card_.contains(x, y); }

    void paint(juce::Graphics& g) override
    {
        if (!target_.isEmpty())
        {
            g.setColour(UIColors::accent.withAlpha(0.9f));
            g.drawRoundedRectangle(target_.toFloat().expanded(4.0f), 8.0f, 2.5f);
        }

        const auto card = card_.toFloat();
        juce::Path shape;
        shape.addRoundedRectangle(card, 10.0f);
        if (!target_.isEmpty())
            shape.addPath(arrowPath(card));

        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillPath(shape, juce::AffineTransform::translation(0.0f, 3.0f));
        g.setColour(UIColors::backgroundMedium);
        g.fillPath(shape);
        g.setColour(UIColors::accent);
        g.strokePath(shape, juce::PathStrokeType(1.5f));

        auto text = card_.reduced(kPadding);
        g.setColour(UIColors::textPrimary);
        g.setFont(UIColors::getHeaderFont(18.0f));
        g.drawText(title_, text.removeFromTop(kTitleHeight), juce::Justification::centredLeft, true);
        text.removeFromTop(6);

        layoutBody(text.getWidth()).draw(g, text.removeFromTop(bodyHeight_).toFloat());
    }

    void resized() override { layoutCard(); }

private:
    static constexpr int kCardWidth = 340;
    static constexpr int kPadding = 16;
    static constexpr int kTitleHeight = 24;
    static constexpr int kButtonHeight = 36;
    static constexpr int kArrow = 12;
    static constexpr int kGap = 10;

    static void styleButton(juce::TextButton& button, bool accent)
    {
        button.setColour(juce::TextButton::buttonColourId, accent ? UIColors::accent : UIColors::backgroundLight);
        button.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        button.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
    }

    juce::TextLayout layoutBody(int width) const
    {
        juce::AttributedString text;
        text.append(body_, UIColors::getUIFont(), UIColors::textSecondary);
        text.setLineSpacing(3.0f);
        text.setWordWrap(juce::AttributedString::byWord);
        juce::TextLayout layout;
        layout.createLayout(text, static_cast<float>(juce::jmax(1, width)));
        return layout;
    }

    juce::Path arrowPath(juce::Rectangle<float> card) const
    {
        const auto t = target_.toFloat().expanded(4.0f);
        juce::Path arrow;
        if (card.getY() >= t.getBottom())
        {
            const float x = juce::jlimit(card.getX() + 20.0f, card.getRight() - 20.0f, t.getCentreX());
            arrow.addTriangle(x - kArrow, card.getY() + 1.0f, x + kArrow, card.getY() + 1.0f, x, card.getY() - kArrow);
        }
        else if (card.getBottom() <= t.getY())
        {
            const float x = juce::jlimit(card.getX() + 20.0f, card.getRight() - 20.0f, t.getCentreX());
            arrow.addTriangle(x - kArrow, card.getBottom() - 1.0f, x + kArrow, card.getBottom() - 1.0f, x, card.getBottom() + kArrow);
        }
        else if (card.getRight() <= t.getX())
        {
            const float y = juce::jlimit(card.getY() + 20.0f, card.getBottom() - 20.0f, t.getCentreY());
            arrow.addTriangle(card.getRight() - 1.0f, y - kArrow, card.getRight() - 1.0f, y + kArrow, card.getRight() + kArrow, y);
        }
        else if (card.getX() >= t.getRight())
        {
            const float y = juce::jlimit(card.getY() + 20.0f, card.getBottom() - 20.0f, t.getCentreY());
            arrow.addTriangle(card.getX() + 1.0f, y - kArrow, card.getX() + 1.0f, y + kArrow, card.getX() - kArrow, y);
        }
        return arrow;
    }

    void layoutCard()
    {
        const auto area = getLocalBounds().reduced(12);
        if (area.isEmpty())
            return;

        const int width = juce::jmin(kCardWidth, area.getWidth());
        bodyHeight_ = static_cast<int>(std::ceil(layoutBody(width - kPadding * 2).getHeight()));
        const int height = kPadding + kTitleHeight + 6 + bodyHeight_ + 12 + kButtonHeight + kPadding;
        const int offset = kArrow + kGap;

        juce::Rectangle<int> card;
        if (target_.isEmpty())
        {
            card = juce::Rectangle<int>(width, height).withCentre(area.getCentre());
        }
        else
        {
            // 依次尝试：下方、上方、左侧、右侧；都放不下时取下方并夹紧到可视区域。
            const juce::Rectangle<int> candidates[] = {
                { target_.getCentreX() - width / 2, target_.getBottom() + offset, width, height },
                { target_.getCentreX() - width / 2, target_.getY() - offset - height, width, height },
                { target_.getX() - offset - width, target_.getCentreY() - height / 2, width, height },
                { target_.getRight() + offset, target_.getCentreY() - height / 2, width, height },
            };
            card = candidates[0];
            for (const auto& candidate : candidates)
            {
                const auto constrained = candidate.constrainedWithin(area);
                if (!constrained.intersects(target_.expanded(4)))
                {
                    card = constrained;
                    break;
                }
            }
            card = card.constrainedWithin(area);
        }
        card_ = card;

        auto buttons = card_.reduced(kPadding).removeFromBottom(kButtonHeight);
        if (primaryButton_.isVisible())
        {
            const int primaryWidth = juce::jmax(96, primaryButton_.getBestWidthForHeight(kButtonHeight));
            primaryButton_.setBounds(buttons.removeFromRight(primaryWidth));
            buttons.removeFromRight(8);
        }
        const int skipWidth = juce::jmax(96, skipButton_.getBestWidthForHeight(kButtonHeight));
        skipButton_.setBounds(buttons.removeFromRight(skipWidth));
        repaint();
    }

    juce::String title_;
    juce::String body_;
    juce::Rectangle<int> target_;
    juce::Rectangle<int> card_;
    int bodyHeight_ = 0;
    juce::TextButton primaryButton_;
    juce::TextButton skipButton_;
};

} // namespace OpenTune
