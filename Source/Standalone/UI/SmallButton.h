#pragma once

/**
 * 小型按钮控件定义
 * 
 * 本文件定义了两种小型按钮控件，用于时间轴、滚动模式切换等场景：
 * 1. SmallButton - 可自定义字体大小的小型按钮（用于PianoRoll）
 * 2. SmallButtonLookAndFeel - 为TextButton提供小字体样式（用于ArrangementView）
 * 
 * 抽取自 PianoRollComponent.h 和 ArrangementViewComponent.h，避免重复定义。
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include "UIColors.h"

namespace OpenTune {

/**
 * SmallButton - 小型圆角按钮
 * 
 * 用于时间轴控制区域的紧凑型按钮，支持自定义字体大小。
 * 特点：
 * - 圆角矩形外观（半径3px）
 * - 使用主题颜色（backgroundLight + textPrimary）
 * - 可通过setFontHeight()调整字体大小
 */
class SmallButton : public juce::TextButton
{
public:
    SmallButton()
    {
        setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
        setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    }

    /**
     * 设置按钮文字的字体高度
     * @param height 字体高度（像素），默认13.0f
     */
    void setFontHeight(float height)
    {
        fontHeight_ = height;
        repaint();
    }

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsMouseOver, bool shouldDrawButtonAsDown) override
    {
        juce::ignoreUnused(shouldDrawButtonAsMouseOver, shouldDrawButtonAsDown);

        auto bounds = getLocalBounds().toFloat();
        
        // 绘制圆角矩形背景
        g.setColour(findColour(juce::TextButton::buttonColourId));
        g.fillRoundedRectangle(bounds, 3.0f);

        // 绘制按钮文字
        g.setColour(findColour(juce::TextButton::textColourOffId));
        g.setFont(UIColors::getLabelFont(fontHeight_));
        g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred, true);
    }

private:
    float fontHeight_ = 13.0f;  // 默认字体高度
};

/**
 * SmallButtonLookAndFeel - 小型按钮外观
 * 
 * 为标准TextButton提供小字体样式，用于ArrangementView的工具栏按钮。
 * 字体大小固定为11.0f。
 */
class SmallButtonLookAndFeel : public juce::LookAndFeel_V4
{
public:
    juce::Font getTextButtonFont(juce::TextButton&, int) override
    {
        return UIColors::getLabelFont(11.0f);
    }
};

} // namespace OpenTune
