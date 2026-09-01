#include "AuroraLookAndFeel.h"
#include "AuroraTheme.h"
#include "UIColors.h"
#include "../../Utils/CursorTheme.h"

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

void AuroraLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                       float sliderPosProportional, float rotaryStartAngle,
                                       float rotaryEndAngle, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height).reduced(2.0f);
    const auto enabledAlpha = slider.isEnabled() ? 1.0f : 0.42f;

    juce::Graphics::ScopedSaveState saveState(g);
    g.setOpacity(enabledAlpha);
    UIColors::drawAuroraKnob(g,
                             bounds,
                             sliderPosProportional,
                             slider.isMouseOverOrDragging(),
                             rotaryStartAngle,
                             rotaryEndAngle,
                             &slider);

    if (slider.isMouseOverOrDragging() && slider.isEnabled())
    {
        g.setFont(UIColors::getUIFont(12.0f).withStyle(juce::Font::bold));

        juce::String text;
        if (slider.getValue() < 10.0) text = juce::String(slider.getValue(), 1);
        else text = juce::String((int)slider.getValue());

        g.setColour(UIColors::textPrimary.withMultipliedAlpha(0.86f));
        g.drawText(text, bounds.reduced(4.0f), juce::Justification::centred, false);
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
    const auto chromeIntensity = static_cast<float>(button.getProperties().getWithDefault(
        UIColors::auroraChromeIntensityProperty, 1.0f));

    UIColors::drawAuroraButtonChrome(g,
                                     bounds,
                                     radius,
                                     shouldDrawButtonAsHighlighted,
                                     shouldDrawButtonAsDown,
                                     button.getToggleState(),
                                     {},
                                     {},
                                     nullptr,
                                     chromeIntensity);
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

    UIColors::fillAuroraGlass(g, bounds, radius);
}

void AuroraLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                            juce::TextEditor& textEditor)
{
    auto bounds = juce::Rectangle<float>((float)width, (float)height);
    float radius = Aurora::Style::ControlRadius;
    
    const auto focused = textEditor.hasKeyboardFocus(true) && !textEditor.isReadOnly();
    if (focused)
        UIColors::drawAuroraGlow(g, bounds.reduced(0.5f), UIColors::knobGlow, 0.38f, 0.58f);

    UIColors::drawAuroraGlassFrame(g, bounds, radius, focused);
}

void AuroraLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                                   int buttonX, int buttonY, int buttonW, int buttonH,
                                   juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
    float radius = Aurora::Style::ControlRadius;
    const auto chromeIntensity = static_cast<float>(box.getProperties().getWithDefault(
        UIColors::auroraChromeIntensityProperty, 1.0f));

    bool isActive = isButtonDown || box.isPopupActive();

    UIColors::drawAuroraButtonChrome(g,
                                     bounds.reduced(2.0f), // 与 UnifiedToolbarButton 缩进一致，视觉高度统一 36px
                                     radius,
                                     box.isMouseOver(),
                                     isButtonDown,
                                     isActive,
                                     {},
                                     {},
                                     nullptr,
                                     chromeIntensity);
    
    // Arrow
    if (buttonW > 0 && buttonH > 0)
    {
        juce::Path arrow;
        float arrowSize = 4.25f;
        float cx = buttonX + buttonW * 0.5f;
        float cy = buttonY + buttonH * 0.5f;
        
        arrow.addTriangle(cx - arrowSize, cy - arrowSize * 0.5f,
                          cx + arrowSize, cy - arrowSize * 0.5f,
                          cx, cy + arrowSize * 0.5f);
                          
        g.setColour(UIColors::textSecondary.withAlpha(isActive ? 0.78f : 0.62f));
        g.fillPath(arrow);
    }
}

int AuroraLookAndFeel::getDefaultScrollbarWidth()
{
    return UIColors::scrollBarThickness;
}

