#pragma once

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "ThemeTokens.h"

namespace OpenTune {

struct UIColors
{
    // 导航栏（TopBar/TransportBar）统一度量
    // 目标：所有控件同高，不同宽；字体统一，不再“一个大一个小”。
    static constexpr int navControlHeight = 50; // Scaled by 1.25x (was 40)
    static constexpr float navFontHeight = 20.0f; // Scaled by 1.25x (was 16.0f)
    static constexpr float navMonoFontHeight = 20.0f; // Scaled by 1.25x (was 16.0f)

    // Primary Colors - 柔和蓝色系
    static inline juce::Colour primaryPurple { 0xFF6A9AB8 };
    static inline juce::Colour accent { 0xFF6A9AB8 };
    static inline juce::Colour lightPurple { 0xFF8AB8D0 };
    static inline juce::Colour darkPurple { 0xFF4A7A98 };

    // Background Colors
    static inline juce::Colour backgroundDark { 0xFF222830 };
    static inline juce::Colour backgroundMedium { 0xFF2F3640 };
    static inline juce::Colour backgroundLight { 0xFF3E4652 };

    // Gradient Background Colors
    static inline juce::Colour gradientTop { 0xFF353C48 };
    static inline juce::Colour gradientBottom { 0xFF222830 };

    // UI Element Colors
    static inline juce::Colour panelBorder { 0xFF4E5865 };
    static inline juce::Colour buttonNormal { 0xFF3E4652 };
    static inline juce::Colour buttonHover { 0xFF5D6D7E };
    static inline juce::Colour buttonPressed { 0xFF2C3E50 };

    // 3D Effect Colors (for CyberNeon)
    static inline juce::Colour bevelLight { 0xFF4A4A60 };
    static inline juce::Colour bevelDark { 0xFF080810 };
    static inline juce::Colour glowColor { 0xFF00F5FF };

    // Text Colors
    static inline juce::Colour textPrimary { 0xFFECF0F1 };
    static inline juce::Colour textSecondary { 0xFFBDC3C7 };
    static inline juce::Colour textDisabled { 0xFF7F8C8D };
    static inline juce::Colour textHighlight { 0xFF7FB3D5 };

    // Piano Roll Colors
    static inline juce::Colour rollBackground { 0xFF1E2329 };
    static inline juce::Colour laneC { 0xFF2F3640 };
    static inline juce::Colour laneOther { 0xFF252B33 };
    static inline juce::Colour gridLine { 0xFF3E4652 };

    // Pitch Curve Colors - 蓝色系主题
    static inline juce::Colour originalF0 { 0xFFFF6666 };   // 柔和红色
    static inline juce::Colour correctedF0 { 0xFF6A9AB8 };  // 柔和蓝色
    static inline juce::Colour shadowTrack { 0x406A9AB8 };

    // Note Block Colors
    static inline juce::Colour noteBlock { 0xFF7FB3D5 };
    static inline juce::Colour noteBlockBorder { 0xFFA9CCE3 };
    static inline juce::Colour noteBlockSelected { 0xFFFF7675 };
    static inline juce::Colour noteBlockHover { 0xFFAED6F1 };

    // Playhead & Timeline
    static inline juce::Colour playhead { 0xFFFFFFFF };
    static inline juce::Colour timelineMarker { 0xFF7FB3D5 };
    static inline juce::Colour beatMarker { 0xFF4E5865 };

    // Tool Selection
    static inline juce::Colour toolActive { 0xFF7FB3D5 };
    static inline juce::Colour toolInactive { 0xFF4E5865 };
    static inline juce::Colour buttonInactive { 0xFF4E5865 };

    // Status Indicators
    static inline juce::Colour statusProcessing { 0xFFF39C12 };
    static inline juce::Colour statusReady { 0xFF2ECC71 };
    static inline juce::Colour statusError { 0xFFE74C3C };

    // Waveform Colors - Dark Grey to match button style
    static inline juce::Colour waveformFill { 0x603E4652 };
    static inline juce::Colour waveformOutline { 0xFF3E4652 };

    // Scale Detection
    static inline juce::Colour scaleHighlight { 0x60F39C12 };

    // Knob Colors
    static inline juce::Colour knobBody { 0xFF1B2026 };
    static inline juce::Colour knobIndicator { 0xFF7FB3D5 };

    // Global Corner Radius
    static inline float cornerRadius = 8.0f;
    static inline ThemeId currentThemeId_ = ThemeId::Aurora;
    static inline ThemeStyle currentThemeStyle_ = Theme::getStyle(ThemeId::Aurora);

