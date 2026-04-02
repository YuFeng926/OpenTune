#include "AuroraLookAndFeel.h"
#include "AuroraTheme.h"
#include "UIColors.h"

namespace OpenTune {

// Helper: Draw Neon Glow
void AuroraLookAndFeel::drawNeonGlow(juce::Graphics& g, juce::Path& path, juce::Colour color, float intensity)
{
    // Layered Glow for Richness
    // Inner bright core
    g.setColour(color.withAlpha(0.8f * intensity));
    g.strokePath(path, juce::PathStrokeType(1.5f));
    
    // Middle soft glow
    {
        juce::DropShadow glow;
        glow.radius = static_cast<int>(8.0f * intensity);
        glow.colour = color.withAlpha(0.4f * intensity);
        glow.offset = {0, 0};
        glow.drawForPath(g, path);
    }

    // Outer atmospheric dispersion
    {
        juce::DropShadow glow;
        glow.radius = static_cast<int>(16.0f * intensity);
        glow.colour = color.withAlpha(0.2f * intensity);
        glow.offset = {0, 0};
        glow.drawForPath(g, path);
    }
}

// Helper: Glass Gradient
static juce::ColourGradient createGlassGradient(juce::Rectangle<float> bounds)
{
    // Subtle vertical gradient for card depth
    return juce::ColourGradient(
        juce::Colour(Aurora::Colors::BgSurface).brighter(0.05f), bounds.getTopLeft(),
        juce::Colour(Aurora::Colors::BgSurface).darker(0.05f), bounds.getBottomRight(),
        false
    );
}

// Helper: Rainbow Gradient for Text
static void drawRainbowText(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> bounds, juce::Justification justification)
{
    juce::ColourGradient rainbow(
        juce::Colour(Aurora::Colors::Cyan), bounds.getX(), bounds.getCentreY(),
        juce::Colour(Aurora::Colors::NeonOrange), bounds.getRight(), bounds.getCentreY(),
        false
    );
    rainbow.addColour(0.3f, juce::Colour(Aurora::Colors::NeonGreen));
    rainbow.addColour(0.6f, juce::Colour(Aurora::Colors::NeonYellow));
    
    g.setGradientFill(rainbow);
    g.drawText(text, bounds, justification, false);
}

void AuroraLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                       float sliderPosProportional, float rotaryStartAngle,
                                       float rotaryEndAngle, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height).reduced(2.0f);
    auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    auto center = bounds.getCentre();
    auto trackRadius = radius * 0.85f;

    // 1. Knob Body (Deep Card Style)
    g.setColour(juce::Colour(Aurora::Colors::KnobBody));
    g.fillEllipse(center.x - trackRadius, center.y - trackRadius, trackRadius * 2, trackRadius * 2);

    // 2. Track Background (Dashed/Tech Look)
    juce::Path bgArc;
    bgArc.addCentredArc(center.x, center.y, trackRadius * 0.9f, trackRadius * 0.9f, 
                        0.0f, rotaryStartAngle, rotaryEndAngle, true);
    
    // Dashed effect for tech feel
    float dashLengths[] = { 2.0f, 2.0f };
    juce::PathStrokeType stroke(2.0f);
    stroke.createDashedStroke(bgArc, bgArc, dashLengths, 2);
    
    g.setColour(juce::Colour(Aurora::Colors::BorderLight));
    g.strokePath(bgArc, stroke);

    // 3. Value Arc (Gradient Neon)
    float currentAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    
    juce::Path valueArc;
    valueArc.addCentredArc(center.x, center.y, trackRadius * 0.9f, trackRadius * 0.9f, 
                           0.0f, rotaryStartAngle, currentAngle, true);

    if (slider.isEnabled())
    {
        // Dynamic Color based on value (Blue -> Green -> Orange -> Red)
        juce::Colour valueColor;
        if (sliderPosProportional < 0.33f) valueColor = juce::Colour(Aurora::Colors::Cyan).interpolatedWith(juce::Colour(Aurora::Colors::NeonGreen), sliderPosProportional * 3.0f);
        else if (sliderPosProportional < 0.66f) valueColor = juce::Colour(Aurora::Colors::NeonGreen).interpolatedWith(juce::Colour(Aurora::Colors::NeonOrange), (sliderPosProportional - 0.33f) * 3.0f);
        else valueColor = juce::Colour(Aurora::Colors::NeonOrange).interpolatedWith(juce::Colour(Aurora::Colors::NeonRed), (sliderPosProportional - 0.66f) * 3.0f);

        // Glow Effect
        float intensity = slider.isMouseOverOrDragging() ? 1.0f : 0.6f;
        drawNeonGlow(g, valueArc, valueColor, intensity);

        g.setColour(valueColor);
        g.strokePath(valueArc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        
        // Knob Indicator Dot
        juce::Point<float> thumbPoint(
            center.x + trackRadius * 0.9f * std::cos(currentAngle - juce::MathConstants<float>::halfPi),
            center.y + trackRadius * 0.9f * std::sin(currentAngle - juce::MathConstants<float>::halfPi)
        );
        
        float dotSize = 6.0f;
        g.setColour(valueColor.brighter());
        g.fillEllipse(thumbPoint.x - dotSize/2, thumbPoint.y - dotSize/2, dotSize, dotSize);
        
        juce::Path dotPath;
        dotPath.addEllipse(thumbPoint.x - dotSize/2, thumbPoint.y - dotSize/2, dotSize, dotSize);
        drawNeonGlow(g, dotPath, valueColor, intensity);
    }

    // 4. Value Text (Rainbow Gradient)
    if (slider.isMouseOverOrDragging())
    {
        g.setFont(UIColors::getUIFont(12.0f).withStyle(juce::Font::bold));
        
        juce::String text;
        if (slider.getValue() < 10.0) text = juce::String(slider.getValue(), 1);
        else text = juce::String((int)slider.getValue());
        
        // Draw centered rainbow text
        drawRainbowText(g, text, bounds, juce::Justification::centred);
    }
}

void AuroraLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                       float sliderPos, float minSliderPos, float maxSliderPos,
                                       const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height);
    
    if (style == juce::Slider::LinearVertical)
    {
        float trackW = 4.0f;
        auto trackRect = bounds.withWidth(trackW).withX(bounds.getCentreX() - trackW / 2.0f).reduced(0.0f, 4.0f);
        
        // Dark Track
        g.setColour(juce::Colour(Aurora::Colors::BgDeep).withAlpha(0.5f));
        g.fillRoundedRectangle(trackRect, trackW/2);
        
        // Neon Fill
        float fillTop = juce::jmax(trackRect.getY(), sliderPos);
        float fillBottom = trackRect.getBottom();
        
        if (fillTop < fillBottom)
        {
            juce::Rectangle<float> activeTrack(trackRect.getX(), fillTop, trackRect.getWidth(), fillBottom - fillTop);
            
            // Glow
            juce::Path glowPath;
            glowPath.addRoundedRectangle(activeTrack, trackW/2);
            drawNeonGlow(g, glowPath, juce::Colour(Aurora::Colors::Cyan), 0.8f);
            
            g.setColour(juce::Colour(Aurora::Colors::Cyan));
            g.fillRoundedRectangle(activeTrack, trackW/2);
        }
        
        // Thumb (Capsule style - unified with other themes)
        float thumbW = 24.0f;
        float thumbH = 12.0f;
        auto thumbRect = juce::Rectangle<float>(bounds.getCentreX() - thumbW/2, sliderPos - thumbH/2, thumbW, thumbH);
        
        g.setColour(juce::Colour(Aurora::Colors::TextPrimary));
        g.fillRoundedRectangle(thumbRect, thumbH/2);
        
        if (slider.isMouseOverOrDragging())
        {
            juce::Path p;
            p.addRoundedRectangle(thumbRect, thumbH/2);
            drawNeonGlow(g, p, juce::Colour(Aurora::Colors::TextPrimary), 1.0f);
        }
    }
    else
    {
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
    }
}

void AuroraLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                           const juce::Colour& backgroundColour,
                                           bool shouldDrawButtonAsHighlighted,
                                           bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
    float radius = Aurora::Style::ControlRadius;

    bool isActive = button.getToggleState() || shouldDrawButtonAsDown;
    
    if (isActive)
    {
        // Neon Border + Glow
        g.setColour(juce::Colour(Aurora::Colors::Cyan).withAlpha(0.1f));
        g.fillRoundedRectangle(bounds, radius);
        
        g.setColour(juce::Colour(Aurora::Colors::Cyan));
        g.drawRoundedRectangle(bounds, radius, 1.5f);
        
        juce::Path p;
        p.addRoundedRectangle(bounds, radius);
        drawNeonGlow(g, p, juce::Colour(Aurora::Colors::Cyan), 0.6f);
    }
    else
    {
        // Glass Look
        g.setGradientFill(createGlassGradient(bounds));
        g.fillRoundedRectangle(bounds, radius);
        
        if (shouldDrawButtonAsHighlighted)
        {
            g.setColour(juce::Colour(Aurora::Colors::BorderGlow));
            g.drawRoundedRectangle(bounds, radius, 1.0f);
        }
        else
        {
            g.setColour(juce::Colour(Aurora::Colors::BorderLight));
            g.drawRoundedRectangle(bounds, radius, 1.0f);
        }
    }
}

void AuroraLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                       bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto fontSize = juce::jmin(15.0f, (float)button.getHeight() * 0.75f);
    auto tickWidth = fontSize * 1.1f;

    auto bounds = button.getLocalBounds().toFloat();
    auto tickBounds = juce::Rectangle<float>(4.0f, (bounds.getHeight() - tickWidth) * 0.5f, tickWidth, tickWidth);
    
    float checkRadius = 4.0f;
    
    // Checkbox Background
    g.setColour(juce::Colour(Aurora::Colors::BgDeep));
    g.fillRoundedRectangle(tickBounds, checkRadius);
    
    g.setColour(juce::Colour(Aurora::Colors::BorderLight));
    g.drawRoundedRectangle(tickBounds, checkRadius, 1.0f);

    if (button.getToggleState())
    {
        g.setColour(juce::Colour(Aurora::Colors::Cyan));
        g.fillRoundedRectangle(tickBounds.reduced(3.0f), checkRadius * 0.5f);
        
        juce::Path p;
        p.addRoundedRectangle(tickBounds.reduced(3.0f), checkRadius * 0.5f);
        drawNeonGlow(g, p, juce::Colour(Aurora::Colors::Cyan), 0.8f);
    }

    g.setColour(juce::Colour(Aurora::Colors::TextPrimary));
    g.setFont(fontSize);

    if (!button.getButtonText().isEmpty())
    {
        g.drawFittedText(button.getButtonText(),
                         button.getLocalBounds().withTrimmedLeft(juce::roundToInt(tickWidth) + 10)
                                              .withTrimmedRight(2),
                         juce::Justification::centredLeft, 10);
    }
}

void AuroraLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                               juce::TextEditor& textEditor)
{
    auto bounds = juce::Rectangle<float>((float)width, (float)height);
    float radius = Aurora::Style::ControlRadius;
    
    g.setColour(juce::Colour(Aurora::Colors::BgDeep).withAlpha(0.6f));
    g.fillRoundedRectangle(bounds, radius);
}

void AuroraLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                            juce::TextEditor& textEditor)
{
    auto bounds = juce::Rectangle<float>((float)width, (float)height);
    float radius = Aurora::Style::ControlRadius;
    
    if (textEditor.hasKeyboardFocus(true) && !textEditor.isReadOnly())
    {
        g.setColour(juce::Colour(Aurora::Colors::Cyan));
        g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.5f);
        
        juce::Path p;
        p.addRoundedRectangle(bounds.reduced(0.5f), radius);
        drawNeonGlow(g, p, juce::Colour(Aurora::Colors::Cyan), 0.5f);
    }
    else
    {
        g.setColour(juce::Colour(Aurora::Colors::BorderLight));
        g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
    }
}

void AuroraLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                                   int buttonX, int buttonY, int buttonW, int buttonH,
                                   juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    float radius = Aurora::Style::ControlRadius;

    bool isActive = isButtonDown || box.isPopupActive();
    
    // Glass Background
    g.setGradientFill(createGlassGradient(bounds));
    g.fillRoundedRectangle(bounds, radius);
    
    if (isActive)
    {
        g.setColour(juce::Colour(Aurora::Colors::Cyan));
        g.drawRoundedRectangle(bounds, radius, 1.5f);
    }
    else
    {
        g.setColour(juce::Colour(Aurora::Colors::BorderLight));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
    }
    
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
                          
        g.setColour(juce::Colour(Aurora::Colors::TextSecondary));
        g.fillPath(arrow);
    }
}

void AuroraLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    float radius = Aurora::Style::ControlRadius;
    
    // Dark Glass
    g.setColour(juce::Colour(Aurora::Colors::BgDeep).withAlpha(0.95f));
    g.fillRoundedRectangle(bounds, radius);
    
    g.setColour(juce::Colour(Aurora::Colors::BorderLight));
    g.drawRoundedRectangle(bounds, radius, 1.0f);
}

void AuroraLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                        bool isSeparator, bool isActive, bool isHighlighted,
                                        bool isTicked, bool hasSubMenu, const juce::String& text,
                                        const juce::String& shortcutKeyText,
                                        const juce::Drawable* icon, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        auto r = area.reduced(5, 0);
        g.setColour(juce::Colour(Aurora::Colors::BorderLight));
        g.fillRect(r.removeFromTop(1));
        return;
    }

    if (isHighlighted)
    {
        g.setColour(juce::Colour(Aurora::Colors::Cyan).withAlpha(0.15f));
        g.fillRect(area);
        
        g.setColour(juce::Colour(Aurora::Colors::Cyan));
        auto r = area.reduced(2, 0);
        g.fillRect(r.removeFromLeft(3)); // Side highlight bar
        
        g.setColour(juce::Colour(Aurora::Colors::TextPrimary));
    }
    else
    {
        g.setColour(juce::Colour(Aurora::Colors::TextSecondary));
    }

    g.setFont(UIColors::getUIFont(14.0f));
    
    auto r = area.reduced(10, 0);
    g.drawFittedText(text, r, juce::Justification::centredLeft, 1);
    
    if (isTicked)
    {
        auto tickArea = r.removeFromRight(20);
        auto tickCenter = tickArea.getCentre().toFloat();
        float tickRadius = 4.0f;
        
        g.setColour(juce::Colour(Aurora::Colors::Cyan));
        g.fillEllipse(tickCenter.x - tickRadius, tickCenter.y - tickRadius, 
                      tickRadius * 2.0f, tickRadius * 2.0f);
    }
}

juce::Font AuroraLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return UIColors::getUIFont(14.0f);
}

juce::Font AuroraLookAndFeel::getLabelFont(juce::Label&)
{
    return UIColors::getLabelFont(14.0f);
}

juce::Font AuroraLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return UIColors::getUIFont(14.0f);
}

juce::Font AuroraLookAndFeel::getPopupMenuFont()
{
    return UIColors::getUIFont(14.0f);
}

juce::Font AuroraLookAndFeel::getAlertWindowTitleFont()
{
    return UIColors::getUIFont(18.0f);
}

juce::Font AuroraLookAndFeel::getAlertWindowMessageFont()
{
    return UIColors::getUIFont(16.0f);
}

juce::Font AuroraLookAndFeel::getAlertWindowFont()
{
    return UIColors::getUIFont(16.0f);
}

} // namespace OpenTune
