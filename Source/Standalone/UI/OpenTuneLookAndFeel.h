#pragma once

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"
#include "UIColors.h"
#include "TopBarComponent.h"

namespace OpenTune {

// Animation helper for hover effects
class GlowAnimation
{
public:
    GlowAnimation() = default;
    
    void trigger()
    {
        active_ = true;
        phase_ = 0.0f;
    }
    
    void stop()
    {
        active_ = false;
        phase_ = 0.0f;
    }
    
    void update(float deltaTime)
    {
        if (!active_) return;
        
        phase_ += deltaTime * speed_;
        if (phase_ > 1.0f)
        {
            phase_ = 0.0f;
        }
    }
    
    float getIntensity() const
    {
        if (!active_) return 0.0f;
        // Sine wave for smooth pulsing
        return 0.5f + 0.5f * std::sin(phase_ * juce::MathConstants<float>::twoPi);
    }
    
    bool isActive() const { return active_; }

private:
    bool active_ = false;
    float phase_ = 0.0f;
    static constexpr float speed_ = 3.0f; // cycles per second
};

class OpenTuneLookAndFeel : public juce::LookAndFeel_V4
{
public:
    OpenTuneLookAndFeel()
    {
        auto typeface = juce::Typeface::createSystemTypefaceFor(
            BinaryData::HONORSansCNMedium_ttf,
            BinaryData::HONORSansCNMedium_ttfSize
        );

        if (typeface != nullptr)
        {
            juce::LookAndFeel::setDefaultSansSerifTypeface(typeface);
        }
    }

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