    // 阴影层级（用于现代 UI 的立体感表达）
    // L1: Ambient（面板贴底）
    // L2: Float（悬浮控件，例如顶部条/工具条）
    // L3: Pop（弹窗/菜单）
    enum class ShadowLevel : int
    {
        Ambient = 0,
        Float = 1,
        Pop = 2
    };

    static void applyTheme(const ThemeTokens& tokens)
    {
        primaryPurple = tokens.primaryPurple;
        accent = tokens.accent;
        lightPurple = tokens.lightPurple;
        darkPurple = tokens.darkPurple;

        backgroundDark = tokens.backgroundDark;
        backgroundMedium = tokens.backgroundMedium;
        backgroundLight = tokens.backgroundLight;

        gradientTop = tokens.gradientTop;
        gradientBottom = tokens.gradientBottom;

        panelBorder = tokens.panelBorder;
        buttonNormal = tokens.buttonNormal;
        buttonHover = tokens.buttonHover;
        buttonPressed = tokens.buttonPressed;

        bevelLight = tokens.bevelLight;
        bevelDark = tokens.bevelDark;
        glowColor = tokens.glowColor;

        textPrimary = tokens.textPrimary;
        textSecondary = tokens.textSecondary;
        textDisabled = tokens.textDisabled;
        textHighlight = tokens.textHighlight;

        rollBackground = tokens.rollBackground;
        laneC = tokens.laneC;
        laneOther = tokens.laneOther;
        gridLine = tokens.gridLine;

        originalF0 = tokens.originalF0;
        correctedF0 = tokens.correctedF0;
        shadowTrack = tokens.shadowTrack;

        noteBlock = tokens.noteBlock;
        noteBlockBorder = tokens.noteBlockBorder;
        noteBlockSelected = tokens.noteBlockSelected;
        noteBlockHover = tokens.noteBlockHover;

        playhead = tokens.playhead;
        timelineMarker = tokens.timelineMarker;
        beatMarker = tokens.beatMarker;

        toolActive = tokens.toolActive;
        toolInactive = tokens.toolInactive;
        buttonInactive = tokens.buttonInactive;

        statusProcessing = tokens.statusProcessing;
        statusReady = tokens.statusReady;
        statusError = tokens.statusError;

        waveformFill = tokens.waveformFill;
        waveformOutline = tokens.waveformOutline;

        scaleHighlight = tokens.scaleHighlight;

        knobBody = tokens.knobBody;
        knobIndicator = tokens.knobIndicator;

        cornerRadius = tokens.cornerRadius;
    }

    static void applyTheme(ThemeId themeId)
    {
        currentThemeId_ = themeId;
        currentThemeStyle_ = Theme::getStyle(themeId);
        applyTheme(Theme::getTokens(themeId));
    }

    static ThemeId currentThemeId()
    {
        return currentThemeId_;
    }

    static const ThemeTokens& currentTokens()
    {
        return Theme::getTokens(currentThemeId_);
    }

    static const ThemeStyle& currentThemeStyle()
    {
        return currentThemeStyle_;
    }

    // Shadow Helper - Dark Blue-Grey 主题：冷色环境阴影 + 更柔和扩散
    static void drawShadow(juce::Graphics& g, const juce::Rectangle<float>& bounds)
    {
        drawShadow(g, bounds, ShadowLevel::Ambient);
    }

    static void drawShadow(juce::Graphics& g, const juce::Rectangle<float>& bounds, ShadowLevel level)
    {
        const auto themeId = currentThemeId();
        const auto& style = currentThemeStyle();

        juce::DropShadow ds;

        if (themeId == ThemeId::DarkBlueGrey)
        {
            // 深蓝灰主题：不要用纯黑阴影，使用带环境色偏移的“冷色空气感”
            // 注意：这里故意不复用 style.shadowAlpha/radius，因为我们需要 L1/L2/L3 分层。
            const auto shadowBase = juce::Colour { 0xFF050A12 }; // 冷色深阴影基色
            if (level == ShadowLevel::Ambient)
            {
                ds.colour = shadowBase.withAlpha(0.28f);
                ds.radius = 14;
                ds.offset = { 0, 3 };
            }
            else if (level == ShadowLevel::Float)
            {
                ds.colour = shadowBase.withAlpha(0.34f);
                ds.radius = 20;
                ds.offset = { 0, 6 };
            }
            else
            {
                ds.colour = shadowBase.withAlpha(0.45f);
                ds.radius = 26;
                ds.offset = { 0, 10 };
            }
        }
        else
        {
            // 其他主题保持原有行为
            ds.colour = juce::Colours::black.withAlpha(style.shadowAlpha);
            ds.radius = style.shadowRadius;
            ds.offset = style.shadowOffset;
        }

        juce::Path p;
        p.addRoundedRectangle(bounds, style.panelRadius);
        ds.drawForPath(g, p);
    }