void AuroraLookAndFeel::drawScrollbar(juce::Graphics& g,
                                      juce::ScrollBar& scrollBar,
                                      int x,
                                      int y,
                                      int width,
                                      int height,
                                      bool isScrollbarVertical,
                                      int thumbStartPosition,
                                      int thumbSize,
                                      bool isMouseOver,
                                      bool isMouseDown)
{
    juce::ignoreUnused(scrollBar);

    auto bounds = juce::Rectangle<float>(static_cast<float>(x),
                                         static_cast<float>(y),
                                         static_cast<float>(width),
                                         static_cast<float>(height));
    if (bounds.isEmpty())
        return;

    const float availableThickness = isScrollbarVertical ? bounds.getWidth() : bounds.getHeight();
    const float trackThickness = juce::jmin(UIColors::scrollBarThumbThickness, availableThickness * 0.55f);
    auto track = isScrollbarVertical
        ? bounds.withWidth(trackThickness).withX(bounds.getCentreX() - trackThickness * 0.5f).reduced(0.0f, 8.0f)
        : bounds.withHeight(trackThickness).withY(bounds.getCentreY() - trackThickness * 0.5f).reduced(8.0f, 0.0f);

    g.setColour(UIColors::glassSurface.withAlpha(0.12f));
    g.fillRoundedRectangle(track, trackThickness * 0.5f);

    if (thumbSize <= 0)
        return;

    auto thumb = isScrollbarVertical
        ? juce::Rectangle<float>(bounds.getCentreX() - trackThickness * 0.5f,
                                 static_cast<float>(thumbStartPosition),
                                 trackThickness,
                                 static_cast<float>(thumbSize)).reduced(0.0f, 2.0f)
        : juce::Rectangle<float>(static_cast<float>(thumbStartPosition),
                                 bounds.getCentreY() - trackThickness * 0.5f,
                                 static_cast<float>(thumbSize),
                                 trackThickness).reduced(2.0f, 0.0f);

    thumb = thumb.getIntersection(bounds.reduced(3.0f));
    if (thumb.isEmpty())
        return;

    const auto glowAlpha = isMouseDown ? 0.24f : (isMouseOver ? 0.18f : 0.08f);
    UIColors::drawAuroraGlow(g, thumb, UIColors::correctedF0, glowAlpha, 0.38f);

    juce::ColourGradient fill(UIColors::correctedF0.withAlpha(isMouseDown ? 0.44f : 0.34f),
                              thumb.getX(),
                              thumb.getY(),
                              UIColors::panelGlow.withAlpha(isMouseDown ? 0.20f : 0.14f),
                              thumb.getRight(),
                              thumb.getBottom(),
                              false);
    g.setGradientFill(fill);
    g.fillRoundedRectangle(thumb, trackThickness * 0.5f);

    const auto outline = UIColors::correctedF0.withAlpha(isMouseDown ? 0.82f : (isMouseOver ? 0.70f : 0.56f));
    g.setColour(outline);
    g.drawRoundedRectangle(thumb.reduced(0.5f), trackThickness * 0.5f, 1.0f);
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

juce::Font AuroraLookAndFeel::getComboBoxFont(juce::ComboBox& box)
{
    if (box.getProperties().contains("fontHeight"))
        return UIColors::getUIFont(static_cast<float>(static_cast<double>(box.getProperties()["fontHeight"])));

    return UIColors::getUIFont(16.0f);
}

juce::Font AuroraLookAndFeel::getPopupMenuFont()
{
    return UIColors::getUIFont(14.0f);
}

juce::Label* AuroraLookAndFeel::createComboBoxTextBox(juce::ComboBox& box)
{
    auto* label = new juce::Label();
    label->setFont(getComboBoxFont(box));
    label->setMinimumHorizontalScale(1.0f);

    if (box.getProperties().contains("noArrow"))
        label->setJustificationType(juce::Justification::centred);
    else
        label->setJustificationType(juce::Justification::centredLeft);

    return label;
}

void AuroraLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setFont(getComboBoxFont(box));
    label.setMinimumHorizontalScale(1.0f);

    if (box.getProperties().contains("noArrow"))
    {
        label.setBounds(0, 0, box.getWidth(), box.getHeight());
    }
    else
    {
        label.setBounds(6, 1, box.getWidth() - 36, box.getHeight() - 2);
    }
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

// ============================================================================
// Process-level default LookAndFeel
// ============================================================================

void AuroraLookAndFeel::installAsDefault()
{
    auto& instance = getDefaultInstance();

    instance.setColour(juce::AlertWindow::backgroundColourId,
                       juce::Colour(Aurora::Colors::BgSurface));
    instance.setColour(juce::AlertWindow::textColourId,
                       juce::Colour(Aurora::Colors::TextPrimary));
    instance.setColour(juce::AlertWindow::outlineColourId,
                       juce::Colour(Aurora::Colors::GlassEdge));
    instance.setColour(juce::ResizableWindow::backgroundColourId,
                       juce::Colour(Aurora::Colors::BgDeep));
    instance.setColour(juce::DocumentWindow::textColourId,
                       juce::Colour(Aurora::Colors::TextPrimary));

    juce::LookAndFeel::setDefaultLookAndFeel(&instance);
}

AuroraLookAndFeel& AuroraLookAndFeel::getDefaultInstance()
{
    static AuroraLookAndFeel instance;
    return instance;
}

// ============================================================================
// AlertWindow — Aurora dark-glass style
// ============================================================================

void AuroraLookAndFeel::drawAlertBox(juce::Graphics& g, juce::AlertWindow& alert,
                                     const juce::Rectangle<int>& textArea,
                                     juce::TextLayout& textLayout)
{
    auto bounds = alert.getLocalBounds().reduced(1);
    float radius = Aurora::Style::ControlRadius;

    // Background — frosted dark glass
    {
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(bounds);
        UIColors::fillAuroraGlass(g, bounds.toFloat(), radius);
    }

    // Outline
    g.setColour(alert.findColour(juce::AlertWindow::outlineColourId));
    g.drawRoundedRectangle(bounds.toFloat(), radius, 1.5f);

    // Top highlight line
    g.setColour(juce::Colour(Aurora::Colors::GlassHighlight));
    g.drawLine(bounds.getX() + radius, bounds.getY() + 1.0f,
               bounds.getRight() - radius, bounds.getY() + 1.0f, 1.0f);

    // Icon
    auto iconSpaceUsed = 0;
    auto iconSize = juce::jmin(130, bounds.getHeight() + 20);

    if (alert.containsAnyExtraComponents() || alert.getNumButtons() > 2)
        iconSize = juce::jmin(iconSize, textArea.getHeight() + 50);

    juce::Rectangle<int> iconRect(iconSize / -10, iconSize / -10, iconSize, iconSize);

    if (alert.getAlertType() != juce::MessageBoxIconType::NoIcon)
    {
        juce::Path icon;
        juce::juce_wchar character;
        juce::Colour colour;

        if (alert.getAlertType() == juce::MessageBoxIconType::WarningIcon)
        {
            character = '!';
            icon.addTriangle(
                static_cast<float>(iconRect.getX()) + static_cast<float>(iconRect.getWidth()) * 0.5f,
                static_cast<float>(iconRect.getY()),
                static_cast<float>(iconRect.getRight()),
                static_cast<float>(iconRect.getBottom()),
                static_cast<float>(iconRect.getX()),
                static_cast<float>(iconRect.getBottom()));
            icon = icon.createPathWithRoundedCorners(5.0f);
            colour = juce::Colour(Aurora::Colors::Error);
        }
        else
        {
            character = alert.getAlertType() == juce::MessageBoxIconType::InfoIcon ? 'i' : '?';
            icon.addEllipse(iconRect.toFloat());
            colour = juce::Colour(Aurora::Colors::Cyan).withAlpha(0.6f);
        }

        juce::GlyphArrangement ga;
        ga.addFittedText(juce::Font(juce::FontOptions(static_cast<float>(iconRect.getHeight()) * 0.9f, juce::Font::bold)),
                         juce::String::charToString(character),
                         static_cast<float>(iconRect.getX()),
                         static_cast<float>(iconRect.getY()),
                         static_cast<float>(iconRect.getWidth()),
                         static_cast<float>(iconRect.getHeight()),
                         juce::Justification::centred, false);
        ga.createPath(icon);
        icon.setUsingNonZeroWinding(false);
        g.setColour(colour);
        g.fillPath(icon);

        iconSpaceUsed = 80;
    }

    // Text
    g.setColour(alert.findColour(juce::AlertWindow::textColourId));
    textLayout.draw(g, juce::Rectangle<int>(
        textArea.getX() + iconSpaceUsed,
        textArea.getY(),
        textArea.getWidth() - iconSpaceUsed,
        textArea.getHeight()).toFloat());
}

// ============================================================================
// DocumentWindow title bar — Aurora glass style
// ============================================================================

void AuroraLookAndFeel::drawDocumentWindowTitleBar(juce::DocumentWindow& window, juce::Graphics& g,
                                                   int w, int h, int titleSpaceX, int titleSpaceW,
                                                   const juce::Image* icon, bool drawTitleTextOnLeft)
{
    juce::ignoreUnused(drawTitleTextOnLeft);

    if (w * h == 0)
        return;

    auto bounds = juce::Rectangle<int>(0, 0, w, h).toFloat();
    auto titleBarBg = window.findColour(juce::DocumentWindow::backgroundColourId);

    g.setColour(titleBarBg);
    g.fillAll();

    // Bottom separator
    g.setColour(juce::Colour(Aurora::Colors::BorderLight));
    g.drawLine(bounds.getX(), bounds.getBottom() - 1.0f,
               bounds.getRight(), bounds.getBottom() - 1.0f, 1.0f);

    // Title text — always white on dark Aurora background
    g.setColour(juce::Colour(Aurora::Colors::TextPrimary).withAlpha(0.92f));
    g.setFont(UIColors::getUIFont(14.0f));

    auto font = g.getCurrentFont();
    auto textW = juce::GlyphArrangement::getStringWidthInt(font, window.getName());
    auto iconW = 0;
    auto iconH = 0;

    if (icon != nullptr)
    {
        iconH = static_cast<int>(font.getHeight());
        iconW = icon->getWidth() * iconH / icon->getHeight() + 4;
    }

    textW = juce::jmin(titleSpaceW, textW + iconW);
    auto textX = juce::jmax(titleSpaceX, (w - textW) / 2);

    if (textX + textW > titleSpaceX + titleSpaceW)
        textX = titleSpaceX + titleSpaceW - textW;

    if (icon != nullptr)
    {
        g.setOpacity(window.isActiveWindow() ? 1.0f : 0.6f);
        g.drawImageWithin(*icon, textX, (h - iconH) / 2, iconW, iconH,
                          juce::RectanglePlacement::centred, false);
        textX += iconW;
        textW -= iconW;
    }

    g.drawText(window.getName(), textX, 0, textW, h, juce::Justification::centredLeft, true);
}

juce::MouseCursor AuroraLookAndFeel::getMouseCursorFor(juce::Component& component)
{
    return CursorThemeManager::getInstance().resolveCursor(LookAndFeel_V4::getMouseCursorFor(component));
}

} // namespace OpenTune
