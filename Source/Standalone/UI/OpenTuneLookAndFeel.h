#pragma once

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UIColors.h"
#include "UiAssets.h"
#include "TopBarComponent.h"
#include "../../Utils/CursorTheme.h"

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
        auto typeface = UiAssets::createHonorSansTypeface();

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

    static void drawBlueBreezeSurface(juce::Graphics& g,
                                      juce::Rectangle<float> bounds,
                                      float radius,
                                      bool highlighted,
                                      bool down,
                                      bool active,
                                      const juce::Path* shapeOverride = nullptr)
    {
        juce::Path shape;
        if (shapeOverride != nullptr)
            shape = *shapeOverride;
        else
            shape.addRoundedRectangle(bounds, radius);

        const bool isPressed = down;
        const bool isActive = active || isPressed;
        const bool isHovered = highlighted && !isPressed;

        const auto shadowAlpha = isPressed ? 0.05f : (isActive || isHovered ? 0.10f : 0.08f);
        juce::DropShadow softShadow(juce::Colour(BlueBreeze::Colors::ControlShadow).withAlpha(shadowAlpha),
                                    isPressed ? 7 : 12,
                                    isPressed ? juce::Point<int> { 0, 1 } : juce::Point<int> { 0, 3 });
        softShadow.drawForPath(g, shape);

        if (isActive || isHovered)
        {
            juce::DropShadow accentGlow(juce::Colour(BlueBreeze::Colors::AccentBlue).withAlpha(isActive ? 0.08f : 0.04f),
                                        isActive ? 10 : 8,
                                        {});
            accentGlow.drawForPath(g, shape);

            juce::DropShadow responseHalo(juce::Colour(BlueBreeze::Colors::SourceLight).withAlpha(isActive ? 0.10f : 0.05f),
                                          isActive ? 12 : 9,
                                          { 0, 1 });
            responseHalo.drawForPath(g, shape);
        }

        const auto top = (isActive ? juce::Colour(BlueBreeze::Colors::ControlHover)
                                   : juce::Colour(BlueBreeze::Colors::ControlTop))
                             .interpolatedWith(juce::Colour(BlueBreeze::Colors::SourceLight), isHovered ? 0.07f : 0.04f);
        const auto mid = (isHovered ? juce::Colour(BlueBreeze::Colors::ControlHover)
                                    : juce::Colour(BlueBreeze::Colors::PanelTop))
                             .interpolatedWith(juce::Colour(BlueBreeze::Colors::TrayInset), isPressed ? 0.05f : 0.015f)
                             .interpolatedWith(juce::Colour(BlueBreeze::Colors::CanvasTop), isActive ? 0.10f : 0.05f);
        const auto bottom = (isPressed ? juce::Colour(BlueBreeze::Colors::ControlPressed)
                                       : juce::Colour(BlueBreeze::Colors::ControlBottom))
                                .interpolatedWith(juce::Colour(BlueBreeze::Colors::PanelInset), isPressed ? 0.08f : 0.03f)
                                .interpolatedWith(juce::Colour(BlueBreeze::Colors::PanelBottom), isPressed ? 0.12f : 0.06f);

        juce::ColourGradient body(top, bounds.getX(), bounds.getY(),
                                  bottom, bounds.getRight(), bounds.getBottom(), false);
        body.addColour(0.38, top.interpolatedWith(mid, 0.42f));
        body.addColour(0.68, mid);
        g.setGradientFill(body);
        g.fillPath(shape);

        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(shape);

            juce::ColourGradient light(juce::Colour(BlueBreeze::Colors::SourceLight).withAlpha(isPressed ? 0.06f : 0.12f),
                                       bounds.getX() + bounds.getWidth() * 0.14f,
                                       bounds.getY() + bounds.getHeight() * 0.10f,
                                       juce::Colours::transparentWhite,
                                       bounds.getRight(),
                                       bounds.getBottom(),
                                       true);
            g.setGradientFill(light);
            g.fillRect(bounds);

            auto topBand = bounds.withHeight(bounds.getHeight() * 0.46f);
            juce::ColourGradient glassSheen(juce::Colour(BlueBreeze::Colors::SourceLight).withAlpha(isActive ? 0.15f : 0.10f),
                                            topBand.getX(),
                                            topBand.getY(),
                                            juce::Colours::transparentWhite,
                                            topBand.getX(),
                                            topBand.getBottom(),
                                            false);
            g.setGradientFill(glassSheen);
            g.fillRect(topBand);

            juce::ColourGradient faceLift(juce::Colour(BlueBreeze::Colors::CanvasTop).withAlpha(isActive ? 0.10f : 0.06f),
                                          bounds.getX() + bounds.getWidth() * 0.10f,
                                          bounds.getY() + bounds.getHeight() * 0.08f,
                                          juce::Colours::transparentWhite,
                                          bounds.getCentreX(),
                                          bounds.getY() + bounds.getHeight() * 0.42f,
                                          true);
            g.setGradientFill(faceLift);
            g.fillRect(bounds);

            auto lowerBand = bounds.withTop(bounds.getY() + bounds.getHeight() * 0.56f);
            juce::ColourGradient lowerShade(juce::Colours::transparentBlack,
                                            lowerBand.getX(),
                                            lowerBand.getY(),
                                            juce::Colour(BlueBreeze::Colors::PanelInset).withAlpha(isPressed ? 0.14f : 0.09f),
                                            lowerBand.getX(),
                                            lowerBand.getBottom(),
                                            false);
            g.setGradientFill(lowerShade);
            g.fillRect(lowerBand);

            if (isActive || isHovered)
            {
                juce::ColourGradient response(juce::Colour(BlueBreeze::Colors::AccentBlue).withAlpha(isActive ? 0.08f : 0.04f),
                                              bounds.getCentreX(),
                                              bounds.getY(),
                                              juce::Colours::transparentBlack,
                                              bounds.getRight(),
                                              bounds.getBottom(),
                                              true);
                g.setGradientFill(response);
                g.fillRect(bounds);
            }

            g.setColour(juce::Colours::white.withAlpha(isPressed ? 0.06f : 0.14f));
            g.drawLine(bounds.getX() + radius * 0.75f, bounds.getY() + 1.0f,
                       bounds.getRight() - radius * 0.75f, bounds.getY() + 1.0f, 1.0f);
        }

        const auto border = isActive
            ? juce::Colour(BlueBreeze::Colors::AccentBlue).withAlpha(0.56f)
            : juce::Colour(BlueBreeze::Colors::PanelBorder).withAlpha(isHovered ? 0.58f : 0.40f);
        g.setColour(border);
        g.strokePath(shape, juce::PathStrokeType(isActive ? 1.35f : 1.0f));

        g.setColour(juce::Colour(BlueBreeze::Colors::SourceLight).withAlpha(isActive ? 0.14f : (isHovered ? 0.10f : 0.07f)));
        g.strokePath(shape, juce::PathStrokeType(0.8f));

        if (isPressed)
        {
            g.setColour(juce::Colour(BlueBreeze::Colors::PanelInset).withAlpha(0.18f));
            g.strokePath(shape, juce::PathStrokeType(2.0f));
        }
    }

    static void drawOverdoseSurface(juce::Graphics& g,
                                    juce::Rectangle<float> bounds,
                                    float radius,
                                    bool highlighted,
                                    bool down,
                                    bool active,
                                    const juce::Path* shapeOverride = nullptr)
    {
        juce::Path shape;
        if (shapeOverride != nullptr)
            shape = *shapeOverride;
        else
            shape.addRoundedRectangle(bounds, radius);

        const bool isPressed = down;
        const bool isActive = active || isPressed;
        const bool isHovered = highlighted && !isPressed;

        // 阴影（更柔和）
        juce::DropShadow softShadow;
        softShadow.colour = juce::Colour(Overdose::Colors::SoftShadow).withAlpha(isPressed ? 0.10f : 0.14f);
        softShadow.radius = isPressed ? 5 : 8;
        softShadow.offset = { 0, isPressed ? 1 : 2 };
        softShadow.drawForPath(g, shape);

        // 激活时粉色辉光
        if (isActive || isHovered)
        {
            juce::DropShadow accentGlow;
            accentGlow.colour = juce::Colour(Overdose::Colors::PinkGlowSoft).withAlpha(isActive ? 0.20f : 0.12f);
            accentGlow.radius = isActive ? 10 : 6;
            accentGlow.offset = {};
            accentGlow.drawForPath(g, shape);
        }

        // 主体渐变（更通透的白→淡紫玻璃质感）
        juce::ColourGradient body(juce::Colour(0xFFFFFCFF).withAlpha(isActive ? 0.95f : 0.90f),
                                  bounds.getX(),
                                  bounds.getY(),
                                  juce::Colour(0xFFF0E8F8).withAlpha(isPressed ? 0.95f : 0.88f),
                                  bounds.getX(),
                                  bounds.getBottom(),
                                  false);
        body.addColour(0.35f, juce::Colour(0xFFF8F2FC).withAlpha(isActive ? 0.93f : 0.89f));
        g.setGradientFill(body);
        g.fillPath(shape);

        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(shape);

            // 顶部玻璃高光（覆盖更大区域，更强反光）
            auto topBand = bounds.withHeight(bounds.getHeight() * 0.45f);
            juce::ColourGradient sheen(juce::Colour(0xFFFFFFFF).withAlpha(isActive ? 0.75f : 0.60f),
                                       topBand.getCentreX(), topBand.getY(),
                                       juce::Colours::transparentWhite,
                                       topBand.getCentreX(), topBand.getBottom(),
                                       false);
            g.setGradientFill(sheen);
            g.fillRect(topBand);

            // 底部内阴影（更柔和）
            auto lowerBand = bounds.withTop(bounds.getY() + bounds.getHeight() * 0.50f);
            juce::ColourGradient lowerShade(juce::Colours::transparentBlack,
                                            lowerBand.getX(),
                                            lowerBand.getY(),
                                            juce::Colour(0xFF6050A0).withAlpha(isPressed ? 0.20f : 0.15f),
                                            lowerBand.getX(),
                                            lowerBand.getBottom(),
                                            false);
            g.setGradientFill(lowerShade);
            g.fillRect(lowerBand);

            // 顶部细高光线（更亮更明显）
            g.setColour(juce::Colours::white.withAlpha(isPressed ? 0.15f : 0.55f));
            g.drawLine(bounds.getX() + radius * 0.75f,
                       bounds.getY() + 1.0f,
                       bounds.getRight() - radius * 0.75f,
                       bounds.getY() + 1.0f,
                       1.3f);
        }

        // 边框（粉色，更精致）
        const auto border = isActive
            ? juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.50f)
            : juce::Colour(0xFFC0B8D0).withAlpha(isHovered ? 0.42f : 0.30f);
        g.setColour(border);
        g.strokePath(shape, juce::PathStrokeType(0.7f));
    }

    juce::MouseCursor getMouseCursorFor(juce::Component& component) override
    {
        return CursorThemeManager::getInstance().resolveCursor(LookAndFeel_V4::getMouseCursorFor(component));
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
        else if (themeId == ThemeId::Overdose)
        {
            drawOverdoseButton(g, button, bounds, radius, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
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
        drawBlueBreezeSurface(g, bounds, radius, isHighlighted, isDown, button.getToggleState() || isDown);
    }

    void drawOverdoseButton(juce::Graphics& g, juce::Button& button, juce::Rectangle<float> bounds, float radius,
                            bool isHighlighted, bool isDown)
    {
        juce::ignoreUnused(button);
        bool isActive = isDown || button.getToggleState();

        // 柔和阴影（微调立体感）
        {
            juce::DropShadow ds;
            ds.colour = juce::Colour(Overdose::Colors::SoftShadow).withAlpha(isActive ? 0.33f : 0.22f);
            ds.radius = isActive ? 13 : 11;
            ds.offset = { 0, isActive ? 4 : 3 };
            juce::Path p;
            p.addRoundedRectangle(bounds, radius, radius);
            ds.drawForPath(g, p);
        }

        // 表面渐变
        juce::Colour top, bottom;
        if (isActive)
        {
            top = juce::Colour(Overdose::Colors::ButtonActiveTop);
            bottom = juce::Colour(Overdose::Colors::ButtonActiveBottom);
        }
        else if (isHighlighted)
        {
            top = juce::Colour(Overdose::Colors::ButtonHover);
            bottom = juce::Colour(Overdose::Colors::ButtonHover).darker(0.02f);
        }
        else
        {
            top = juce::Colour(Overdose::Colors::ButtonNormal);
            bottom = juce::Colour(Overdose::Colors::ButtonNormal).darker(0.03f);
        }

        juce::ColourGradient grad(top, bounds.getX(), bounds.getY(),
                                   bottom, bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(bounds, radius);

        // 粉白玻璃边（精致但可见）
        g.setColour(juce::Colour(Overdose::Colors::GlassEdge).withAlpha(isActive ? 0.65f : 0.48f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);

        // 顶部高光（增强玻璃质感）
        if (!isActive && !isDown)
        {
            auto highlightRect = bounds.reduced(1.5f).withHeight(bounds.getHeight() * 0.45f);
            juce::ColourGradient hl(juce::Colour(Overdose::Colors::GlassHighlight).withAlpha(0.50f),
                                     highlightRect.getX(), highlightRect.getY(),
                                     juce::Colour(Overdose::Colors::GlassHighlight).withAlpha(0.0f),
                                     highlightRect.getX(), highlightRect.getBottom(), false);
            g.setGradientFill(hl);
            g.fillRoundedRectangle(highlightRect, radius - 1.5f);

            // 底部内阴影（增强凹陷感）
            auto innerBottom = bounds.reduced(1.5f).withTrimmedTop(bounds.getHeight() * 0.60f);
            juce::ColourGradient innerShade(
                juce::Colours::transparentBlack,
                innerBottom.getCentreX(), innerBottom.getY(),
                juce::Colour(Overdose::Colors::PanelInsetShadow).withAlpha(0.18f),
                innerBottom.getCentreX(), innerBottom.getBottom(), false);
            g.setGradientFill(innerShade);
            g.fillRoundedRectangle(innerBottom, radius - 1.5f);
        }

        // Active 时粉色辉光
        if (isActive)
        {
            g.setColour(juce::Colour(Overdose::Colors::ButtonActiveGlow).withAlpha(0.35f));
            g.drawRoundedRectangle(bounds, radius, Overdose::Style::StrokeThin);
        }
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        const auto themeId = UIColors::currentThemeId();
        if (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose)
        {
            drawModernToggleButton(g, button, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
            return;
        }
        
        juce::LookAndFeel_V4::drawToggleButton(g, button, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
    }

    void drawModernToggleButton(juce::Graphics& g, juce::ToggleButton& button,
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
        const auto themeId = UIColors::currentThemeId();
        if (themeId == ThemeId::BlueBreeze)
        {
            juce::Rectangle<float> tickBounds(x, y, w, h);
            drawBlueBreezeSurface(g, tickBounds, 4.0f, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown, ticked);

            if (ticked)
            {
                juce::Path tickPath;
                tickPath.startNewSubPath(tickBounds.getX() + 3.0f, tickBounds.getCentreY());
                tickPath.lineTo(tickBounds.getCentreX(), tickBounds.getBottom() - 3.0f);
                tickPath.lineTo(tickBounds.getRight() - 3.0f, tickBounds.getY() + 3.0f);
                g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue));
                g.strokePath(tickPath, juce::PathStrokeType(2.0f));
            }
            return;
        }

        if (themeId == ThemeId::Overdose)
        {
            juce::Rectangle<float> tickBounds(x, y, w, h);
            drawOverdoseSurface(g, tickBounds, 4.0f, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown, ticked);

            if (ticked)
            {
                juce::Path tickPath;
                tickPath.startNewSubPath(tickBounds.getX() + 3.0f, tickBounds.getCentreY());
                tickPath.lineTo(tickBounds.getCentreX(), tickBounds.getBottom() - 3.0f);
                tickPath.lineTo(tickBounds.getRight() - 3.0f, tickBounds.getY() + 3.0f);
                g.setColour(juce::Colour(Overdose::Colors::PrimaryPink));
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
        // 显式使用 HONOR Sans CN Medium 确保 CJK 字符正确渲染
        return juce::Font(juce::FontOptions("HONOR Sans CN", "Medium", 16.0f));
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
        if (themeId != ThemeId::DarkBlueGrey
            && themeId != ThemeId::BlueBreeze
            && themeId != ThemeId::Overdose)
        {
            juce::LookAndFeel_V4::drawLabel(g, label);
            return;
        }

        const auto& style = UIColors::currentThemeStyle();
        auto bounds = label.getLocalBounds().toFloat().reduced(0.5f);

        if (themeId == ThemeId::BlueBreeze)
        {
            const bool focused = label.hasKeyboardFocus(true) && label.isEditable();
            drawBlueBreezeSurface(g, bounds, style.fieldRadius, label.isMouseOver(), false, focused);
            
            g.setColour(label.findColour(juce::Label::textColourId));
            g.setFont(label.getFont());
            g.drawFittedText(label.getText(), label.getLocalBounds().reduced(8, 2), label.getJustificationType(), 1);
            return;
        }

        if (themeId == ThemeId::Overdose)
        {
            const bool focused = label.hasKeyboardFocus(true) && label.isEditable();

            // 药丸形玻璃质感（更圆润，radius 更大，更接近纯药丸）
            const float pillRadius = juce::jmax(style.fieldRadius, bounds.getHeight() * 0.70f);
            
            // 阴影（更柔和）
            juce::DropShadow softShadow;
            softShadow.colour = juce::Colour(Overdose::Colors::SoftShadow).withAlpha(0.10f);
            softShadow.radius = 4;
            softShadow.offset = { 0, 1 };
            juce::Path shape;
            shape.addRoundedRectangle(bounds, pillRadius);
            softShadow.drawForPath(g, shape);

            // 主体渐变（更通透的白→淡紫）
            juce::ColourGradient bg(juce::Colour(0xFFFFFCFF).withAlpha(0.88f),  // 更通透的粉白
                                     bounds.getX(), bounds.getY(),
                                     juce::Colour(0xFFF0E8F8).withAlpha(0.84f),
                                     bounds.getX(), bounds.getBottom(), false);
            g.setGradientFill(bg);
            g.fillPath(shape);

            // 顶部高光（更强，覆盖更大区域）
            {
                juce::Graphics::ScopedSaveState clip(g);
                g.reduceClipRegion(shape);
                auto topBand = bounds.withHeight(bounds.getHeight() * 0.50f);
                juce::ColourGradient hl(juce::Colour(0xFFFFFFFF).withAlpha(0.60f),
                                         topBand.getCentreX(), topBand.getY(),
                                         juce::Colours::transparentWhite,
                                         topBand.getCentreX(), topBand.getBottom(), false);
                g.setGradientFill(hl);
                g.fillRect(topBand);
            }

            // 边框（粉色，更精致更淡）
            if (label.isMouseOver() || focused)
            {
                g.setColour(juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(focused ? 0.55f : 0.35f));
                g.strokePath(shape, juce::PathStrokeType(focused ? style.focusRingThickness : 0.8f));
            }
            else
            {
                g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.25f));
                g.strokePath(shape, juce::PathStrokeType(0.6f));
            }

            // 文字
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
            if (themeId == ThemeId::Overdose)
            {
                const float trackH = 6.0f;
                auto trackRect = bounds.withHeight(trackH)
                                     .withY(bounds.getCentreY() - trackH * 0.5f)
                                     .reduced(4.0f, 0.0f);

                // 轨道背景（淡紫，更精致）
                g.setColour(juce::Colour(Overdose::Colors::SliderTrackVisual).withAlpha(0.35f));
                g.fillRoundedRectangle(trackRect, trackH * 0.5f);

                // 激活部分（亮粉渐变）
                const float activeW = juce::jlimit(0.0f, trackRect.getWidth(), sliderPos - trackRect.getX());
                if (activeW > 0.0f)
                {
                    auto fillRect = trackRect.withWidth(activeW).reduced(1.5f, 1.0f);
                    juce::ColourGradient fillGrad(juce::Colour(Overdose::Colors::PrimaryPink),
                                                   fillRect.getX(), fillRect.getY(),
                                                   juce::Colour(Overdose::Colors::AccentPink),
                                                   fillRect.getX(), fillRect.getBottom(), false);
                    g.setGradientFill(fillGrad);
                    g.fillRoundedRectangle(fillRect, fillRect.getHeight() * 0.5f);
                }

                // 滑块（白→淡紫渐变+粉边+阴影）
                const float thumbSize = 14.0f;
                auto thumbRect = juce::Rectangle<float>(sliderPos - thumbSize * 0.5f,
                                                        bounds.getCentreY() - thumbSize * 0.5f,
                                                        thumbSize,
                                                        thumbSize);

                // 滑块阴影
                juce::DropShadow thumbShadow(juce::Colour(Overdose::Colors::SoftShadow).withAlpha(0.30f), 6, { 0, 2 });
                juce::Path thumbPath;
                thumbPath.addEllipse(thumbRect);
                thumbShadow.drawForPath(g, thumbPath);

                // 滑块主体渐变
                juce::ColourGradient thumbBg(juce::Colour(Overdose::Colors::SliderThumb),
                                              thumbRect.getX(), thumbRect.getY(),
                                              juce::Colour(Overdose::Colors::SliderThumbEdge),
                                              thumbRect.getX(), thumbRect.getBottom(), false);
                g.setGradientFill(thumbBg);
                g.fillEllipse(thumbRect);

                // 滑块顶部高光
                auto thumbHighlight = thumbRect.withHeight(thumbRect.getHeight() * 0.40f);
                juce::ColourGradient hl(juce::Colour(Overdose::Colors::GlassHighlight).withAlpha(0.35f),
                                         thumbHighlight.getX(), thumbHighlight.getY(),
                                         juce::Colours::transparentWhite,
                                         thumbHighlight.getX(), thumbHighlight.getBottom(), false);
                g.setGradientFill(hl);
                g.fillEllipse(thumbHighlight);

                // 滑块描边
                g.setColour(juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(slider.isMouseButtonDown() ? 0.60f : 0.45f));
                g.drawEllipse(thumbRect.reduced(0.5f), slider.isMouseButtonDown() ? 1.2f : 0.9f);

                // 悬停/拖拽时辉光
                if (slider.isMouseOverOrDragging() || slider.isMouseButtonDown())
                {
                    g.setColour(juce::Colour(Overdose::Colors::PinkGlow).withAlpha(slider.isMouseButtonDown() ? 0.30f : 0.18f));
                    g.drawEllipse(thumbRect.reduced(-1.5f), 0.8f);
                }
            }
            else if (themeId == ThemeId::BlueBreeze)
            {
                float trackH = 4.0f;
                auto trackRect = bounds.withHeight(trackH).withY(bounds.getCentreY() - trackH/2).reduced(4, 0);

                g.setColour(juce::Colour(BlueBreeze::Colors::KnobTrack).withAlpha(0.32f));
                g.fillRoundedRectangle(trackRect, trackH/2);

                auto fillColour = juce::Colour(BlueBreeze::Colors::AccentBlue).withAlpha(0.78f);
                auto fillRect = trackRect.withWidth(sliderPos - trackRect.getX());
                g.setColour(fillColour);
                g.fillRoundedRectangle(fillRect, trackH/2);

                float thumbW = 12.0f;
                float thumbH = 24.0f;
                auto thumbRect = juce::Rectangle<float>(sliderPos - thumbW/2, bounds.getCentreY() - thumbH/2, thumbW, thumbH);
                drawBlueBreezeSurface(g, thumbRect, thumbW / 2.0f, slider.isMouseOverOrDragging(), slider.isMouseButtonDown(), false);
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
        else if (themeId == ThemeId::Overdose)
        {
            // 粉色玻璃胶囊拇指
            juce::DropShadow ds;
            ds.colour = juce::Colour(Overdose::Colors::SoftShadow);
            ds.radius = 8;
            ds.offset = { 0, 2 };
            juce::Path shadowPath;
            shadowPath.addRoundedRectangle(thumb, thumbH * 0.5f);
            ds.drawForPath(g, shadowPath);

            juce::ColourGradient bodyGrad(
                juce::Colour(Overdose::Colors::SliderThumb), thumb.getX(), thumb.getY(),
                juce::Colour(Overdose::Colors::SliderThumb).darker(0.08f), thumb.getX(), thumb.getBottom(), false);
            g.setGradientFill(bodyGrad);
            g.fillRoundedRectangle(thumb, thumbH * 0.5f);

            g.setColour(juce::Colour(Overdose::Colors::GlassHighlight).withAlpha(0.25f));
            g.drawLine(thumb.getX() + thumbH * 0.5f, thumb.getY() + 1.5f,
                       thumb.getRight() - thumbH * 0.5f, thumb.getY() + 1.5f, 1.5f);

            g.setColour(juce::Colour(Overdose::Colors::SliderThumbEdge));
            g.drawRoundedRectangle(thumb.reduced(0.5f), thumbH * 0.5f, 1.0f);
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
        
        g.setColour(juce::Colour(BlueBreeze::Colors::KnobTrack).withAlpha(0.32f));
        g.fillRoundedRectangle(trackRect, trackW/2);
        
        // Active Fill
        // sliderPos is the center of the thumb.
        // Assuming slider min is at bottom.
        float fillTop = juce::jmax(trackRect.getY(), sliderPos);
        float fillBottom = trackRect.getBottom();
        
        if (fillTop < fillBottom)
        {
            juce::Rectangle<float> activeTrack(trackRect.getX(), fillTop, trackRect.getWidth(), fillBottom - fillTop);
            g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue).withAlpha(0.78f));
            g.fillRoundedRectangle(activeTrack, trackW/2);
        }
        
        // Capsule Thumb
        float thumbW = 24.0f;
        float thumbH = 12.0f;
        auto thumbRect = juce::Rectangle<float>(bounds.getCentreX() - thumbW/2, sliderPos - thumbH/2, thumbW, thumbH);
        
        drawBlueBreezeSurface(g, thumbRect, thumbH / 2.0f, slider.isMouseOverOrDragging(), slider.isMouseButtonDown(), false);
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
        else if (themeId == ThemeId::Overdose)
        {
            // 统一调用程序化旋钮绘制（拟物风格+线状指针）
            UIColors::drawOverdoseKnob(g, bounds, sliderPosProportional,
                                        slider.isMouseOverOrDragging(),
                                        rotaryStartAngle, rotaryEndAngle);
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
        juce::ignoreUnused(cx, cy, radius, angle, trackRadius);
        const auto denominator = rotaryEndAngle - rotaryStartAngle;
        const auto normalised = denominator != 0.0f ? (angle - rotaryStartAngle) / denominator : 0.0f;
        UIColors::drawBlueBreezePianoKnob(g,
                                          bounds,
                                          normalised,
                                          slider.isMouseOverOrDragging(),
                                          rotaryStartAngle,
                                          rotaryEndAngle);
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
        else if (themeId == ThemeId::Overdose)
        {
            g.fillAll(juce::Colour(Overdose::Colors::CanvasTop));
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
                const auto field = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
                if (themeId == ThemeId::Overdose)
                {
                    juce::ignoreUnused(bg);
                    UIColors::fillOverdosePanelBackground(g, field, style.fieldRadius);
                }
                else if (themeId == ThemeId::BlueBreeze)
                {
                    juce::ignoreUnused(bg);
                    drawBlueBreezeSurface(g, field.reduced(0.5f), style.fieldRadius, textEditor.isMouseOver(), false, textEditor.hasKeyboardFocus(true));
                }
                else
                {
                    g.setColour(bg);
                    g.fillRoundedRectangle(field, style.fieldRadius);
                }
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
                    else if (themeId == ThemeId::Overdose)
                    {
                        g.setColour(juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.85f));
                        g.drawRoundedRectangle(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f, style.fieldRadius, style.focusRingThickness);
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
                    else if (themeId == ThemeId::Overdose)
                    {
                        if (textEditor.isMouseOver())
                            border = juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.6f);
                        else
                            border = juce::Colour(Overdose::Colors::PanelBorder);

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

        if (themeId == ThemeId::Overdose)
        {
            const bool isActive = isButtonDown || box.isPopupActive();
            const bool isHover = box.isMouseOver(true);
            const auto bgBounds = boxBounds.reduced(2.0f);
            UIColors::fillOverdosePanelBackground(g, bgBounds, style.fieldRadius);

            if (isHover || isActive)
            {
                g.setColour(juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(isActive ? 0.34f : 0.18f));
                g.drawRoundedRectangle(bgBounds.reduced(0.75f), style.fieldRadius, isActive ? 1.35f : 1.0f);
            }

            if (!box.getProperties().contains("noArrow"))
            {
                auto arrowZone = boxBounds.removeFromRight(boxBounds.getHeight()).reduced(boxBounds.getHeight() * 0.35f);
                juce::Path path;
                path.startNewSubPath(arrowZone.getX(), arrowZone.getY());
                path.lineTo(arrowZone.getCentreX(), arrowZone.getBottom());
                path.lineTo(arrowZone.getRight(), arrowZone.getY());

                g.setColour(juce::Colour(Overdose::Colors::TextSecondary));
                g.strokePath(path, juce::PathStrokeType(1.5f));
            }
            return;
        }

        if (themeId == ThemeId::BlueBreeze)
        {
            const float radius = style.fieldRadius;
            bool isActive = isButtonDown || box.isPopupActive();
            bool isHover = box.isMouseOver(true);

            drawBlueBreezeSurface(g, boxBounds.reduced(2.0f), radius, isHover, isButtonDown, isActive);

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
        // 背景与 UnifiedToolbarButton 一致缩进 2px，视觉高度统一 36px
        const auto bgBounds = boxBounds.reduced(2.0f);
        if (themeId == ThemeId::DarkBlueGrey)
        {
            g.setColour(bg);
            g.fillRoundedRectangle(bgBounds, cornerSize);
        }
        else
        {
            g.setColour(bg);
            g.fillRoundedRectangle(bgBounds, cornerSize);
        }

        auto outline = box.findColour(juce::ComboBox::outlineColourId);
        if (themeId == ThemeId::DarkBlueGrey)
            outline = UIColors::panelBorder.withAlpha(0.74f);
        g.setColour(outline);
        g.drawRoundedRectangle(bgBounds.reduced(0.5f, 0.5f), cornerSize, style.strokeThin);

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
            g.fillAll(juce::Colour(BlueBreeze::Colors::PanelTop).withAlpha(0.98f));
            g.setColour(juce::Colour(BlueBreeze::Colors::PanelBorder));
            g.drawRect(0, 0, width, height);
            return;
        }

        if (themeId == ThemeId::Overdose)
        {
            g.fillAll(juce::Colour(Overdose::Colors::PanelOpaqueTop).withAlpha(0.98f));
            g.setColour(juce::Colour(Overdose::Colors::PanelBorder));
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
                g.setColour(juce::Colour(BlueBreeze::Colors::AccentBlue).withAlpha(0.13f));
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

        if (themeId == ThemeId::Overdose)
        {
            if (isSeparator)
            {
                auto r = area.reduced(5, 0);
                r.removeFromTop(juce::roundToInt(((float)r.getHeight() * 0.5f) - 0.5f));
                g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.5f));
                g.fillRect(r.removeFromTop(1));
                return;
            }

            auto textCol = (textColour != nullptr ? *textColour : juce::Colour(Overdose::Colors::TextPrimary));

            if (isHighlighted)
            {
                g.setColour(juce::Colour(Overdose::Colors::PinkGlowSoft));
                g.fillRect(area);
                g.setColour(juce::Colour(Overdose::Colors::PrimaryPink));
                textCol = juce::Colour(Overdose::Colors::PrimaryPink);
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

                g.setColour(juce::Colour(Overdose::Colors::PrimaryPink));
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
        return UIColors::scrollBarThickness;
    }
    
    void drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar,
                       int x, int y, int width, int height,
                       bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                       bool isMouseOver, bool isMouseDown) override
    {
        const auto themeId = UIColors::currentThemeId();
        auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                             static_cast<float>(width), static_cast<float>(height));

        if (themeId == ThemeId::Overdose)
        {
            juce::ignoreUnused(scrollbar);

            juce::Rectangle<float> thumbBounds;
            if (isScrollbarVertical)
            {
                thumbBounds = juce::Rectangle<float>(bounds.getX() + 2.0f,
                                                     bounds.getY() + static_cast<float>(thumbStartPosition),
                                                     bounds.getWidth() - 4.0f,
                                                     static_cast<float>(thumbSize));
                thumbBounds.setHeight(juce::jmax(thumbBounds.getHeight(), 28.0f));

                // 轨道背景（浅紫白渐变，极低存在感）
                juce::ColourGradient trackBg(juce::Colour(Overdose::Colors::ScrollbarTrack).withAlpha(0.12f),
                                              bounds.getX(), bounds.getY(),
                                              juce::Colour(Overdose::Colors::ScrollbarTrack).withAlpha(0.05f),
                                              bounds.getX(), bounds.getBottom(), false);
                g.setGradientFill(trackBg);
                g.fillRoundedRectangle(bounds.reduced(2.0f, 0.0f), 4.0f);
            }
            else
            {
                thumbBounds = juce::Rectangle<float>(bounds.getX() + static_cast<float>(thumbStartPosition),
                                                     bounds.getY() + 2.0f,
                                                     static_cast<float>(thumbSize),
                                                     bounds.getHeight() - 4.0f);
                thumbBounds.setWidth(juce::jmax(thumbBounds.getWidth(), 28.0f));

                // 轨道背景（浅紫白渐变，极低存在感）
                juce::ColourGradient trackBg(juce::Colour(Overdose::Colors::ScrollbarTrack).withAlpha(0.12f),
                                              bounds.getX(), bounds.getY(),
                                              juce::Colour(Overdose::Colors::ScrollbarTrack).withAlpha(0.05f),
                                              bounds.getRight(), bounds.getY(), false);
                g.setGradientFill(trackBg);
                g.fillRoundedRectangle(bounds.reduced(0.0f, 2.0f), 4.0f);
            }

            // Soft pink glow beneath thumb
            if (isMouseOver || isMouseDown)
            {
                juce::DropShadow glow;
                glow.colour = juce::Colour(Overdose::Colors::PinkGlowSoft).withAlpha(isMouseDown ? 0.35f : 0.20f);
                glow.radius = isMouseDown ? 10 : 6;
                glow.offset = {};
                juce::Path thumbPath;
                const float thumbRadius = juce::jmin(thumbBounds.getWidth(), thumbBounds.getHeight()) * 0.45f;
                thumbPath.addRoundedRectangle(thumbBounds, thumbRadius);
                glow.drawForPath(g, thumbPath);
            }

            // 滑块（粉渐变：参考图 #FF76BF 高亮粉滑块）
            juce::ColourGradient thumbBg(juce::Colour(Overdose::Colors::ScrollbarThumbTop),
                                          thumbBounds.getX(), thumbBounds.getY(),
                                          juce::Colour(Overdose::Colors::ScrollbarThumbPink),
                                          thumbBounds.getX(), thumbBounds.getBottom(), false);
            g.setGradientFill(thumbBg);
            const float thumbRadius = juce::jmin(thumbBounds.getWidth(), thumbBounds.getHeight()) * 0.45f;
            g.fillRoundedRectangle(thumbBounds, thumbRadius);

            // 白色边缘高光（增强玻璃质感）
            g.setColour(juce::Colour(Overdose::Colors::GlassHighlight).withAlpha(0.65f));
            g.drawRoundedRectangle(thumbBounds.reduced(0.5f), thumbRadius, 1.2f);

            // 顶部高光
            auto highlightRect = thumbBounds.withHeight(thumbBounds.getHeight() * 0.35f);
            juce::ColourGradient hl(juce::Colour(Overdose::Colors::GlassHighlight).withAlpha(0.25f),
                                     highlightRect.getX(), highlightRect.getY(),
                                     juce::Colour(Overdose::Colors::GlassHighlight).withAlpha(0.0f),
                                     highlightRect.getX(), highlightRect.getBottom(), false);
            g.setGradientFill(hl);
            g.fillRoundedRectangle(highlightRect, thumbRadius - 1.0f);

            if (isMouseOver || isMouseDown)
            {
                const auto outline = juce::Colour(Overdose::Colors::PrimaryPink)
                    .withAlpha(isMouseDown ? 0.60f : 0.35f);
                g.setColour(outline);
                g.drawRoundedRectangle(thumbBounds.reduced(0.5f), thumbRadius, isMouseDown ? 1.5f : 1.0f);
            }
            return;
        }
        
        // 鏍规嵁涓婚閫夋嫨鐏拌壊閰嶈壊鏂规
        juce::Colour trackBg, thumbBg, thumbHover, thumbPressed, highlight;
        
        if (themeId == ThemeId::BlueBreeze)
        {
            trackBg = juce::Colour(BlueBreeze::Colors::PanelBorder);
            thumbBg = juce::Colour(BlueBreeze::Colors::ControlBottom);
            thumbHover = juce::Colour(BlueBreeze::Colors::ControlHover);
            thumbPressed = juce::Colour(BlueBreeze::Colors::ControlPressed);
            highlight = juce::Colour(BlueBreeze::Colors::SourceLight);
        }
        else if (themeId == ThemeId::DarkBlueGrey)
        {
            // DarkBlueGrey 主题 - 浅灰色调
            trackBg = UIColors::backgroundLight.darker(0.15f);
            thumbBg = UIColors::backgroundLight.brighter(0.1f);  // 更浅的背景色
            thumbHover = UIColors::textPrimary;
            thumbPressed = UIColors::panelBorder;
            highlight = UIColors::textPrimary;
        }
        else
        {
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