    static void fillSoothe2CanvasBackground(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        const auto themeId = currentThemeId();

        auto top = juce::Colour { 0xFFF7F3EA };
        auto bottom = juce::Colour { 0xFFD2E0E8 };

        if (themeId != ThemeId::DarkBlueGrey)
        {
            top = UIColors::gradientTop;
            bottom = UIColors::gradientBottom;
        }

        juce::ColourGradient base(top, bounds.getX(), bounds.getY(),
                                  bottom, bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill(base);
        if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
        else g.fillRect(bounds);

        auto c = bounds.getCentre();
        auto edge = juce::Point<float>(bounds.getX(), bounds.getY());
        juce::ColourGradient glow(top.withAlpha(0.55f), c.x, c.y,
                                  top.withAlpha(0.0f), edge.x, edge.y, true);
        g.setGradientFill(glow);
        if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
        else g.fillRect(bounds);
    }

    static void fillSoothe2SpectrumBackground(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        const auto themeId = currentThemeId();

        // For DarkBlueGrey (Soothe2 style): Use the actual theme colors
        if (themeId == ThemeId::DarkBlueGrey)
        {
            // 1. Base Dark Background with subtle gradient
            juce::ColourGradient baseGrad(
                UIColors::backgroundDark.brighter(0.08f), bounds.getX(), bounds.getY(),
                UIColors::backgroundDark.darker(0.12f), bounds.getX(), bounds.getBottom(), false);
            g.setGradientFill(baseGrad);
            if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
            else g.fillRect(bounds);

            // 2. Very subtle grid lines
            g.setColour(UIColors::panelBorder.withAlpha(0.08f));
            for (float x = 0.1f; x < 1.0f; x += 0.1f)
            {
                float xPos = bounds.getX() + bounds.getWidth() * x;
                g.drawVerticalLine((int)xPos, bounds.getY(), bounds.getBottom());
            }
            for (float y = 0.1f; y < 1.0f; y += 0.2f)
            {
                float yPos = bounds.getY() + bounds.getHeight() * y;
                g.drawHorizontalLine((int)yPos, bounds.getX(), bounds.getRight());
            }
            return;
        }

        // Fallback for other themes
        g.setColour(UIColors::backgroundMedium);
        if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
        else g.fillRect(bounds);
    }

    static void fillPanelBackground(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        const auto themeId = currentThemeId();
        const auto& style = currentThemeStyle();

        if (themeId == ThemeId::DarkBlueGrey)
        {
            // 深蓝灰：现代面板背景（阴影由 drawShadow 统一处理，避免重复叠加）

            // 1. 面板背景 - 柔和的渐变（极克制）
            juce::ColourGradient panelGrad(
                UIColors::backgroundMedium.brighter(0.04f), bounds.getX(), bounds.getY(),
                UIColors::backgroundMedium.darker(0.04f), bounds.getX(), bounds.getBottom(), false);
            g.setGradientFill(panelGrad);
            if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
            else g.fillRect(bounds);

            // 2. 顶部高光 - 增加立体感（线条明快，不要厚重）
            g.setColour(UIColors::textPrimary.withAlpha(0.08f));
            g.drawLine(bounds.getX() + radius, bounds.getY() + 1.0f, 
                      bounds.getRight() - radius, bounds.getY() + 1.0f, 1.5f);
            
            // 3. 内阴影 - 底部边缘的轻微暗化
            if (radius > 0.0f)
            {
                juce::Path innerPath;
                innerPath.addRoundedRectangle(bounds.reduced(1.0f), radius - 1.0f);
                g.setColour(UIColors::bevelDark.withAlpha(0.22f));
                g.strokePath(innerPath, juce::PathStrokeType(2.0f));
            }
            
            return;
        }

        // BlueBreeze 默认主题
        juce::ColourGradient base(UIColors::gradientTop, bounds.getX(), bounds.getY(),
                                  UIColors::gradientBottom, bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill(base);
        if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
        else g.fillRect(bounds);

        float softLightAlpha = 0.05f;

        auto topH = bounds.withTrimmedBottom(bounds.getHeight() * 0.45f);
        juce::ColourGradient softLight(UIColors::textPrimary.withAlpha(softLightAlpha), topH.getX(), topH.getY(),
                                       UIColors::textPrimary.withAlpha(0.0f), topH.getX(), topH.getBottom(), false);
        g.setGradientFill(softLight);
        if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
        else g.fillRect(bounds);

        float vignetteAlpha = 0.05f;

        if (vignetteAlpha > 0.0f)
        {
            auto c = bounds.getCentre();
            juce::ColourGradient vignette(juce::Colours::transparentBlack, c.x, c.y,
                                          juce::Colours::black.withAlpha(vignetteAlpha), bounds.getX(), bounds.getY(), true);
            g.setGradientFill(vignette);
            if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
            else g.fillRect(bounds);
        }

        if (style.glowAlpha > 0.0f)
        {
            auto c = bounds.getCentre();
            juce::ColourGradient edgeGlow(juce::Colours::transparentBlack, c.x, c.y,
                                          UIColors::accent.withAlpha(style.glowAlpha * 0.45f), bounds.getX(), bounds.getY(), true);
            g.setGradientFill(edgeGlow);
            if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
            else g.fillRect(bounds);
        }
    }

    static void drawPanelFrame(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        const auto themeId = currentThemeId();
        const auto& style = currentThemeStyle();

        float borderAlpha = 0.9f;
        float innerAlpha = 0.05f;
        if (themeId == ThemeId::DarkBlueGrey)
        {
            borderAlpha = 0.55f;  // 深色底上的边框需要更清晰，但仍保持克制
            innerAlpha = 0.06f;   // 内高光更弱，避免“发灰”
        }

        g.setColour(UIColors::panelBorder.withAlpha(borderAlpha));
        if (radius > 0.0f) g.drawRoundedRectangle(bounds.reduced(0.5f), radius, static_cast<float>(style.strokeThin));
        else g.drawRect(bounds.reduced(0.5f), static_cast<float>(style.strokeThin));

        // 内高光线 - 增加立体感
        g.setColour(UIColors::textPrimary.withAlpha(innerAlpha));
        if (radius > 1.0f) 
            g.drawRoundedRectangle(bounds.reduced(1.5f), radius - 1.0f, static_cast<float>(style.strokeThin));
        else 
            g.drawRect(bounds.reduced(1.5f), static_cast<float>(style.strokeThin));

        if (style.glowAlpha > 0.0f)
        {
            auto glow = UIColors::accent.withAlpha(style.glowAlpha);
            for (int i = 0; i < 2; ++i)
            {
                g.setColour(glow.withMultipliedAlpha(1.0f - 0.35f * static_cast<float>(i)));
                if (radius > 0.0f)
                    g.drawRoundedRectangle(bounds.reduced(-0.5f * static_cast<float>(i)), radius + 0.5f, static_cast<float>(style.strokeThin + static_cast<float>(i)));
                else
                    g.drawRect(bounds.reduced(-0.5f * static_cast<float>(i)), static_cast<float>(style.strokeThin + static_cast<float>(i)));
            }
        }
    }

    // Font Management
    static juce::Font getUIFont(float height = 16.0f)
    {
        return juce::Font(juce::FontOptions(juce::Font::getDefaultSansSerifFontName(), "Regular", juce::jmax(16.0f, height)));
    }

    static juce::Font getHeaderFont(float height = 18.0f)
    {
        return juce::Font(juce::FontOptions(juce::Font::getDefaultSansSerifFontName(), "Semibold", juce::jmax(18.0f, height)));
    }

    static juce::Font getLabelFont(float height = 14.0f)
    {
        return juce::Font(juce::FontOptions(juce::Font::getDefaultSansSerifFontName(), "Regular", juce::jmax(14.0f, height)));
    }

    static juce::Font getMonoFont(float height = 20.0f)
    {
        return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), "Bold", height));
    }

