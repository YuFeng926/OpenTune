#include "BlueBreezeLookAndFeel.h"
#include "BlueBreezeTheme.h"
#include "UIColors.h"

namespace OpenTune {

// Helper for shadow drawing
static void drawSoftShadow(juce::Graphics& g, juce::Rectangle<float> area, float radius, juce::Colour color)
{
    juce::DropShadow ds;
    ds.radius = static_cast<int>(radius * 1.5f);
    ds.offset = { 0, static_cast<int>(radius * 0.4f) };
    ds.colour = color;
    
    juce::Path p;
    p.addRoundedRectangle(area, radius);
    ds.drawForPath(g, p);
}

void BlueBreezeLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPosProportional, float rotaryStartAngle,
                                           float rotaryEndAngle, juce::Slider& slider)
{
    // ============================================================================
    // 钢琴漆旋钮 - 纯黑、凸起、金属光泽、短刻度指针
    // ============================================================================
    
    auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height)).reduced(4.0f);
    auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    auto center = bounds.getCentre();
    auto knobRadius = radius * 0.85f;
    
    bool isHovered = slider.isMouseOver();
    bool isDragging = slider.isMouseButtonDown();
    
    // 1. 凸起阴影 - 底部柔和阴影，营造浮起感
    {
        juce::DropShadow shadow;
        shadow.colour = juce::Colour(BlueBreeze::Colors::KnobShadow);
        shadow.radius = isDragging ? 12 : 8;  // 拖动时阴影更大
        shadow.offset = { 0, isDragging ? 4 : 2 };
        
        juce::Path shadowPath;
        shadowPath.addEllipse(center.x - knobRadius, center.y - knobRadius, 
                              knobRadius * 2, knobRadius * 2);
        shadow.drawForPath(g, shadowPath);
    }
    
    // 2. 旋钮主体 - 钢琴漆黑色渐变
    {
        // 从上到下的渐变：亮黑 -> 深黑 -> 亮黑（模拟光泽）
        juce::ColourGradient bodyGrad(
            juce::Colour(BlueBreeze::Colors::KnobBodyLight),  // 顶部亮
            center.x, center.y - knobRadius,
            juce::Colour(BlueBreeze::Colors::KnobBody),       // 中间深
            center.x, center.y + knobRadius,
            false
        );
        // 添加中间光泽点
        bodyGrad.addColour(0.3f, juce::Colour(BlueBreeze::Colors::KnobBody));
        bodyGrad.addColour(0.7f, juce::Colour(BlueBreeze::Colors::KnobBody).darker(0.1f));
        
        g.setGradientFill(bodyGrad);
        g.fillEllipse(center.x - knobRadius, center.y - knobRadius, 
                      knobRadius * 2, knobRadius * 2);
    }
    
    // 3. 顶部高光弧 - 钢琴漆的标志性反光
    {
        juce::Path highlightPath;
        auto highlightRadius = knobRadius * 0.95f;
        auto highlightAngleStart = -juce::MathConstants<float>::pi * 0.8f;
        auto highlightAngleEnd = -juce::MathConstants<float>::pi * 0.2f;
        
        highlightPath.addCentredArc(center.x, center.y, 
                                    highlightRadius, highlightRadius * 0.6f,  // 椭圆弧
                                    -0.3f, highlightAngleStart, highlightAngleEnd, false);
        
        juce::PathStrokeType highlightStroke(3.0f + (isHovered ? 1.0f : 0.0f), 
                                             juce::PathStrokeType::curved);
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobHighlight)
                    .withAlpha(isHovered ? 0.5f : 0.35f));
        g.strokePath(highlightPath, highlightStroke);
    }
    
    // 4. 边缘金属光泽 - 微妙的边缘高光
    {
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobEdge).withAlpha(0.6f));
        g.drawEllipse(center.x - knobRadius, center.y - knobRadius, 
                      knobRadius * 2, knobRadius * 2, 1.0f);
        
        // 内边缘高光
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobHighlight).withAlpha(0.15f));
        g.drawEllipse(center.x - knobRadius + 1, center.y - knobRadius + 1, 
                      knobRadius * 2 - 2, knobRadius * 2 - 2, 0.5f);
    }
    
    // 5. 短刻度指针 - 银白色短线
    {
        float currentAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
        
        // 指针参数
        float pointerLength = knobRadius * 0.7f;   // 指针长度
        float pointerWidth = 2.5f;                  // 指针宽度
        float pointerInset = knobRadius * 0.25f;   // 内缩距离
        
        // 计算指针端点
        auto pointerStart = center.getPointOnCircumference(pointerInset, currentAngle);
        auto pointerEnd = center.getPointOnCircumference(pointerInset + pointerLength, currentAngle);
        
        // 指针阴影（微妙的深度感）
        juce::DropShadow pointerShadow;
        pointerShadow.colour = juce::Colours::black.withAlpha(0.4f);
        pointerShadow.radius = 2;
        pointerShadow.offset = { 1, 1 };
        
        juce::Path pointerPath;
        pointerPath.startNewSubPath(pointerStart);
        pointerPath.lineTo(pointerEnd);
        pointerShadow.drawForPath(g, pointerPath);
        
        // 指针主体 - 银白色渐变
        juce::ColourGradient pointerGrad(
            juce::Colour(BlueBreeze::Colors::KnobIndicator).brighter(0.2f),
            pointerStart.x, pointerStart.y,
            juce::Colour(BlueBreeze::Colors::KnobIndicator),
            pointerEnd.x, pointerEnd.y,
            false
        );
        
        juce::PathStrokeType pointerStroke(pointerWidth, 
                                           juce::PathStrokeType::curved, 
                                           juce::PathStrokeType::rounded);
        g.setGradientFill(pointerGrad);
        g.strokePath(pointerPath, pointerStroke);
        
        // 指针顶端圆点
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobIndicator));
        g.fillEllipse(pointerEnd.x - 2.0f, pointerEnd.y - 2.0f, 4.0f, 4.0f);
    }
    
    // 6. 悬停/拖动交互反馈
    if (isHovered || isDragging)
    {
        // 外发光效果
        float glowAlpha = isDragging ? 0.25f : 0.15f;
        float glowExpand = isDragging ? 3.0f : 2.0f;
        
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobGlow).withAlpha(glowAlpha));
        g.drawEllipse(center.x - knobRadius - glowExpand, 
                      center.y - knobRadius - glowExpand,
                      knobRadius * 2 + glowExpand * 2, 
                      knobRadius * 2 + glowExpand * 2, 
                      glowExpand);
    }
    
    // 7. 中心值文本（可选，保持简洁）
    if (!isDragging)  // 拖动时不显示文本
    {
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobIndicator).withAlpha(0.7f));
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        
        juce::String text;
        if (slider.getValue() < 10.0) 
            text = juce::String(slider.getValue(), 1);
        else 
            text = juce::String(static_cast<int>(slider.getValue()));
        
        g.drawText(text, bounds, juce::Justification::centred, false);
    }
}

void BlueBreezeLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float minSliderPos, float maxSliderPos,
                                           const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height));
    
    if (style == juce::Slider::LinearVertical)
    {
        float trackW = 4.0f;
        auto trackRect = bounds.withWidth(trackW).withX(bounds.getCentreX() - trackW / 2.0f).reduced(0.0f, 4.0f);
        
        // Track Background
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobBody).withAlpha(0.2f));
        g.fillRoundedRectangle(trackRect, trackW/2);
        
        // Active Fill
        auto fillRect = trackRect.withTop(sliderPos);
        g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
        g.fillRoundedRectangle(fillRect, trackW/2);
        
        // Capsule Thumb
        float thumbW = 24.0f;
        float thumbH = 12.0f;
        auto thumbRect = juce::Rectangle<float>(bounds.getCentreX() - thumbW/2, sliderPos - thumbH/2, thumbW, thumbH);
        
        drawSoftShadow(g, thumbRect, 4.0f, juce::Colour(BlueBreeze::Colors::ShadowColor).withAlpha(0.2f));
        
        g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite));
        g.fillRoundedRectangle(thumbRect, thumbH/2);
        
        if (slider.isMouseOverOrDragging())
        {
            g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
            g.drawRoundedRectangle(thumbRect, thumbH/2, 1.0f);
        }
    }
    else
    {
        // Horizontal logic similar to Vertical...
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
    }
}

void BlueBreezeLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                               const juce::Colour& backgroundColour,
                                               bool shouldDrawButtonAsHighlighted,
                                               bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
    float radius = bounds.getHeight() * 0.5f; // Pill shape

    bool isActive = button.getToggleState() || shouldDrawButtonAsDown;
    
    if (isActive)
    {
        // Active: Pure White Background + Dark Text (Inverted)
        
        // Shadow for depth
        drawSoftShadow(g, bounds, radius, juce::Colour(BlueBreeze::Colors::ShadowColor).withAlpha(0.2f));
        
        g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite));
        g.fillRoundedRectangle(bounds, radius);
        
        // Inner shadow for pressed state
        if (shouldDrawButtonAsDown)
        {
            g.setColour(juce::Colours::black.withAlpha(0.1f));
            g.fillRoundedRectangle(bounds, radius);
        }
    }
    else
    {
        // Inactive: Transparent/Subtle Background + Dark Text
        
        if (shouldDrawButtonAsHighlighted)
        {
            g.setColour(juce::Colour(BlueBreeze::Colors::HoverOverlay)); // Slight white overlay
            g.fillRoundedRectangle(bounds, radius);
        }
        
        // Optional: Very subtle border
        g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder).withAlpha(0.5f));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
    }
}

void BlueBreezeLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    // Minimalist Checkbox/Radio
    auto fontSize = juce::jmin(15.0f, (float)button.getHeight() * 0.75f);
    auto tickWidth = fontSize * 1.1f;

    drawTickBox(g, button, 4.0f, ((float)button.getHeight() - tickWidth) * 0.5f,
                tickWidth, tickWidth,
                button.getToggleState(),
                button.isEnabled(),
                shouldDrawButtonAsHighlighted,
                shouldDrawButtonAsDown);

    g.setColour(button.findColour(juce::ToggleButton::textColourId));
    g.setFont(fontSize);

    if (!button.getButtonText().isEmpty())
    {
        g.drawFittedText(button.getButtonText(),
                         button.getLocalBounds().withTrimmedLeft(juce::roundToInt(tickWidth) + 10)
                                              .withTrimmedRight(2),
                         juce::Justification::centredLeft, 10);
    }
}

void BlueBreezeLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                                   juce::TextEditor& textEditor)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    float radius = BlueBreeze::Style::ControlRadius;
    
    g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite).withAlpha(0.8f));
    g.fillRoundedRectangle(bounds, radius);
}

void BlueBreezeLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                                juce::TextEditor& textEditor)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    float radius = BlueBreeze::Style::ControlRadius;
    
    if (textEditor.hasKeyboardFocus(true) && !textEditor.isReadOnly())
    {
        g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
        g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 2.0f);
    }
    else
    {
        g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder));
        g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
    }
}

void BlueBreezeLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                                       int buttonX, int buttonY, int buttonW, int buttonH,
                                       juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    float radius = BlueBreeze::Style::ControlRadius;

    g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite).withAlpha(0.6f));
    g.fillRoundedRectangle(bounds, radius);
    
    g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder));
    g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
    
    // Arrow
    if (buttonW > 0 && buttonH > 0)
    {
        juce::Path arrow;
        float arrowSize = 5.0f;
        float cx = buttonX + buttonW * 0.5f;
        float cy = buttonY + buttonH * 0.5f;
        
        arrow.addTriangle(cx - arrowSize, cy - arrowSize * 0.5f,
                          cx + arrowSize, cy - arrowSize * 0.5f,
                          cx, cy + arrowSize * 0.5f);
                          
        g.setColour(juce::Colour(BlueBreeze::Colors::TextDim));
        g.fillPath(arrow);
    }
}

void BlueBreezeLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    
    // Clean, flat background with border
    g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite));
    g.fillRect(bounds);
    
    g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder));
    g.drawRect(bounds, 1.0f);
}

void BlueBreezeLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                            bool isSeparator, bool isActive, bool isHighlighted,
                                            bool isTicked, bool hasSubMenu, const juce::String& text,
                                            const juce::String& shortcutKeyText,
                                            const juce::Drawable* icon, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        auto r = area.reduced(5, 0);
        g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder).withAlpha(0.3f));
        g.fillRect(r.removeFromTop(1));
        return;
    }

    if (isHighlighted)
    {
        g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue).withAlpha(0.1f));
        g.fillRect(area);
        
        g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
    }
    else
    {
        g.setColour(juce::Colour(BlueBreeze::Colors::TextDark));
    }

    g.setFont(UIColors::getUIFont(14.0f));
    
    auto r = area.reduced(10, 0);
    g.drawFittedText(text, r, juce::Justification::centredLeft, 1);
    
    if (isTicked)
    {
        auto tickArea = r.removeFromRight(20);
        auto tickCenter = tickArea.getCentre().toFloat();
        float tickRadius = 4.0f;
        
        g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
        g.fillEllipse(tickCenter.x - tickRadius, tickCenter.y - tickRadius, 
                      tickRadius * 2.0f, tickRadius * 2.0f);
    }
}

juce::Font BlueBreezeLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return UIColors::getUIFont(14.0f);
}

juce::Font BlueBreezeLookAndFeel::getLabelFont(juce::Label&)
{
    return UIColors::getLabelFont(14.0f);
}

juce::Font BlueBreezeLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return UIColors::getUIFont(14.0f);
}

juce::Font BlueBreezeLookAndFeel::getPopupMenuFont()
{
    return UIColors::getUIFont(14.0f);
}

juce::Font BlueBreezeLookAndFeel::getAlertWindowTitleFont()
{
    return UIColors::getUIFont(18.0f);
}

juce::Font BlueBreezeLookAndFeel::getAlertWindowMessageFont()
{
    return UIColors::getUIFont(16.0f);
}

juce::Font BlueBreezeLookAndFeel::getAlertWindowFont()
{
    return UIColors::getUIFont(16.0f);
}

} // namespace OpenTune