    juce::Font getTextButtonFont(juce::TextButton& button, int height) override
    {
        // 閲嶈锛氫笉瑕佺敤鎸夐挳楂樺害鎺ㄥ瀛椾綋澶у皬锛屽惁鍒欎細鍑虹幇鈥滀竴澶т竴灏忊€濄€?        // 绾﹀畾锛氶渶瑕佺粺涓€瀛楀彿鐨勬寜閽缃?properties["fontHeight"].
        if (button.getProperties().contains("fontHeight"))
        {
            const auto v = static_cast<double>(button.getProperties()["fontHeight"]);
            return UIColors::getUIFont(static_cast<float>(v));
        }

        if (button.getProperties().contains("navFont"))
        {
            return UIColors::getUIFont(UIColors::navFontHeight);
        }

        return UIColors::getUIFont(static_cast<float>(juce::jmax(height, 16)));
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        const auto& style = UIColors::currentThemeStyle();
        float radius = style.controlRadius;

        juce::Colour base = backgroundColour;
        if (auto* tb = dynamic_cast<juce::TextButton*>(&button))
        {
            base = button.getToggleState()
                ? tb->findColour(juce::TextButton::buttonOnColourId)
                : tb->findColour(juce::TextButton::buttonColourId);
        }

        auto themeId = UIColors::currentThemeId();

        if (themeId == ThemeId::DarkBlueGrey)
        {
            drawDarkBlueGreyButton(g, button, bounds, radius, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown, base);
        }
        else
        {
            drawBlueBreezeButton(g, button, bounds, radius, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
        }

        if (!button.isEnabled())
        {
            g.setColour(UIColors::backgroundMedium.withAlpha(0.35f));
            g.fillRoundedRectangle(bounds, radius);
        }
    }

    void drawBlueBreezeButton(juce::Graphics& g, juce::Button& button, juce::Rectangle<float> bounds, float radius,
                              bool isHighlighted, bool isDown)
    {
        bool isActive = button.getToggleState() || isDown;
        
        if (isActive)
        {
            // Active: Pure White Background + Dark Text (Inverted)
            
            // Shadow for depth
            drawSoftShadow(g, bounds, radius, juce::Colour(BlueBreeze::Colors::ShadowColor).withAlpha(0.2f));
            
            g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite));
            g.fillRoundedRectangle(bounds, radius);
            
            // Inner shadow for pressed state
            if (isDown)
            {
                g.setColour(juce::Colours::black.withAlpha(0.1f));
                g.fillRoundedRectangle(bounds, radius);
            }
        }
        else
        {
            // Inactive: Transparent/Subtle Background + Dark Text
            
            if (isHighlighted)
            {
                g.setColour(juce::Colour(BlueBreeze::Colors::HoverOverlay)); // Slight white overlay
                g.fillRoundedRectangle(bounds, radius);
            }
            
            // Optional: Very subtle border
            g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder).withAlpha(0.5f));
            g.drawRoundedRectangle(bounds, radius, 1.0f);
        }
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        if (UIColors::currentThemeId() == ThemeId::BlueBreeze)
        {
            drawBlueBreezeToggleButton(g, button, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
            return;
        }
        
        juce::LookAndFeel_V4::drawToggleButton(g, button, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
    }

    void drawBlueBreezeToggleButton(juce::Graphics& g, juce::ToggleButton& button,
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

    void drawTickBox(juce::Graphics& g, juce::Component& component,
                     float x, float y, float w, float h,
                     bool ticked, bool isEnabled,
                     bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        if (UIColors::currentThemeId() == ThemeId::BlueBreeze)
        {
            juce::Rectangle<float> tickBounds(x, y, w, h);
            
            // Background
            g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite));
            g.fillRoundedRectangle(tickBounds, 3.0f);
            
            // Border
            juce::Colour border = juce::Colour(BlueBreeze::Colors::PanelBorder);
            if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
                border = juce::Colour(BlueBreeze::Colors::AccentBlue);
                
            g.setColour(border);
            g.drawRoundedRectangle(tickBounds, 3.0f, 1.0f);
            
            // Tick
            if (ticked)
            {
                g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
                juce::Path tickPath;
                tickPath.startNewSubPath(tickBounds.getX() + 3.0f, tickBounds.getCentreY());
                tickPath.lineTo(tickBounds.getCentreX(), tickBounds.getBottom() - 3.0f);
                tickPath.lineTo(tickBounds.getRight() - 3.0f, tickBounds.getY() + 3.0f);
                g.strokePath(tickPath, juce::PathStrokeType(2.0f));
            }
            return;
        }
        
        juce::LookAndFeel_V4::drawTickBox(g, component, x, y, w, h, ticked, isEnabled, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
    }


    void draw3DRaised(juce::Graphics& g, juce::Rectangle<float> bounds, float cornerSize, float bevelWidth, bool isHighlighted)
    {
        // Top and left bevel (light)
        juce::Path topBevel;
        topBevel.addRoundedRectangle(bounds, cornerSize);
        g.setColour(isHighlighted ? UIColors::bevelLight.brighter(0.2f) : UIColors::bevelLight);
        g.fillPath(topBevel);
        
        // Bottom and right shadow (dark)
        juce::Path shadow;
        shadow.addRoundedRectangle(bounds.translated(0, bevelWidth * 0.5f), cornerSize);
        g.setColour(UIColors::bevelDark.withAlpha(0.8f));
        g.fillPath(shadow);
        
        // Drop shadow
        juce::DropShadow ds;
        ds.colour = juce::Colours::black.withAlpha(0.4f);
        ds.radius = 8;
        ds.offset = { 0, 3 };
        juce::Path p;
        p.addRoundedRectangle(bounds, cornerSize);
        ds.drawForPath(g, p);
    }
    
    void draw3DRecessed(juce::Graphics& g, juce::Rectangle<float> bounds, float cornerSize, float bevelWidth)
    {
        // Inverted bevel for pressed state
        juce::Path p;
        p.addRoundedRectangle(bounds, cornerSize);
        
        // Dark top/left (shadow)
        g.setColour(UIColors::bevelDark);
        g.fillPath(p);
        
        // Light bottom/right (inner highlight)
        juce::Path inner;
        inner.addRoundedRectangle(bounds.reduced(bevelWidth), cornerSize - 1);
        g.setColour(UIColors::buttonPressed);
        g.fillPath(inner);
    }

    void drawDarkBlueGreyButton(juce::Graphics& g, juce::Button& button, juce::Rectangle<float> bounds, float radius,
                                bool isHighlighted, bool isDown, juce::Colour base)
    {
        juce::ignoreUnused(base);

        // Dark Blue-Grey锛氭竻鐖姐€佺嚎鏉℃槑蹇€?        // 璁捐鍘熷垯锛?        // - 涓嶇敤鈥滃ぇ闈㈢Н绾己璋冭壊濉厖鈥濓紝鑰屾槸鐢ㄢ€滅粏鎻忚竟 + 杞绘礂鑹测€濊〃杈炬縺娲?        // - 闃村奖浣跨敤鍐疯壊鐜闃村奖锛屽噺灏戝帤閲嶇函榛?
        const auto themeId = UIColors::currentThemeId();
        if (themeId != ThemeId::DarkBlueGrey)
        {
            // 闃插尽鎬э細鏈嚱鏁板彧涓?DarkBlueGrey 璁捐
            return;
        }

        const bool toggled = button.getToggleState();

        if (!isDown)
        {
            const auto shadowBase = juce::Colour { 0xFF050A12 };
            const float alpha = isHighlighted ? 0.34f : 0.30f;

            juce::DropShadow ds;
            ds.colour = shadowBase.withAlpha(alpha);
            ds.radius = 12;
            ds.offset = { 0, 4 };

            juce::Path p;
            p.addRoundedRectangle(bounds, radius);
            ds.drawForPath(g, p);
        }

        // 2) 鑳屾櫙濉厖锛堣交娓愬彉锛屽埗閫犱綋绉級
        juce::Colour bg;
        if (isDown)
            bg = UIColors::buttonPressed;
        else if (isHighlighted)
            bg = UIColors::buttonHover;
        else
            bg = UIColors::buttonNormal;

        if (toggled)
            bg = UIColors::backgroundLight.brighter(0.03f);

        juce::ColourGradient bgGrad(
            bg.brighter(0.03f), bounds.getX(), bounds.getY(),
            bg.darker(0.03f), bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill(bgGrad);
        g.fillRoundedRectangle(bounds, radius);

        // 3) 杞绘礂鑹诧紙鍙湪閫変腑鎬侊級
        if (toggled && !isDown)
        {
            g.setColour(UIColors::accent.withAlpha(0.10f));
            g.fillRoundedRectangle(bounds.reduced(1.0f), juce::jmax(0.0f, radius - 1.0f));
        }

        if (!isDown)
        {
            g.setColour(UIColors::textPrimary.withAlpha(0.08f));
            g.drawLine(bounds.getX() + radius, bounds.getY() + 1.0f,
                       bounds.getRight() - radius, bounds.getY() + 1.0f, 1.0f);
        }

        // 5) 鎸変笅鎬佸唴闃村奖锛堣交寰級
        if (isDown)
        {
            g.setColour(UIColors::bevelDark.withAlpha(0.26f));
            juce::Path innerShadow;
            innerShadow.addRoundedRectangle(bounds.reduced(1.0f), juce::jmax(0.0f, radius - 1.0f));
            g.strokePath(innerShadow, juce::PathStrokeType(2.0f));
        }

        // 6) 杈规
        juce::Colour border = toggled ? UIColors::accent.withAlpha(0.85f)
                                      : UIColors::panelBorder.withAlpha(isHighlighted ? 0.70f : 0.55f);
        g.setColour(border);
        g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
    }

    juce::Font getComboBoxFont(juce::ComboBox& box) override
    {
        if (box.getProperties().contains("fontHeight"))
            return UIColors::getUIFont(static_cast<float>(static_cast<double>(box.getProperties()["fontHeight"])));
        return UIColors::getUIFont(16.0f);
    }

    juce::Font getPopupMenuFont() override
    {
        return UIColors::getLabelFont(16.0f);
    }

    juce::Font getLabelFont(juce::Label&) override
    {
        return UIColors::getLabelFont(16.0f);
    }

    void drawLabel(juce::Graphics& g, juce::Label& label) override
    {
        const bool isValueField = label.isEditable()
            || dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr;
        if (!isValueField)
        {
            juce::LookAndFeel_V4::drawLabel(g, label);
            return;
        }

        const auto themeId = UIColors::currentThemeId();
        if (themeId != ThemeId::DarkBlueGrey && themeId != ThemeId::BlueBreeze)
        {
            juce::LookAndFeel_V4::drawLabel(g, label);
            return;
        }

        const auto& style = UIColors::currentThemeStyle();
        auto bounds = label.getLocalBounds().toFloat().reduced(0.5f);

        if (themeId == ThemeId::BlueBreeze)
        {
            float radius = 4.0f;
            g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite).withAlpha(0.8f));
            g.fillRoundedRectangle(bounds, radius);

            if (label.hasKeyboardFocus(true) && label.isEditable())
            {
                g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
                g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 2.0f);
            }
            else
            {
                g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder));
                g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
            }
            
            g.setColour(label.findColour(juce::Label::textColourId));
            g.setFont(label.getFont());
            g.drawFittedText(label.getText(), label.getLocalBounds().reduced(8, 2), label.getJustificationType(), 1);
            return;
        }

        // Dark Blue Grey Logic
        juce::Colour bg = label.findColour(juce::Label::backgroundColourId);
        if (bg.getAlpha() == 0)
            bg = UIColors::backgroundLight;

        juce::ColourGradient bgGrad(bg.brighter(0.04f), bounds.getX(), bounds.getY(),
                                    bg.darker(0.05f), bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill(bgGrad);
        g.fillRoundedRectangle(bounds, style.fieldRadius);

        g.setColour(UIColors::textPrimary.withAlpha(0.08f));
        g.drawLine(bounds.getX() + style.fieldRadius, bounds.getY() + 1.0f,
                   bounds.getRight() - style.fieldRadius, bounds.getY() + 1.0f, 1.0f);

        const bool focused = label.hasKeyboardFocus(true) && label.isEditable();
        g.setColour(focused ? UIColors::accent.withAlpha(0.95f) : UIColors::panelBorder.withAlpha(0.70f));
        g.drawRoundedRectangle(bounds, style.fieldRadius, focused ? style.focusRingThickness : style.strokeThin);

        g.setColour(label.findColour(juce::Label::textColourId));
        g.setFont(label.getFont());
        g.drawFittedText(label.getText(), label.getLocalBounds().reduced(8, 2), label.getJustificationType(), 1);
    }

    juce::Font getAlertWindowTitleFont() override
    {
        return juce::Font(juce::FontOptions("HONOR Sans CN", "Medium", 18.0f));
    }

    juce::Font getAlertWindowMessageFont() override
    {
        return juce::Font(juce::FontOptions("HONOR Sans CN", "Medium", 16.0f));
    }

    juce::Font getAlertWindowFont() override
    {
        return juce::Font(juce::FontOptions("HONOR Sans CN", "Medium", 16.0f));
    }

    juce::Font getSliderPopupFont(juce::Slider&) override
    {
        return UIColors::getUIFont(14.0f);
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          const juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        juce::ignoreUnused(minSliderPos, maxSliderPos);

        const auto themeId = UIColors::currentThemeId();
        const auto& themeStyle = UIColors::currentThemeStyle();
        auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height))
                          .reduced(2.0f);

        if (style == juce::Slider::LinearVertical)
        {
            drawVerticalSlider(g, bounds, sliderPos, slider, themeId, themeStyle);
        }
        else
        {
            if (themeId == ThemeId::BlueBreeze)
            {
                 // Horizontal logic similar to Vertical...
                 // Implement horizontal Blue Breeze slider
                float trackH = 4.0f;
                auto trackRect = bounds.withHeight(trackH).withY(bounds.getCentreY() - trackH/2).reduced(4, 0);
                
                // Track Background
                g.setColour(juce::Colour(BlueBreeze::Colors::KnobBody).withAlpha(0.2f));
                g.fillRoundedRectangle(trackRect, trackH/2);
                
                // Active Fill
                auto fillRect = trackRect.withWidth(sliderPos - trackRect.getX());
                g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
                g.fillRoundedRectangle(fillRect, trackH/2);
                
                // Capsule Thumb
                float thumbW = 12.0f;
                float thumbH = 24.0f;
                auto thumbRect = juce::Rectangle<float>(sliderPos - thumbW/2, bounds.getCentreY() - thumbH/2, thumbW, thumbH);
                
                drawSoftShadow(g, thumbRect, 4.0f, juce::Colour(BlueBreeze::Colors::ShadowColor).withAlpha(0.2f));
                
                g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite));
                g.fillRoundedRectangle(thumbRect, thumbW/2);
                
                if (slider.isMouseOverOrDragging())
                {
                    g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
                    g.drawRoundedRectangle(thumbRect, thumbW/2, 1.0f);
                }
            }
            else
            {
                juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
            }
        }
    }
    
    void drawVerticalSlider(juce::Graphics& g, juce::Rectangle<float> bounds, float sliderPos, 
                           juce::Slider& slider, ThemeId themeId, const ThemeStyle& themeStyle)
    {
        if (themeId == ThemeId::BlueBreeze)
        {
            drawBlueBreezeVerticalSlider(g, bounds, sliderPos, slider);
            return;
        }

        // Soothe 2 Style: Clean, Thin Vertical Track with Capsule Handle

        // 1. Background Track (Vertical Bar)
        float trackWidth = 4.0f;
        auto track = bounds.withWidth(trackWidth).withX(bounds.getCentreX() - trackWidth * 0.5f);
        float r = trackWidth * 0.5f;

        // Track (Background)
        g.setColour(UIColors::backgroundLight.darker(0.3f));
        g.fillRoundedRectangle(track, r);

        // Fill (Active part) - From bottom up to sliderPos
        // sliderPos is the Y center of the thumb.
        // We want fill from bottom of track to sliderPos.
        // Ensure sliderPos is within bounds to avoid drawing outside or negative height
        float fillTop = juce::jmax(track.getY(), sliderPos);
        float fillBottom = track.getBottom();
        
        if (fillTop < fillBottom)
        {
            juce::Rectangle<float> activeTrack(track.getX(), fillTop, track.getWidth(), fillBottom - fillTop);
            g.setColour(UIColors::accent);
            g.fillRoundedRectangle(activeTrack, r);
        }

        // 2. Thumb (Capsule/Lozenge Shape)
        // Fixed size for consistency
        float thumbW = 20.0f; 
        float thumbH = 10.0f;
        juce::Rectangle<float> thumb(bounds.getCentreX() - thumbW * 0.5f, sliderPos - thumbH * 0.5f, thumbW, thumbH);

        if (themeId == ThemeId::DarkBlueGrey)
        {
            drawDarkBlueGreySliderThumb(g, thumb, themeStyle);
        }
        else
        {
            // Default Fallback
            g.setColour(UIColors::backgroundDark.withAlpha(0.25f));
            g.fillEllipse(thumb);
            g.setColour(UIColors::panelBorder);
            g.drawEllipse(thumb, themeStyle.strokeThin);
        }
    }
    
    void drawBlueBreezeVerticalSlider(juce::Graphics& g, juce::Rectangle<float> bounds, float sliderPos, juce::Slider& slider)
    {
        float trackW = 4.0f;
        auto trackRect = bounds.withWidth(trackW).withX(bounds.getCentreX() - trackW/2).reduced(0, 4);
        
        // Track Background
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobBody).withAlpha(0.2f));
        g.fillRoundedRectangle(trackRect, trackW/2);
        
        // Active Fill
        // sliderPos is the center of the thumb.
        // Assuming slider min is at bottom.
        float fillTop = juce::jmax(trackRect.getY(), sliderPos);
        float fillBottom = trackRect.getBottom();
        
        if (fillTop < fillBottom)
        {
            juce::Rectangle<float> activeTrack(trackRect.getX(), fillTop, trackRect.getWidth(), fillBottom - fillTop);
            g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
            g.fillRoundedRectangle(activeTrack, trackW/2);
        }
        
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

    void drawDarkBlueGreySliderThumb(juce::Graphics& g, juce::Rectangle<float> thumb, const ThemeStyle& themeStyle)
    {
        // Soothe 2 Style: Soft pill-shaped handle with gentle shadow
        float r = thumb.getHeight() * 0.5f; // 浣跨敤楂樺害璁＄畻鍦嗚锛屽舰鎴愯兌鍥婂舰鐘?
        // 1. Soft Drop Shadow - 鏌斿拰鐨勫ぇ鍗婂緞闃村奖
        juce::DropShadow ds;
        ds.colour = juce::Colours::black.withAlpha(0.22f);
        ds.radius = 8;
        ds.offset = { 0, 2 };
        juce::Path shadowPath;
        shadowPath.addRoundedRectangle(thumb, r);
        ds.drawForPath(g, shadowPath);

        // 2. Handle Body - 涓庝富棰樺崗璋冪殑鏌斿拰棰滆壊
        juce::ColourGradient bodyGrad(
            UIColors::buttonNormal.brighter(0.06f), thumb.getX(), thumb.getY(),
            UIColors::buttonNormal.darker(0.04f), thumb.getX(), thumb.getBottom(), false);
        g.setGradientFill(bodyGrad);
        g.fillRoundedRectangle(thumb, r);

        g.setColour(juce::Colours::white.withAlpha(0.15f));
        g.drawLine(thumb.getX() + r, thumb.getY() + 1.5f, 
                  thumb.getRight() - r, thumb.getY() + 1.5f, 1.5f);

        g.setColour(UIColors::panelBorder.withAlpha(0.5f));
        g.drawRoundedRectangle(thumb.reduced(0.5f), r, 1.0f);

        // 5. Center Grip Lines - 涓ゆ潯缁嗙嚎浣滀负鎻℃寔鎻愮ず
        g.setColour(juce::Colours::black.withAlpha(0.15f));
        float midY = thumb.getCentreY();
        float lineLen = thumb.getWidth() * 0.3f;
        float lineX = thumb.getCentreX() - lineLen * 0.5f;
        g.drawLine(lineX, midY - 2.0f, lineX + lineLen, midY - 2.0f, 1.0f);
        g.drawLine(lineX, midY + 2.0f, lineX + lineLen, midY + 2.0f, 1.0f);
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider& slider) override
    {
        juce::ignoreUnused(slider);

        const auto themeId = UIColors::currentThemeId();
        const auto& themeStyle = UIColors::currentThemeStyle();
        auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height))
                          .reduced(10.0f);
        auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        auto cx = bounds.getCentreX();
        auto cy = bounds.getCentreY();
        auto angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        juce::Rectangle<float> knob(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

        if (themeId == ThemeId::DarkBlueGrey)
        {
            drawDarkBlueGreyKnob(g, bounds, cx, cy, radius, angle, rotaryStartAngle, rotaryEndAngle, themeStyle);
        }
        else if (themeId == ThemeId::BlueBreeze)
        {
            drawBlueBreezeKnob(g, bounds, cx, cy, radius, angle, rotaryStartAngle, rotaryEndAngle, slider);
        }
        else
        {
            g.setColour(UIColors::knobBody);
            g.fillEllipse(knob);

            g.setColour(UIColors::panelBorder);
            g.drawEllipse(knob, themeStyle.strokeThin);

            juce::Path arc;
            arc.addCentredArc(cx, cy, radius * 0.82f, radius * 0.82f, 0.0f, rotaryStartAngle, angle, true);
            g.setColour(UIColors::knobIndicator);
            g.strokePath(arc, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            juce::Line<float> needle(cx, cy, cx + std::cos(angle) * radius * 0.72f, cy + std::sin(angle) * radius * 0.72f);
            g.setColour(UIColors::textPrimary.withAlpha(0.9f));
            g.drawLine(needle, 2.0f);
        }
    }
    
    void drawBlueBreezeKnob(juce::Graphics& g, juce::Rectangle<float> bounds, float cx, float cy, float radius,
                            float angle, float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
    {
        auto trackRadius = radius * 0.85f;

        // 1. Shadow (Soft, diffused)
        drawSoftShadow(g, 
                       juce::Rectangle<float>(cx - trackRadius, cy - trackRadius, trackRadius * 2, trackRadius * 2), 
                       trackRadius, 
                       juce::Colour(BlueBreeze::Colors::ShadowColor).withAlpha(BlueBreeze::Style::ShadowAlpha));

        // 2. Knob Body (Deep Charcoal)
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobBody));
        g.fillEllipse(cx - trackRadius, cy - trackRadius, trackRadius * 2, trackRadius * 2);

        // 3. Value Arc (Thin, Precise)
        
        // Track background (Very subtle)
        juce::Path bgArc;
        bgArc.addCentredArc(cx, cy, trackRadius * 0.85f, trackRadius * 0.85f, 
                            0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobTrack).withAlpha(0.1f));
        g.strokePath(bgArc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Value Indicator
        juce::Path valueArc;
        valueArc.addCentredArc(cx, cy, trackRadius * 0.85f, trackRadius * 0.85f, 
                               0.0f, rotaryStartAngle, angle, true);
        
        // Outer Glow for Value
        if (slider.isMouseOverOrDragging())
        {
            g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue).withAlpha(0.3f));
            g.strokePath(valueArc, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour(juce::Colour(BlueBreeze::Colors::KnobIndicator));
        g.strokePath(valueArc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // 4. Center Value Text (Soothe2 style)
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobIndicator));
        g.setFont(UIColors::getUIFont(12.0f)); // Small, geometric font
        
        // Format value text
        juce::String text;
        if (std::abs(slider.getValue()) < 10.0) text = juce::String(slider.getValue(), 1);
        else text = juce::String((int)slider.getValue());
        
        g.drawText(text, bounds, juce::Justification::centred, false);
    }

        void drawDarkBlueGreyKnob(juce::Graphics& g, juce::Rectangle<float> bounds, float cx, float cy, float radius,
                              float angle, float rotaryStartAngle, float rotaryEndAngle, const ThemeStyle& themeStyle)
    {
        juce::ignoreUnused(themeStyle, cx, cy);

        auto center = bounds.getCentre();
        const float knobRadius = radius * 0.80f;
        const float ringRadius = knobRadius * 1.22f;

        {
            juce::DropShadow ds;
            ds.colour = juce::Colour(0xFF050A12).withAlpha(0.40f);
            ds.radius = 18;
            ds.offset = { 0, 6 };
            juce::Path p;
            p.addEllipse(center.x - knobRadius, center.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
            ds.drawForPath(g, p);
        }

        juce::Path trackPath;
        trackPath.addCentredArc(center.x, center.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(UIColors::backgroundDark.brighter(0.10f));
        g.strokePath(trackPath, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path valuePath;
        valuePath.addCentredArc(center.x, center.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, angle, true);
        g.setColour(UIColors::accent.withAlpha(0.95f));
        g.strokePath(valuePath, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        {
            juce::ColourGradient rimGrad(UIColors::bevelLight.withAlpha(0.45f), center.x, center.y - knobRadius,
                                         UIColors::bevelDark.withAlpha(0.55f), center.x, center.y + knobRadius, false);
            g.setGradientFill(rimGrad);
            g.fillEllipse(center.x - knobRadius, center.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
        }

        const float inner = knobRadius * 0.90f;
        {
            juce::ColourGradient bodyGrad(UIColors::knobBody.brighter(0.10f), center.x - inner * 0.30f, center.y - inner * 0.35f,
                                          UIColors::knobBody.darker(0.20f), center.x + inner * 0.35f, center.y + inner * 0.40f, false);
            g.setGradientFill(bodyGrad);
            g.fillEllipse(center.x - inner, center.y - inner, inner * 2.0f, inner * 2.0f);
        }

        {
            juce::Path hi;
            hi.addPieSegment(center.x - inner, center.y - inner, inner * 2.0f, inner * 2.0f,
                             juce::MathConstants<float>::pi * 1.10f,
                             juce::MathConstants<float>::pi * 1.90f, 0.78f);
            g.setColour(UIColors::textPrimary.withAlpha(0.10f));
            g.fillPath(hi);
        }

        const float cap = inner * 0.22f;
        juce::ColourGradient capGrad(UIColors::backgroundLight.brighter(0.15f), center.x, center.y - cap,
                                     UIColors::backgroundLight.darker(0.18f), center.x, center.y + cap, false);
        g.setGradientFill(capGrad);
        g.fillEllipse(center.x - cap, center.y - cap, cap * 2.0f, cap * 2.0f);
        g.setColour(UIColors::panelBorder.withAlpha(0.75f));
        g.drawEllipse(center.x - cap, center.y - cap, cap * 2.0f, cap * 2.0f, 1.0f);

        const float pinLen = inner * 0.68f;
        const float pinW = 3.0f;
        const float px = center.x + std::cos(angle) * pinLen;
        const float py = center.y + std::sin(angle) * pinLen;
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.drawLine(center.x + 1.0f, center.y + 1.0f, px + 1.0f, py + 1.0f, pinW);
        g.setColour(UIColors::knobIndicator);
        g.drawLine(center.x, center.y, px, py, pinW);

        const float tip = 3.6f;
        g.setColour(UIColors::knobIndicator.withAlpha(0.95f));
        g.fillEllipse(px - tip, py - tip, tip * 2.0f, tip * 2.0f);
    }

    void drawMenuBarBackground(juce::Graphics& g, int width, int height,
                               bool isMouseOverBar, juce::MenuBarComponent& menuBar) override
    {
        juce::ignoreUnused(isMouseOverBar, menuBar);

        const auto themeId = UIColors::currentThemeId();
        const auto& style = UIColors::currentThemeStyle();
        auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));

        if (menuBar.findParentComponentOfClass<TopBarComponent>() != nullptr)
            return;

        if (themeId == ThemeId::DarkBlueGrey)
        {
            juce::ColourGradient grad(UIColors::gradientTop.brighter(0.10f), 0.0f, 0.0f,
                                      UIColors::gradientBottom.darker(0.10f), 0.0f, bounds.getBottom(), false);
            g.setGradientFill(grad);
            g.fillAll();
        }
        else
        {
            g.fillAll(UIColors::backgroundMedium);
        }

        auto borderAlpha = themeId == ThemeId::DarkBlueGrey ? 0.55f : 0.9f;
        g.setColour(UIColors::panelBorder.withAlpha(borderAlpha));
        g.drawLine(0.0f, bounds.getBottom(), bounds.getRight(), bounds.getBottom(), style.strokeThin);

    }

    void drawMenuBarItem(juce::Graphics& g, int width, int height,
                         int itemIndex, const juce::String& itemText,
                         bool isMouseOverItem, bool isMenuOpen,
                         bool isMouseOverBar, juce::MenuBarComponent& menuBar) override
    {
        juce::ignoreUnused(itemIndex, isMouseOverBar, menuBar);

        const auto themeId = UIColors::currentThemeId();
        const auto& style = UIColors::currentThemeStyle();
        auto b = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(2.0f, 4.0f);

        bool hot = (isMenuOpen || isMouseOverItem);
        
        if (themeId == ThemeId::DarkBlueGrey)
        {
            if (hot)
            {
                g.setColour(UIColors::textPrimary.withAlpha(0.06f));
                g.fillRoundedRectangle(b, style.controlRadius);
                g.setColour(UIColors::panelBorder.withAlpha(0.35f));
                g.drawRoundedRectangle(b, style.controlRadius, style.strokeThin);
            }
        }

        g.setColour(hot ? UIColors::textPrimary : UIColors::textPrimary.withAlpha(0.9f));

        g.setFont(UIColors::getUIFont(UIColors::navFontHeight));
        g.drawFittedText(itemText, 0, 0, width, height, juce::Justification::centred, 1);
    }

    void fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                  juce::TextEditor& textEditor) override
    {
        if (dynamic_cast<juce::AlertWindow*> (textEditor.getParentComponent()) != nullptr)
        {
            g.setColour(textEditor.findColour(juce::TextEditor::backgroundColourId));
            g.fillRect(0, 0, width, height);
        }
        else
        {
            const auto& style = UIColors::currentThemeStyle();
            const auto themeId = UIColors::currentThemeId();
            auto bg = textEditor.findColour(juce::TextEditor::backgroundColourId);

            if (themeId == ThemeId::DarkBlueGrey)
            {
                juce::ColourGradient grad(bg.brighter(0.05f), 0.0f, 0.0f, bg.darker(0.06f), 0.0f, static_cast<float>(height), false);
                g.setGradientFill(grad);
                g.fillRoundedRectangle(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), style.fieldRadius);
                g.setColour(UIColors::textPrimary.withAlpha(0.10f));
                g.drawLine(2.0f, 1.0f, static_cast<float>(width) - 2.0f, 1.0f, style.strokeThin);
            }
            else
            {
                // BlueBreeze - flat look
                g.setColour(bg);
                g.fillRoundedRectangle(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), style.fieldRadius);
            }
        }
    }

    void drawTextEditorOutline(juce::Graphics& g, int width, int height,
                               juce::TextEditor& textEditor) override
    {
        if (dynamic_cast<juce::AlertWindow*> (textEditor.getParentComponent()) == nullptr)
        {
            if (textEditor.isEnabled())
            {
                const auto& style = UIColors::currentThemeStyle();
                const auto themeId = UIColors::currentThemeId();
                
                if (textEditor.hasKeyboardFocus(true) && !textEditor.isReadOnly())
                {
                    if (themeId == ThemeId::BlueBreeze)
                    {
                        g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
                        g.drawRoundedRectangle(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f, style.fieldRadius, 2.0f);
                    }
                    else
                    {
                        g.setColour(textEditor.findColour(juce::TextEditor::focusedOutlineColourId));
                        g.drawRoundedRectangle(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f, style.fieldRadius, style.focusRingThickness);
                    }
                }
                else
                {
                    auto border = textEditor.findColour(juce::TextEditor::outlineColourId);
                    
                    if (themeId == ThemeId::BlueBreeze)
                    {
                        if (textEditor.isMouseOver())
                            border = juce::Colour(BlueBreeze::Colors::AccentBlue).withAlpha(0.6f);
                        else
                            border = juce::Colour(BlueBreeze::Colors::PanelBorder);
                            
                        g.setColour(border);
                        g.drawRoundedRectangle(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f, style.fieldRadius, 1.0f);
                    }
                    else
                    {
                        if (themeId == ThemeId::DarkBlueGrey)
                            border = UIColors::panelBorder.withAlpha(0.72f);
                        g.setColour(border);
                        g.drawRoundedRectangle(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f, style.fieldRadius, style.strokeThin);
                    }
                }
            }
        }
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox& box) override
    {
        juce::ignoreUnused(isButtonDown, buttonX, buttonY, buttonW, buttonH);

        const auto themeId = UIColors::currentThemeId();
        const auto& style = UIColors::currentThemeStyle();
        auto cornerSize = box.findParentComponentOfClass<juce::GroupComponent>() != nullptr ? 0.0f : style.fieldRadius;
        juce::Rectangle<float> boxBounds(0.0f, 0.0f, (float)width, (float)height);

        if (themeId == ThemeId::BlueBreeze)
        {
            // Pill/Rounded shape
            float radius = 4.0f;
            bool isActive = isButtonDown || box.isPopupActive();
            bool isHover = box.isMouseOver(true);

            if (isActive)
            {
                drawSoftShadow(g, boxBounds, radius, juce::Colour(BlueBreeze::Colors::ShadowColor).withAlpha(0.2f));
                g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite));
                g.fillRoundedRectangle(boxBounds, radius);
            }
            else
            {
                g.setColour(juce::Colour(BlueBreeze::Colors::ActiveWhite).withAlpha(0.6f));
                g.fillRoundedRectangle(boxBounds, radius);
                
                if (isHover)
                {
                    g.setColour(juce::Colour(BlueBreeze::Colors::HoverOverlay));
                    g.fillRoundedRectangle(boxBounds, radius);
                }
            }

            g.setColour(isActive ? juce::Colour(BlueBreeze::Colors::AccentBlue) : juce::Colour(BlueBreeze::Colors::PanelBorder));
            g.drawRoundedRectangle(boxBounds.reduced(0.5f), radius, 1.0f);
            
            if (!box.getProperties().contains("noArrow"))
            {
                auto arrowZone = boxBounds.removeFromRight(boxBounds.getHeight()).reduced(boxBounds.getHeight() * 0.35f);
                juce::Path path;
                path.startNewSubPath(arrowZone.getX(), arrowZone.getY());
                path.lineTo(arrowZone.getCentreX(), arrowZone.getBottom());
                path.lineTo(arrowZone.getRight(), arrowZone.getY());
                
                g.setColour(juce::Colour(BlueBreeze::Colors::TextDim));
                g.strokePath(path, juce::PathStrokeType(1.5f));
            }
            return;
        }

        auto bg = box.findColour(juce::ComboBox::backgroundColourId);
        if (themeId == ThemeId::DarkBlueGrey)
        {
            g.setColour(bg);
            g.fillRoundedRectangle(boxBounds, cornerSize);
        }
        else
        {
            g.setColour(bg);
            g.fillRoundedRectangle(boxBounds, cornerSize);
        }

        auto outline = box.findColour(juce::ComboBox::outlineColourId);
        if (themeId == ThemeId::DarkBlueGrey)
            outline = UIColors::panelBorder.withAlpha(0.74f);
        g.setColour(outline);
        g.drawRoundedRectangle(boxBounds.reduced(0.5f, 0.5f), cornerSize, style.strokeThin);

        if (box.getProperties().contains("noArrow")) return;

        juce::Rectangle<int> arrowZone(width - 30, 0, 20, height);
        juce::Path path;
        path.startNewSubPath(static_cast<float>(arrowZone.getX() + 3), static_cast<float>(arrowZone.getCentreY() - 2));
        path.lineTo(static_cast<float>(arrowZone.getCentreX()), static_cast<float>(arrowZone.getCentreY() + 3));
        path.lineTo(static_cast<float>(arrowZone.getRight() - 3), static_cast<float>(arrowZone.getCentreY() - 2));

        g.setColour(box.findColour(juce::ComboBox::arrowColourId).withAlpha((box.isEnabled() ? 0.9f : 0.2f)));
        g.strokePath(path, juce::PathStrokeType(2.0f));
    }

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override
    {
        const auto themeId = UIColors::currentThemeId();
        if (themeId == ThemeId::BlueBreeze)
        {
            g.fillAll(juce::Colour(BlueBreeze::Colors::ActiveWhite).withAlpha(0.98f));
            g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder));
            g.drawRect(0, 0, width, height);
            return;
        }
        
        juce::LookAndFeel_V4::drawPopupMenuBackground(g, width, height);
    }

    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText,
                           const juce::Drawable* icon, const juce::Colour* textColour) override
    {
        const auto themeId = UIColors::currentThemeId();
        if (themeId == ThemeId::BlueBreeze)
        {
            if (isSeparator)
            {
                auto r = area.reduced(5, 0);
                r.removeFromTop(juce::roundToInt(((float)r.getHeight() * 0.5f) - 0.5f));
                g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder).withAlpha(0.5f));
                g.fillRect(r.removeFromTop(1));
                return;
            }

            auto textCol = (textColour != nullptr ? *textColour : juce::Colour(BlueBreeze::Colors::TextDark));
            
            if (isHighlighted)
            {
                g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue).withAlpha(0.1f));
                g.fillRect(area);
                g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
                textCol = juce::Colour(BlueBreeze::Colors::AccentBlue);
            }

            g.setColour(textCol);
            g.setFont(UIColors::getUIFont(15.0f));
            
            auto r = area.reduced(1);
            r.removeFromLeft(r.getHeight());
            
            g.drawFittedText(text, r, juce::Justification::centredLeft, 1);
            
            if (isTicked)
            {
                auto tickArea = r.removeFromRight(20);
                auto tickCenter = tickArea.getCentre().toFloat();
                float tickRadius = static_cast<float>(tickArea.getHeight()) * 0.2f;
                
                g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
                g.fillEllipse(tickCenter.x - tickRadius, tickCenter.y - tickRadius, 
                              tickRadius * 2.0f, tickRadius * 2.0f);
            }
            
            if (shortcutKeyText.isNotEmpty())
            {
                g.setColour(textCol.withAlpha(0.6f));
                g.drawText(shortcutKeyText, r, juce::Justification::centredRight, true);
            }
            return;
        }

        juce::LookAndFeel_V4::drawPopupMenuItem(g, area, isSeparator, isActive, isHighlighted, isTicked, hasSubMenu, text, shortcutKeyText, icon, textColour);
    }

    juce::Label* createComboBoxTextBox(juce::ComboBox& box) override
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

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
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

    void drawDocumentWindowTitleBar(juce::DocumentWindow& window, juce::Graphics& g,
                                    int w, int h, int titleSpaceX, int titleSpaceW,
                                    const juce::Image* icon, bool drawTitleTextOnLeft) override
    {
        juce::ignoreUnused(icon, drawTitleTextOnLeft);

        const auto themeId = UIColors::currentThemeId();
        const auto& style = UIColors::currentThemeStyle();

        auto bounds = juce::Rectangle<int>(0, 0, w, h).toFloat();
        auto bg = window.findColour(juce::DocumentWindow::backgroundColourId);
        auto titleBarBg = bg;

        if (themeId == ThemeId::DarkBlueGrey)
        {
            auto base = juce::Colour { 0xFF8FA3B5 };
            titleBarBg = base.darker(0.15f);
        }

        g.setColour(titleBarBg);
        g.fillRect(bounds);

        g.setColour(UIColors::panelBorder.withAlpha(themeId == ThemeId::DarkBlueGrey ? 0.55f : 0.9f));
        g.drawLine(bounds.getX(), bounds.getBottom() - 1.0f, bounds.getRight(), bounds.getBottom() - 1.0f, style.strokeThin);

        auto titleArea = juce::Rectangle<int>(titleSpaceX, 0, titleSpaceW, h);
        auto titleText = titleBarBg.getPerceivedBrightness() < 0.5f
            ? juce::Colours::white
            : UIColors::textPrimary;

        g.setColour(titleText.withAlpha(0.92f));
        g.setFont(UIColors::getHeaderFont(14.0f));
        g.drawFittedText(window.getName(), titleArea.reduced(6, 0), juce::Justification::centredLeft, 1);
    }

    // ============================================================================
    // 婊氬姩鏉¤嚜瀹氫箟缁樺埗 - 鐏拌壊纾ㄧ爞璐ㄦ劅
    // ============================================================================
    
    int getDefaultScrollbarWidth() override
    {
        return 15; // 15px 瀹藉害
    }
    
    void drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar,
                       int x, int y, int width, int height,
                       bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                       bool isMouseOver, bool isMouseDown) override
    {
        const auto themeId = UIColors::currentThemeId();
        auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                             static_cast<float>(width), static_cast<float>(height));
        
        // 鏍规嵁涓婚閫夋嫨鐏拌壊閰嶈壊鏂规
        juce::Colour trackBg, thumbBg, thumbHover, thumbPressed, highlight;
        
        if (themeId == ThemeId::BlueBreeze)
        {
            // Blue Breeze 涓婚 - 鏇存祬鐨勭伆鑹茬郴
            trackBg = juce::Colour(0xFFB0C0CC);
            thumbBg = juce::Colour(0xFF8CA2B0);
            thumbHover = juce::Colour(0xFFA0B0B8);
            thumbPressed = juce::Colour(0xFF7A8F9E);
            highlight = juce::Colour(0xFFC6D4DD);
        }
        else if (themeId == ThemeId::DarkBlueGrey)
        {
            // DarkBlueGrey 涓婚 - 娴呯伆鑹茶皟
            trackBg = UIColors::backgroundLight.darker(0.15f);
            thumbBg = UIColors::backgroundLight.brighter(0.1f);  // 鏇存祬鐨勮儗鏅壊
            thumbHover = UIColors::textPrimary;
            thumbPressed = UIColors::panelBorder;
            highlight = UIColors::textPrimary;
        }
        else
        {
            // 榛樿 - 浣跨敤鏇存祬鐨?BlueBreeze 椋庢牸
            trackBg = juce::Colour(0xFFB0C0CC);
            thumbBg = juce::Colour(0xFF8CA2B0);
            thumbHover = juce::Colour(0xFFA0B0B8);
            thumbPressed = juce::Colour(0xFF7A8F9E);
            highlight = juce::Colour(0xFFC6D4DD);
        }
        
        // 涓嶇粯鍒惰建閬撹儗鏅?- 閫忔槑鑳屾櫙璁╂粦鍧楃洿鎺ユ诞鍔ㄥ湪鍐呭涔嬩笂
        juce::ignoreUnused(trackBg);
        
        // 缁樺埗婊戝潡 (thumb) - 澶у渾瑙?+ 纾ㄧ爞璐ㄦ劅
        float cornerRadius = 5.0f; // 澶ц搴﹀渾瑙掞紝寰井鍦嗘鼎
        juce::Rectangle<float> thumbBounds;
        
        if (isScrollbarVertical)
        {
            thumbBounds = juce::Rectangle<float>(
                bounds.getX() + 2.0f,
                bounds.getY() + static_cast<float>(thumbStartPosition),
                bounds.getWidth() - 4.0f,
                static_cast<float>(thumbSize));
        }
        else
        {
            thumbBounds = juce::Rectangle<float>(
                bounds.getX() + static_cast<float>(thumbStartPosition),
                bounds.getY() + 2.0f,
                static_cast<float>(thumbSize),
                bounds.getHeight() - 4.0f);
        }
        
        if (isScrollbarVertical)
            thumbBounds.setHeight(juce::jmax(thumbBounds.getHeight(), 20.0f));
        else
            thumbBounds.setWidth(juce::jmax(thumbBounds.getWidth(), 20.0f));
        
        // 閫夋嫨褰撳墠鐘舵€佺殑棰滆壊
        juce::Colour currentThumbColor = thumbBg;
        if (isMouseDown)
            currentThumbColor = thumbPressed;
        else if (isMouseOver)
            currentThumbColor = thumbHover;
        
        // 婊戝潡闃村奖 - 鏌斿拰鐨勭┖姘旀劅
        juce::DropShadow thumbShadow;
        thumbShadow.colour = juce::Colours::black.withAlpha(0.15f);
        thumbShadow.radius = 6;
        thumbShadow.offset = { 0, 1 };
        juce::Path thumbPath;
        thumbPath.addRoundedRectangle(thumbBounds, cornerRadius);
        thumbShadow.drawForPath(g, thumbPath);
        
        juce::ColourGradient thumbGrad(
            currentThumbColor.brighter(0.08f),
            isScrollbarVertical ? thumbBounds.getX() : thumbBounds.getCentreX(),
            isScrollbarVertical ? thumbBounds.getCentreY() : thumbBounds.getY(),
            currentThumbColor.darker(0.06f),
            isScrollbarVertical ? thumbBounds.getRight() : thumbBounds.getCentreX(),
            isScrollbarVertical ? thumbBounds.getCentreY() : thumbBounds.getBottom(),
            !isScrollbarVertical);
        g.setGradientFill(thumbGrad);
        g.fillRoundedRectangle(thumbBounds, cornerRadius);
        
        // 婊戝潡椤堕儴/宸︿晶楂樺厜绾?- 鏅惰幑鍓旈€忔劅
        g.setColour(highlight.withAlpha(0.25f));
        if (isScrollbarVertical)
        {
            g.drawLine(thumbBounds.getX() + cornerRadius, thumbBounds.getY() + 1.0f,
                       thumbBounds.getRight() - cornerRadius, thumbBounds.getY() + 1.0f, 1.5f);
        }
        else
        {
            g.drawLine(thumbBounds.getX() + 1.0f, thumbBounds.getY() + cornerRadius,
                       thumbBounds.getX() + 1.0f, thumbBounds.getBottom() - cornerRadius, 1.5f);
        }
        
        // 鎮仠鏃剁殑鏌斿拰鍙戝厜鏁堟灉
        if (isMouseOver && !isMouseDown)
        {
            float glowAlpha = 0.12f;
            g.setColour(highlight.withAlpha(glowAlpha));
            g.drawRoundedRectangle(thumbBounds.reduced(0.5f), cornerRadius, 1.5f);
        }
    }
};

} // namespace OpenTune