    static juce::Colour withAlpha(const juce::Colour& baseColor, float alpha)
    {
        return baseColor.withAlpha(alpha);
    }

    static juce::Colour interpolate(const juce::Colour& colorA, const juce::Colour& colorB, float ratio)
    {
        return colorA.interpolatedWith(colorB, ratio);
    }

    static juce::Colour brighten(const juce::Colour& color, float amount)
    {
        return color.brighter(amount);
    }

    static juce::Colour darken(const juce::Colour& color, float amount)
    {
        return color.darker(amount);
    }
};

// Small font text button
class SmallFontTextButton : public juce::TextButton
{
public:
    SmallFontTextButton(const juce::String& name = {}) : juce::TextButton(name) {}

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = getLocalBounds().toFloat();

        juce::Colour buttonColor;
        if (shouldDrawButtonAsDown)
            buttonColor = UIColors::buttonPressed;
        else if (shouldDrawButtonAsHighlighted)
            buttonColor = UIColors::buttonHover;
        else
            buttonColor = findColour(juce::TextButton::buttonColourId);

        g.setColour(buttonColor);
        g.fillRoundedRectangle(bounds, 4.0f);

        g.setColour(findColour(juce::TextButton::textColourOffId));
        g.setFont(juce::Font(juce::FontOptions("HONOR Sans CN", "Medium", 12.0f)));
        g.drawText(getButtonText(), bounds, juce::Justification::centred);
    }
};

} // namespace OpenTune
