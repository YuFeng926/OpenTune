#include "DarkBlueGreyLookAndFeel.h"
#include "DarkBlueGreyTheme.h"
#include "UIColors.h"

namespace OpenTune {

void DarkBlueGreyLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                               float sliderPosProportional, float rotaryStartAngle,
                                               float rotaryEndAngle, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height)).reduced(2.0f);
    auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    auto center = bounds.getCentre();
    auto trackRadius = radius * 0.85f;

    // 1. 阴影 - 柔和扩散
    juce::DropShadow ds;
    ds.radius = 12;
    ds.offset = { 0, 4 };
    ds.colour = juce::Colour(DarkBlueGrey::Colors::BevelDark).withAlpha(DarkBlueGrey::Style::ShadowAlpha);
    
    juce::Path shadowPath;
    shadowPath.addEllipse(center.x - trackRadius, center.y - trackRadius, trackRadius * 2, trackRadius * 2);
    ds.drawForPath(g, shadowPath);

    // 2. 旋钮主体 - 深色
    g.setColour(juce::Colour(DarkBlueGrey::Colors::KnobBody));
    g.fillEllipse(center.x - trackRadius, center.y - trackRadius, trackRadius * 2, trackRadius * 2);

    // 3. 轨道背景 - 非常微弱
    float currentAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    
    juce::Path bgArc;
    bgArc.addCentredArc(center.x, center.y, trackRadius * 0.85f, trackRadius * 0.85f, 
                        0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(juce::Colour(DarkBlueGrey::Colors::TextPrimary).withAlpha(0.08f));
    g.strokePath(bgArc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // 4. 值弧线
    juce::Path valueArc;
    valueArc.addCentredArc(center.x, center.y, trackRadius * 0.85f, trackRadius * 0.85f, 
                           0.0f, rotaryStartAngle, currentAngle, true);
    
    // 悬停时外发光
    if (slider.isMouseOverOrDragging())
    {
        g.setColour(juce::Colour(DarkBlueGrey::Colors::PrimaryBlue).withAlpha(0.25f));
        g.strokePath(valueArc, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // 主弧线
    g.setColour(juce::Colour(DarkBlueGrey::Colors::KnobIndicator));
    g.strokePath(valueArc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // 5. 中心数值文本
    g.setColour(juce::Colour(DarkBlueGrey::Colors::KnobIndicator));
    g.setFont(UIColors::getLabelFont(12.0f));
    
    juce::String text;
    if (slider.getValue() < 10.0) 
        text = juce::String(slider.getValue(), 1);
    else 
        text = juce::String((int)slider.getValue());
    
    g.drawText(text, bounds, juce::Justification::centred, false);
}

void DarkBlueGreyLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                               float sliderPos, float minSliderPos, float maxSliderPos,
                                               const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height));
    
    if (style == juce::Slider::LinearVertical)
    {
        float trackW = 4.0f;
        auto trackRect = bounds.withWidth(trackW).withX(bounds.getCentreX() - trackW / 2.0f).reduced(0.0f, 4.0f);
        
        // 轨道背景
        g.setColour(juce::Colour(DarkBlueGrey::Colors::KnobBody).withAlpha(0.3f));
        g.fillRoundedRectangle(trackRect, trackW/2);
        
        // 激活填充
        auto fillRect = trackRect.withTop(sliderPos);
        g.setColour(juce::Colour(DarkBlueGrey::Colors::PrimaryBlue));
        g.fillRoundedRectangle(fillRect, trackW/2);
        
        // 滑块
        float thumbW = 24.0f;
        float thumbH = 12.0f;
        auto thumbRect = juce::Rectangle<float>(bounds.getCentreX() - thumbW/2, sliderPos - thumbH/2, thumbW, thumbH);
        
        // 滑块阴影
        juce::DropShadow thumbShadow;
        thumbShadow.radius = 6;
        thumbShadow.offset = { 0, 2 };
        thumbShadow.colour = juce::Colour(DarkBlueGrey::Colors::BevelDark).withAlpha(0.25f);
        juce::Path thumbPath;
        thumbPath.addRoundedRectangle(thumbRect, thumbH/2);
        thumbShadow.drawForPath(g, thumbPath);
        
        g.setColour(juce::Colour(DarkBlueGrey::Colors::TextPrimary));
        g.fillRoundedRectangle(thumbRect, thumbH/2);
        
        if (slider.isMouseOverOrDragging())
        {
            g.setColour(juce::Colour(DarkBlueGrey::Colors::PrimaryBlue));
            g.drawRoundedRectangle(thumbRect, thumbH/2, 1.0f);
        }
    }
    else
    {
        // 水平滑块使用默认实现
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
    }
}

void DarkBlueGreyLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                   const juce::Colour& backgroundColour,
                                                   bool shouldDrawButtonAsHighlighted,
                                                   bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
    float radius = DarkBlueGrey::Style::ControlRadius;

    bool isActive = button.getToggleState() || shouldDrawButtonAsDown;
    
    if (isActive)
    {
        // 激活状态：蓝色背景 + 白色文字
        juce::DropShadow ds;
        ds.radius = 8;
        ds.offset = { 0, 3 };
        ds.colour = juce::Colour(DarkBlueGrey::Colors::PrimaryBlue).withAlpha(0.3f);
        juce::Path p;
        p.addRoundedRectangle(bounds, radius);
        ds.drawForPath(g, p);
        
        g.setColour(juce::Colour(DarkBlueGrey::Colors::PrimaryBlue));
        g.fillRoundedRectangle(bounds, radius);
        
        if (shouldDrawButtonAsDown)
        {
            g.setColour(juce::Colours::black.withAlpha(0.15f));
            g.fillRoundedRectangle(bounds, radius);
        }
    }
    else
    {
        // 非激活状态
        if (shouldDrawButtonAsHighlighted)
        {
            g.setColour(juce::Colour(DarkBlueGrey::Colors::ButtonHover));
        }
        else
        {
            g.setColour(juce::Colour(DarkBlueGrey::Colors::ButtonNormal));
        }
        g.fillRoundedRectangle(bounds, radius);
        
        // 边框
        g.setColour(juce::Colour(DarkBlueGrey::Colors::PanelBorder).withAlpha(0.6f));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
    }
}

void DarkBlueGreyLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto fontSize = juce::jmin(15.0f, (float)button.getHeight() * 0.75f);
    auto tickWidth = fontSize * 1.1f;

    drawTickBox(g, button, 4.0f, ((float)button.getHeight() - tickWidth) * 0.5f,
                tickWidth, tickWidth,
                button.getToggleState(),
                button.isEnabled(),
                shouldDrawButtonAsHighlighted,
                shouldDrawButtonAsDown);

    g.setColour(button.findColour(juce::ToggleButton::textColourId));
    g.setFont(UIColors::getLabelFont(fontSize));

    if (!button.getButtonText().isEmpty())
    {
        g.drawFittedText(button.getButtonText(),
                         button.getLocalBounds().withTrimmedLeft(juce::roundToInt(tickWidth) + 10)
                                              .withTrimmedRight(2),
                         juce::Justification::centredLeft, 10);
    }
}

void DarkBlueGreyLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                                       juce::TextEditor& textEditor)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    float radius = DarkBlueGrey::Style::FieldRadius;
    
    g.setColour(juce::Colour(DarkBlueGrey::Colors::BackgroundLight));
    g.fillRoundedRectangle(bounds, radius);
}

void DarkBlueGreyLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                                    juce::TextEditor& textEditor)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    float radius = DarkBlueGrey::Style::FieldRadius;
    
    if (textEditor.hasKeyboardFocus(true) && !textEditor.isReadOnly())
    {
        g.setColour(juce::Colour(DarkBlueGrey::Colors::PrimaryBlue));
        g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 2.0f);
    }
    else
    {
        g.setColour(juce::Colour(DarkBlueGrey::Colors::PanelBorder));
        g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
    }
}

void DarkBlueGreyLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                                           int buttonX, int buttonY, int buttonW, int buttonH,
                                           juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    float radius = DarkBlueGrey::Style::FieldRadius;

    g.setColour(juce::Colour(DarkBlueGrey::Colors::BackgroundLight));
    g.fillRoundedRectangle(bounds, radius);
    
    g.setColour(juce::Colour(DarkBlueGrey::Colors::PanelBorder));
    g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
    
    // 箭头
    if (buttonW > 0 && buttonH > 0)
    {
        juce::Path arrow;
        float arrowSize = 5.0f;
        float cx = buttonX + buttonW * 0.5f;
        float cy = buttonY + buttonH * 0.5f;
        
        arrow.addTriangle(cx - arrowSize, cy - arrowSize * 0.5f,
                          cx + arrowSize, cy - arrowSize * 0.5f,
                          cx, cy + arrowSize * 0.5f);
                          
        g.setColour(juce::Colour(DarkBlueGrey::Colors::TextSecondary));
        g.fillPath(arrow);
    }
}

void DarkBlueGreyLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    
    // 背景
    g.setColour(juce::Colour(DarkBlueGrey::Colors::BackgroundMedium));
    g.fillRoundedRectangle(bounds, DarkBlueGrey::Style::ControlRadius);
    
    // 边框
    g.setColour(juce::Colour(DarkBlueGrey::Colors::PanelBorder));
    g.drawRoundedRectangle(bounds.reduced(0.5f), DarkBlueGrey::Style::ControlRadius, 1.0f);
}

void DarkBlueGreyLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                                bool isSeparator, bool isActive, bool isHighlighted,
                                                bool isTicked, bool hasSubMenu, const juce::String& text,
                                                const juce::String& shortcutKeyText,
                                                const juce::Drawable* icon, const juce::Colour* textColour)
{
    juce::ignoreUnused(hasSubMenu, shortcutKeyText, icon);
    
    if (isSeparator)
    {
        auto r = area.reduced(5, 0);
        g.setColour(juce::Colour(DarkBlueGrey::Colors::PanelBorder).withAlpha(0.4f));
        g.fillRect(r.removeFromTop(1));
        return;
    }

    if (isHighlighted)
    {
        g.setColour(juce::Colour(DarkBlueGrey::Colors::PrimaryBlue).withAlpha(0.15f));
        g.fillRect(area);
        
        g.setColour(juce::Colour(DarkBlueGrey::Colors::PrimaryBlue));
    }
    else
    {
        g.setColour(juce::Colour(DarkBlueGrey::Colors::TextPrimary));
    }

    g.setFont(UIColors::getUIFont(14.0f));
    
    auto r = area.reduced(10, 0);
    g.drawFittedText(text, r, juce::Justification::centredLeft, 1);
    
    if (isTicked)
    {
        auto tickArea = r.removeFromRight(20);
        auto tickCenter = tickArea.getCentre().toFloat();
        float tickRadius = 4.0f;
        
        g.setColour(juce::Colour(DarkBlueGrey::Colors::PrimaryBlue));
        g.fillEllipse(tickCenter.x - tickRadius, tickCenter.y - tickRadius, 
                      tickRadius * 2.0f, tickRadius * 2.0f);
    }
}

juce::Font DarkBlueGreyLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    juce::ignoreUnused(buttonHeight);
    return UIColors::getUIFont(14.0f);
}

juce::Font DarkBlueGreyLookAndFeel::getLabelFont(juce::Label&)
{
    return UIColors::getLabelFont(14.0f);
}

juce::Font DarkBlueGreyLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return UIColors::getUIFont(14.0f);
}

juce::Font DarkBlueGreyLookAndFeel::getPopupMenuFont()
{
    return UIColors::getUIFont(14.0f);
}

juce::Font DarkBlueGreyLookAndFeel::getAlertWindowTitleFont()
{
    return UIColors::getHeaderFont(18.0f);
}

juce::Font DarkBlueGreyLookAndFeel::getAlertWindowMessageFont()
{
    return UIColors::getUIFont(16.0f);
}

juce::Font DarkBlueGreyLookAndFeel::getAlertWindowFont()
{
    return UIColors::getUIFont(16.0f);
}

void DarkBlueGreyLookAndFeel::drawRotaryArc(juce::Graphics& g, float cx, float cy, float r, 
                                            float startAngle, float endAngle, float valueAngle, 
                                            const juce::Colour& color)
{
    juce::Path arc;
    arc.addCentredArc(cx, cy, r, r, 0.0f, startAngle, valueAngle, true);
    g.setColour(color);
    g.strokePath(arc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

} // namespace OpenTune
