#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include "UIColors.h"

namespace OpenTune {

/**
 * AutoRenderOverlayComponent - 全屏遮罩组件，用于 AUTO 处理期间阻塞用户输入
 * 
 * 功能：
 * - 半透明黑色遮罩覆盖整个 PianoRoll
 * - 显示"正在渲染中"文案 + 圆形转圈动画
 * - 拦截所有鼠标和键盘输入
 */
class AutoRenderOverlayComponent : public juce::Component,
                                    private juce::Timer
{
public:
    AutoRenderOverlayComponent()
    {
        setInterceptsMouseClicks(true, true);
        setWantsKeyboardFocus(true);
        setAlwaysOnTop(true);
    }
    ~AutoRenderOverlayComponent() override = default;

    void setMessageText(const juce::String& text)
    {
        messageText_ = text;
        subText_.clear();
        repaint();
    }

    void setMessageText(const juce::String& mainText, const juce::String& subText)
    {
        messageText_ = mainText;
        subText_ = subText;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black.withAlpha(0.7f));

        auto bounds = getLocalBounds().toFloat();
        auto centerX = bounds.getCentreX();
        auto centerY = bounds.getCentreY();

        const float textHeight = 24.0f;
        const float spacing = 30.0f;
        const float spinnerSize = 60.0f;
        const float totalTextHeight = subText_.isEmpty() ? textHeight : textHeight * 2.0f + 8.0f;

        g.setColour(juce::Colours::white);
        g.setFont(UIColors::getUIFont(18.0f).boldened());

        float textStartY = centerY - totalTextHeight - spacing;
        juce::Rectangle<float> textBounds(
            centerX - 200.0f,
            textStartY,
            400.0f,
            textHeight
        );
        g.drawText(messageText_, textBounds, juce::Justification::centred, false);

        if (subText_.isNotEmpty())
        {
            g.setFont(UIColors::getUIFont(16.0f));
            juce::Rectangle<float> subTextBounds(
                centerX - 200.0f,
                textStartY + textHeight + 8.0f,
                400.0f,
                textHeight
            );
            g.drawText(subText_, subTextBounds, juce::Justification::centred, false);
        }

        const double time = juce::Time::getMillisecondCounterHiRes() * 0.001;
        const float phase = static_cast<float>(std::fmod(time * 1.5, 1.0));
        const float startAngle = phase * juce::MathConstants<float>::twoPi;
        const float endAngle = startAngle + juce::MathConstants<float>::pi * 1.5f;

        juce::Rectangle<float> spinnerBounds(
            centerX - spinnerSize / 2.0f,
            centerY + spacing / 2.0f,
            spinnerSize,
            spinnerSize
        );

        g.setColour(juce::Colours::white.withAlpha(0.2f));
        g.drawEllipse(spinnerBounds, 3.0f);

        g.setColour(UIColors::accent);
        juce::Path arc;
        arc.addCentredArc(
            spinnerBounds.getCentreX(),
            spinnerBounds.getCentreY(),
            spinnerSize / 2.0f,
            spinnerSize / 2.0f,
            0.0f,
            startAngle,
            endAngle,
            true
        );
        g.strokePath(arc, juce::PathStrokeType(3.0f));
    }

    void resized() override {}

    void setVisible(bool shouldBeVisible) override
    {
        Component::setVisible(shouldBeVisible);

        if (shouldBeVisible)
        {
            grabKeyboardFocus();
            toFront(true);
            startTimer(16);
        }
        else
        {
            stopTimer();
        }
    }

    // 拦截所有输入事件
    void mouseDown(const juce::MouseEvent& e) override { juce::ignoreUnused(e); }
    void mouseUp(const juce::MouseEvent& e) override { juce::ignoreUnused(e); }
    void mouseDrag(const juce::MouseEvent& e) override { juce::ignoreUnused(e); }
    void mouseMove(const juce::MouseEvent& e) override { juce::ignoreUnused(e); }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override 
    { 
        juce::ignoreUnused(e, wheel); 
    }
    bool keyPressed(const juce::KeyPress& key) override 
    { 
        juce::ignoreUnused(key); 
        return true; // 消费所有按键
    }

private:
    void timerCallback() override { repaint(); }

    juce::String messageText_ = juce::String::fromUTF8("正在渲染中");
    juce::String subText_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoRenderOverlayComponent)
};

} // namespace OpenTune
