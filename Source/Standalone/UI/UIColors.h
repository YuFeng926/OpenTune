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
    static constexpr int scrollBarThickness = 20;
    static constexpr float scrollBarThumbThickness = 10.0f;

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

    // Pitch Curve Colors
    static inline juce::Colour originalF0 { 0xFFD24A3A };   // Piano Roll reference red
    static inline juce::Colour correctedF0 { 0xFF196FC4 };  // Piano Roll reference blue
    static inline juce::Colour shadowTrack { 0x30196FC4 };

    // Note Block Colors
    static inline juce::Colour noteBlock { 0xFF235AA8 };
    static inline juce::Colour noteBlockBorder { 0xFF3A69A2 };
    static inline juce::Colour noteBlockSelected { 0xFF2F6FC4 };
    static inline juce::Colour noteBlockHover { 0xFF2A63B8 };

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
    static inline juce::Colour waveformFill { 0x600C3C4A };
    static inline juce::Colour waveformOutline { 0xFF0C3C4A };

    // Scale Detection
    static inline juce::Colour scaleHighlight { 0x20FFFFFF };

    // Knob Colors
    static inline juce::Colour knobBody { 0xFF1B2026 };
    static inline juce::Colour knobIndicator { 0xFF7FB3D5 };
    static inline juce::Colour displayWellTop { 0xFF101A22 };
    static inline juce::Colour displayWellBottom { 0xFF071016 };
    static inline juce::Colour displayWellEdge { 0xFF48677A };
    static inline juce::Colour displayText { 0xFFAFC7D8 };
    static inline juce::Colour displayTextDim { 0x3396AFC1 };
    static inline juce::Colour darkControlFace { 0xFF07090B };
    static inline juce::Colour darkControlEdge { 0xFF536674 };
    static inline juce::Colour keyBedWhite { 0xFFFFFFFF };
    static inline juce::Colour keyBedBlack { 0xFF0F1316 };
    static inline juce::Colour keyBedDivider { 0xFFD7E0E8 };
    static inline juce::Colour glassSurface { 0xE10B1827 };
    static inline juce::Colour glassHighlight { 0x1A7CC5F4 };
    static inline juce::Colour glassEdge { 0x666DA8D8 };
    static inline juce::Colour panelGlow { 0x2A133964 };
    static inline juce::Colour auroraButtonNormal { 0xC00B1728 };
    static inline juce::Colour auroraButtonHover { 0xD1112A44 };
    static inline juce::Colour auroraButtonActive { 0xE51B5F9E };
    static inline juce::Colour pianoRollBackground { 0xFF0C1D2F };
    static inline juce::Colour pianoRollLane { 0xFF87B6D4 };
    static inline juce::Colour pianoRollGrid { 0xFF9BD5FF };
    static inline juce::Colour pianoRollWaveform { 0xFF0C3C4A };
    static inline juce::Colour trackPanelBackground { 0xFF0C1D2F };
    static inline juce::Colour sidebarTrackFade { 0x7A1F7BFF };
    static inline juce::Colour auroraSidebarShellTop { 0xFF142C45 };
    static inline juce::Colour auroraSidebarShellMid { 0xFF0E2238 };
    static inline juce::Colour auroraSidebarShellBottom { 0xFF081523 };
    static inline juce::Colour auroraSidebarTopLip { 0x4CA9D8FF };
    static inline juce::Colour auroraSidebarOuterRim { 0x4A46719B };
    static inline juce::Colour auroraSidebarInnerRim { 0x2193CCFF };
    static inline juce::Colour auroraSidebarEdgeAura { 0x1A2D7FD0 };
    static inline juce::Colour auroraSidebarCornerBloom { 0x142E8BE4 };
    static inline juce::Colour knobRim { 0x9A4FC3FF };
    static inline juce::Colour knobGlow { 0x821688FF };
    static inline const juce::Identifier auroraChromeIntensityProperty { "auroraChromeIntensity" };

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
        displayWellTop = tokens.displayWellTop;
        displayWellBottom = tokens.displayWellBottom;
        displayWellEdge = tokens.displayWellEdge;
        displayText = tokens.displayText;
        displayTextDim = tokens.displayTextDim;
        darkControlFace = tokens.darkControlFace;
        darkControlEdge = tokens.darkControlEdge;
        keyBedWhite = tokens.keyBedWhite;
        keyBedBlack = tokens.keyBedBlack;
        keyBedDivider = tokens.keyBedDivider;
        glassSurface = tokens.glassSurface;
        glassHighlight = tokens.glassHighlight;
        glassEdge = tokens.glassEdge;
        panelGlow = tokens.panelGlow;
        auroraButtonNormal = tokens.auroraButtonNormal;
        auroraButtonHover = tokens.auroraButtonHover;
        auroraButtonActive = tokens.auroraButtonActive;
        pianoRollBackground = tokens.pianoRollBackground;
        pianoRollLane = tokens.pianoRollLane;
        pianoRollGrid = tokens.pianoRollGrid;
        pianoRollWaveform = tokens.pianoRollWaveform;
        trackPanelBackground = tokens.trackPanelBackground;
        sidebarTrackFade = tokens.sidebarTrackFade;
        auroraSidebarShellTop = tokens.auroraSidebarShellTop;
        auroraSidebarShellMid = tokens.auroraSidebarShellMid;
        auroraSidebarShellBottom = tokens.auroraSidebarShellBottom;
        auroraSidebarTopLip = tokens.auroraSidebarTopLip;
        auroraSidebarOuterRim = tokens.auroraSidebarOuterRim;
        auroraSidebarInnerRim = tokens.auroraSidebarInnerRim;
        auroraSidebarEdgeAura = tokens.auroraSidebarEdgeAura;
        auroraSidebarCornerBloom = tokens.auroraSidebarCornerBloom;
        knobRim = tokens.knobRim;
        knobGlow = tokens.knobGlow;

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

    static bool isAuroraTheme()
    {
        return currentThemeId_ == ThemeId::Aurora;
    }

    static bool isOverdoseTheme()
    {
        return currentThemeId_ == ThemeId::Overdose;
    }

    static juce::Colour auroraTrackAccent(int trackIndex)
    {
        static constexpr juce::uint32 colours[] = {
            Aurora::Colors::Cyan,
            Aurora::Colors::Violet,
            Aurora::Colors::NeonGreen,
            Aurora::Colors::Magenta,
            Aurora::Colors::ElectricBlue,
            Aurora::Colors::Warning
        };
        static constexpr int colourCount = static_cast<int>(sizeof(colours) / sizeof(colours[0]));
        return juce::Colour { colours[trackIndex % colourCount] };
    }

    static void drawAuroraGlow(juce::Graphics& g,
                               const juce::Rectangle<float>& bounds,
                               juce::Colour glow,
                               float alpha = 1.0f,
                               float radiusScale = 1.0f)
    {
        auto glowBounds = bounds.expanded(juce::jmax(1.0f, 8.0f * radiusScale));
        juce::Path glowPath;
        glowPath.addRoundedRectangle(glowBounds, currentThemeStyle().panelRadius + 8.0f * radiusScale);

        juce::DropShadow ds(glow.withMultipliedAlpha(0.42f * alpha),
                            juce::roundToInt(18.0f * radiusScale),
                            {});
        ds.drawForPath(g, glowPath);
    }

    static void fillAuroraGlass(juce::Graphics& g,
                                const juce::Rectangle<float>& bounds,
                                float radius,
                                float lightIntensity = 1.0f)
    {
        const auto light = juce::jlimit(0.0f, 1.0f, lightIntensity);
        juce::Path shape;
        if (radius > 0.0f)
            shape.addRoundedRectangle(bounds, radius);
        else
            shape.addRectangle(bounds);

        const auto traySideGlow = juce::Colour { Aurora::Colors::TraySideGlow };
        const auto bodyTop = juce::Colour(0xFF091827).interpolatedWith(juce::Colour(0xFF0E1B2A), light);
        const auto bodyMid = juce::Colour(0xFF071522).interpolatedWith(juce::Colour(0xFF0A1421), light);
        const auto bodyBottom = juce::Colour(0xFF030A12).interpolatedWith(juce::Colour(0xFF040B13), light);

        // 精确3色阶渐变：0%深蓝黑 → 35%深蓝 → 100%极深黑蓝
        juce::ColourGradient body(bodyTop,
                                  bounds.getX(),
                                  bounds.getY(),
                                  bodyBottom,
                                  bounds.getX(),
                                  bounds.getBottom(),
                                  false);
        body.addColour(0.35, bodyMid);
        g.setGradientFill(body);
        g.fillPath(shape);

        juce::Graphics::ScopedSaveState clipState(g);
        g.reduceClipRegion(shape);

        // 顶部冷蓝洗光：短而克制，避免灰白高光带
        const auto topWashBand = bounds.withHeight(bounds.getHeight() * 0.12f);
        juce::ColourGradient topWash(juce::Colour(0xFF5A9ACA).withAlpha(0.008f * light),
                                     topWashBand.getCentreX(),
                                     topWashBand.getY(),
                                     juce::Colours::transparentBlack,
                                     topWashBand.getCentreX(),
                                     topWashBand.getBottom(),
                                     false);
        g.setGradientFill(topWash);
        g.fillRect(topWashBand);

        juce::ColourGradient sideAura(traySideGlow.withAlpha(0.010f * light),
                                      bounds.getX() + bounds.getWidth() * 0.10f,
                                      bounds.getY() + bounds.getHeight() * 0.18f,
                                      juce::Colours::transparentBlack,
                                      bounds.getRight(),
                                      bounds.getY() + bounds.getHeight() * 0.72f,
                                      true);
        g.setGradientFill(sideAura);
        g.fillRect(bounds);

        // 底部蓝色环境光：覆盖78-100%H，保留深色空间感
        const auto bottomGlowBand = bounds.withTrimmedTop(bounds.getHeight() * 0.78f);
        juce::ColourGradient bottomGlow(juce::Colour(0xFF207EB8).withAlpha(0.030f * light),
                                        bottomGlowBand.getCentreX(),
                                        bottomGlowBand.getY(),
                                        juce::Colours::transparentBlack,
                                        bottomGlowBand.getCentreX(),
                                        bottomGlowBand.getBottom(),
                                        false);
        bottomGlow.addColour(0.45, juce::Colour(0xFF207EB8).withAlpha(0.010f * light));
        g.setGradientFill(bottomGlow);
        g.fillRect(bottomGlowBand);
    }

    static void fillAuroraSidebarShell(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        juce::Path shape;
        if (radius > 0.0f)
            shape.addRoundedRectangle(bounds, radius);
        else
            shape.addRectangle(bounds);

        const float edgeAuraWidth = juce::jmax(6.0f, bounds.getWidth() * 0.085f);
        const float edgeAuraInsetY = bounds.getHeight() * 0.055f;
        const auto leftAuraBounds = juce::Rectangle<float>(bounds.getX() - edgeAuraWidth * 0.18f,
                                                           bounds.getY() + edgeAuraInsetY,
                                                           edgeAuraWidth,
                                                           bounds.getHeight() - edgeAuraInsetY * 2.0f);
        juce::ColourGradient leftAura(auroraSidebarEdgeAura.withAlpha(0.14f),
                                      leftAuraBounds.getX(),
                                      leftAuraBounds.getCentreY(),
                                      juce::Colours::transparentBlack,
                                      leftAuraBounds.getRight(),
                                      leftAuraBounds.getCentreY(),
                                      false);
        leftAura.addColour(0.30, auroraSidebarEdgeAura.withAlpha(0.07f));
        g.setGradientFill(leftAura);
        g.fillRoundedRectangle(leftAuraBounds, juce::jmax(0.0f, radius - 3.0f));

        const auto rightAuraBounds = juce::Rectangle<float>(bounds.getRight() - edgeAuraWidth * 0.82f,
                                                            bounds.getY() + edgeAuraInsetY,
                                                            edgeAuraWidth,
                                                            bounds.getHeight() - edgeAuraInsetY * 2.0f);
        juce::ColourGradient rightAura(auroraSidebarEdgeAura.withAlpha(0.13f),
                                       rightAuraBounds.getX(),
                                       rightAuraBounds.getCentreY(),
                                       juce::Colours::transparentBlack,
                                       rightAuraBounds.getRight(),
                                       rightAuraBounds.getCentreY(),
                                       false);
        rightAura.addColour(0.32, auroraSidebarEdgeAura.withAlpha(0.06f));
        g.setGradientFill(rightAura);
        g.fillRoundedRectangle(rightAuraBounds, juce::jmax(0.0f, radius - 3.0f));

        juce::ColourGradient shell(auroraSidebarShellTop,
                                   bounds.getX(),
                                   bounds.getY(),
                                   auroraSidebarShellBottom,
                                   bounds.getX(),
                                   bounds.getBottom(),
                                   false);
        shell.addColour(0.16, auroraSidebarShellTop.interpolatedWith(auroraSidebarShellMid, 0.34f));
        shell.addColour(0.42, auroraSidebarShellMid);
        shell.addColour(0.74, auroraSidebarShellMid.interpolatedWith(auroraSidebarShellBottom, 0.38f));
        g.setGradientFill(shell);
        g.fillPath(shape);

        juce::Graphics::ScopedSaveState clipState(g);
        g.reduceClipRegion(shape);

        const auto topLipBand = bounds.withHeight(bounds.getHeight() * 0.10f);
        juce::ColourGradient topLip(auroraSidebarTopLip.withAlpha(0.06f),
                                    topLipBand.getCentreX(),
                                    topLipBand.getY(),
                                    juce::Colours::transparentWhite,
                                    topLipBand.getCentreX(),
                                    topLipBand.getBottom(),
                                    false);
        g.setGradientFill(topLip);
        g.fillRect(topLipBand);

        const auto leftEdgeVeilBounds = juce::Rectangle<float>(bounds.getX() + 1.0f,
                                                               bounds.getY() + bounds.getHeight() * 0.10f,
                                                               juce::jmax(4.0f, bounds.getWidth() * 0.055f),
                                                               bounds.getHeight() * 0.76f);
        juce::ColourGradient leftEdgeVeil(auroraSidebarEdgeAura.withAlpha(0.09f),
                                          leftEdgeVeilBounds.getX(),
                                          leftEdgeVeilBounds.getCentreY(),
                                          juce::Colours::transparentBlack,
                                          leftEdgeVeilBounds.getRight(),
                                          leftEdgeVeilBounds.getCentreY(),
                                          false);
        leftEdgeVeil.addColour(0.26, auroraSidebarEdgeAura.withAlpha(0.05f));
        g.setGradientFill(leftEdgeVeil);
        g.fillRect(leftEdgeVeilBounds);

        const auto rightEdgeVeilBounds = juce::Rectangle<float>(bounds.getRight() - juce::jmax(4.0f, bounds.getWidth() * 0.055f) - 1.0f,
                                                                bounds.getY() + bounds.getHeight() * 0.10f,
                                                                juce::jmax(4.0f, bounds.getWidth() * 0.055f),
                                                                bounds.getHeight() * 0.76f);
        juce::ColourGradient rightEdgeVeil(juce::Colours::transparentBlack,
                                           rightEdgeVeilBounds.getX(),
                                           rightEdgeVeilBounds.getCentreY(),
                                           auroraSidebarEdgeAura.withAlpha(0.08f),
                                           rightEdgeVeilBounds.getRight(),
                                           rightEdgeVeilBounds.getCentreY(),
                                           false);
        rightEdgeVeil.addColour(0.74, auroraSidebarEdgeAura.withAlpha(0.05f));
        g.setGradientFill(rightEdgeVeil);
        g.fillRect(rightEdgeVeilBounds);

        const auto topLeftCornerBloomBounds = juce::Rectangle<float>(bounds.getX() - bounds.getWidth() * 0.06f,
                                                                     bounds.getY() - bounds.getWidth() * 0.05f,
                                                                     bounds.getWidth() * 0.30f,
                                                                     bounds.getWidth() * 0.24f);
        g.setGradientFill(juce::ColourGradient(auroraSidebarCornerBloom.withAlpha(0.05f),
                                               topLeftCornerBloomBounds.getX() + topLeftCornerBloomBounds.getWidth() * 0.24f,
                                               topLeftCornerBloomBounds.getY() + topLeftCornerBloomBounds.getHeight() * 0.22f,
                                               juce::Colours::transparentBlack,
                                               topLeftCornerBloomBounds.getRight(),
                                               topLeftCornerBloomBounds.getBottom(),
                                               true));
        g.fillEllipse(topLeftCornerBloomBounds);

        const auto topRightCornerBloomBounds = juce::Rectangle<float>(bounds.getRight() - bounds.getWidth() * 0.24f,
                                                                      bounds.getY() - bounds.getWidth() * 0.05f,
                                                                      bounds.getWidth() * 0.30f,
                                                                      bounds.getWidth() * 0.24f);
        g.setGradientFill(juce::ColourGradient(auroraSidebarCornerBloom.withAlpha(0.045f),
                                               topRightCornerBloomBounds.getRight() - topRightCornerBloomBounds.getWidth() * 0.24f,
                                               topRightCornerBloomBounds.getY() + topRightCornerBloomBounds.getHeight() * 0.22f,
                                               juce::Colours::transparentBlack,
                                               topRightCornerBloomBounds.getX(),
                                               topRightCornerBloomBounds.getBottom(),
                                               true));
        g.fillEllipse(topRightCornerBloomBounds);

    }

    static void fillSoftTimelineCanvas(juce::Graphics& g,
                                       const juce::Rectangle<float>& bounds,
                                       float radius,
                                       juce::Colour top,
                                       juce::Colour middle,
                                       juce::Colour bottom,
                                       double middleStop = 0.46)
    {
        juce::Path shape;
        if (radius > 0.0f)
            shape.addRoundedRectangle(bounds, radius);
        else
            shape.addRectangle(bounds);

        juce::ColourGradient field(top,
                                   bounds.getX(),
                                   bounds.getY(),
                                   bottom,
                                   bounds.getX(),
                                   bounds.getBottom(),
                                   false);
        field.addColour(juce::jlimit(0.0, 1.0, middleStop), middle);
        g.setGradientFill(field);
        g.fillPath(shape);
    }

    static void fillAuroraTimelineBackground(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        const auto top = pianoRollBackground.brighter(0.045f);
        const auto middle = pianoRollBackground.interpolatedWith(backgroundDark, 0.08f);
        const auto bottom = pianoRollBackground.darker(0.075f);
        fillSoftTimelineCanvas(g, bounds, radius, top, middle, bottom, 0.44);
    }

    static void fillTrackPanelBackground(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        const auto top = trackPanelBackground.brighter(0.045f);
        const auto middle = trackPanelBackground.interpolatedWith(backgroundDark, 0.08f);
        const auto bottom = trackPanelBackground.darker(0.075f);
        fillSoftTimelineCanvas(g, bounds, radius, top, middle, bottom, 0.44);
    }

    static void fillMistedTimelineField(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        const auto themeId = currentThemeId();
        jassert(themeId != ThemeId::Overdose);
        const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
        const auto top = isBlueBreeze ? juce::Colour { BlueBreeze::Colors::FieldFogTop } : pianoRollBackground.brighter(0.050f);
        const auto middle = isBlueBreeze ? juce::Colour { BlueBreeze::Colors::FieldFogMid } : pianoRollBackground.interpolatedWith(backgroundMedium, 0.12f);
        const auto bottom = isBlueBreeze ? juce::Colour { BlueBreeze::Colors::FieldFogBottom } : pianoRollBackground.darker(0.080f);
        fillSoftTimelineCanvas(g, bounds, radius, top, middle, bottom, isBlueBreeze ? 0.50 : 0.42);

        juce::Path shape;
        if (radius > 0.0f)
            shape.addRoundedRectangle(bounds, radius);
        else
            shape.addRectangle(bounds);

        const auto depthColour = (isBlueBreeze ? juce::Colour { BlueBreeze::Colors::GraphBgDeep } : backgroundDark).withAlpha(isBlueBreeze ? 0.14f : 0.30f);
        const auto sourceColour = (isBlueBreeze ? juce::Colour { BlueBreeze::Colors::SourceLight } : glassHighlight).withAlpha(isBlueBreeze ? 0.085f : 0.16f);
        const auto coolAirColour = juce::Colour { BlueBreeze::Colors::AccentBlue }.withAlpha(0.010f);

        juce::Graphics::ScopedSaveState clipState(g);
        g.reduceClipRegion(shape);

        juce::ColourGradient depth(juce::Colours::transparentBlack,
                                   bounds.getCentreX(),
                                   bounds.getY() + bounds.getHeight() * 0.36f,
                                   depthColour,
                                   bounds.getCentreX(),
                                   bounds.getBottom(),
                                   false);
        g.setGradientFill(depth);
        g.fillRect(bounds);

        juce::ColourGradient source(sourceColour,
                                    bounds.getX() + bounds.getWidth() * 0.10f,
                                    bounds.getY() + bounds.getHeight() * 0.10f,
                                    juce::Colours::transparentWhite,
                                    bounds.getRight(),
                                    bounds.getBottom(),
                                    true);
        g.setGradientFill(source);
        g.fillRect(bounds);

        if (isBlueBreeze)
        {
            juce::ColourGradient coolAir(coolAirColour,
                                         bounds.getX() + bounds.getWidth() * 0.16f,
                                         bounds.getY() + bounds.getHeight() * 0.18f,
                                         juce::Colours::transparentBlack,
                                         bounds.getRight(),
                                         bounds.getY() + bounds.getHeight() * 0.72f,
                                         true);
            g.setGradientFill(coolAir);
            g.fillRect(bounds);

        }
    }

    static void fillBlueBreezeSoftPanel(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        juce::Path shape;
        if (radius > 0.0f)
            shape.addRoundedRectangle(bounds, radius);
        else
            shape.addRectangle(bounds);

        juce::DropShadow ambientShadow(juce::Colour { BlueBreeze::Colors::ControlShadow }.withAlpha(0.12f),
                                       18,
                                       { 0, 4 });
        ambientShadow.drawForPath(g, shape);

        juce::ColourGradient base(juce::Colour { BlueBreeze::Colors::PanelTop }.interpolatedWith(juce::Colour { BlueBreeze::Colors::CanvasTop }, 0.10f),
                                  bounds.getX() + bounds.getWidth() * 0.08f,
                                  bounds.getY(),
                                  juce::Colour { BlueBreeze::Colors::PanelBottom }.darker(0.01f),
                                  bounds.getRight(),
                                  bounds.getBottom(),
                                  false);
        base.addColour(0.34, juce::Colour { BlueBreeze::Colors::SourceLight }.interpolatedWith(juce::Colour { BlueBreeze::Colors::PanelTop }, 0.58f));
        base.addColour(0.72, juce::Colour { BlueBreeze::Colors::TrayTop }.brighter(0.02f).interpolatedWith(juce::Colour { BlueBreeze::Colors::TrayBottom }.darker(0.01f), 0.10f));
        g.setGradientFill(base);
        g.fillPath(shape);

        juce::ColourGradient diagonalLight(juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(0.15f),
                                           bounds.getX() + bounds.getWidth() * 0.08f,
                                           bounds.getY() + bounds.getHeight() * 0.06f,
                                           juce::Colours::transparentWhite,
                                           bounds.getRight(),
                                           bounds.getBottom(),
                                           true);
        g.setGradientFill(diagonalLight);
        g.fillPath(shape);

        juce::ColourGradient innerShade(juce::Colours::transparentBlack,
                                        bounds.getX() + bounds.getWidth() * 0.40f,
                                        bounds.getY() + bounds.getHeight() * 0.30f,
                                        juce::Colour { BlueBreeze::Colors::PanelInset }.withAlpha(0.12f),
                                        bounds.getRight(),
                                        bounds.getBottom(),
                                        true);
        g.setGradientFill(innerShade);
        g.fillPath(shape);

        juce::ColourGradient bottomDepth(juce::Colours::transparentBlack,
                                         bounds.getX() + bounds.getWidth() * 0.60f,
                                         bounds.getY() + bounds.getHeight() * 0.50f,
                                         juce::Colour { BlueBreeze::Colors::GraphBgDeep }.withAlpha(0.05f),
                                         bounds.getCentreX(),
                                         bounds.getBottom(),
                                         false);
        g.setGradientFill(bottomDepth);
        g.fillPath(shape);

        juce::ColourGradient contourDepth(juce::Colours::transparentBlack,
                                          bounds.getX() + bounds.getWidth() * 0.70f,
                                          bounds.getY() + bounds.getHeight() * 0.38f,
                                          juce::Colour { BlueBreeze::Colors::GraphBgDeep }.withAlpha(0.09f),
                                          bounds.getRight(),
                                          bounds.getBottom(),
                                          true);
        g.setGradientFill(contourDepth);
        g.fillPath(shape);

        g.setColour(juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(0.16f));
        g.drawLine(bounds.getX() + radius,
                   bounds.getY() + 1.0f,
                   bounds.getRight() - radius,
                   bounds.getY() + 1.0f,
                   1.0f);
    }

    static void fillBlueBreezeTrackCard(juce::Graphics& g,
                                        const juce::Rectangle<float>& bounds,
                                        float radius,
                                        bool active,
                                        juce::Colour tint)
    {
        juce::Path shape;
        shape.addRoundedRectangle(bounds, radius);

        juce::DropShadow cardShadow(juce::Colour { BlueBreeze::Colors::ControlShadow }.withAlpha(active ? 0.17f : 0.12f),
                                    active ? 16 : 12,
                                    active ? juce::Point<int> { 0, 5 } : juce::Point<int> { 0, 3 });
        cardShadow.drawForPath(g, shape);

        juce::ColourGradient body(juce::Colour { BlueBreeze::Colors::ControlTop }.interpolatedWith(juce::Colour { BlueBreeze::Colors::SourceLight }, 0.18f),
                                  bounds.getX() + bounds.getWidth() * 0.10f,
                                  bounds.getY(),
                                  juce::Colour { BlueBreeze::Colors::ControlBottom }.interpolatedWith(juce::Colour { BlueBreeze::Colors::PanelBottom }, 0.32f),
                                  bounds.getRight(),
                                  bounds.getBottom(),
                                  false);
        body.addColour(0.36, juce::Colour { BlueBreeze::Colors::PanelTop }.interpolatedWith(tint, active ? 0.08f : 0.04f));
        body.addColour(0.74, juce::Colour { BlueBreeze::Colors::PanelInset }.interpolatedWith(tint, active ? 0.10f : 0.05f));
        g.setGradientFill(body);
        g.fillPath(shape);

        juce::ColourGradient source(juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(active ? 0.22f : 0.16f),
                                    bounds.getX() + bounds.getWidth() * 0.10f,
                                    bounds.getY() + bounds.getHeight() * 0.08f,
                                    juce::Colours::transparentWhite,
                                    bounds.getRight(),
                                    bounds.getBottom(),
                                    true);
        g.setGradientFill(source);
        g.fillPath(shape);

        juce::ColourGradient depth(juce::Colours::transparentBlack,
                                   bounds.getX() + bounds.getWidth() * 0.60f,
                                   bounds.getY() + bounds.getHeight() * 0.52f,
                                   juce::Colour { BlueBreeze::Colors::GraphBgDeep }.withAlpha(active ? 0.10f : 0.07f),
                                   bounds.getCentreX(),
                                   bounds.getBottom(),
                                   false);
        g.setGradientFill(depth);
        g.fillPath(shape);

        juce::ColourGradient tintWash(tint.withAlpha(active ? 0.08f : 0.04f),
                                      bounds.getX(),
                                      bounds.getY() + bounds.getHeight() * 0.25f,
                                      juce::Colours::transparentBlack,
                                      bounds.getRight(),
                                      bounds.getBottom(),
                                      true);
        g.setGradientFill(tintWash);
        g.fillPath(shape);

        if (active)
        {
            juce::ColourGradient activeResponse(juce::Colour { BlueBreeze::Colors::AccentBlue }.withAlpha(0.05f),
                                                bounds.getCentreX(),
                                                bounds.getY(),
                                                juce::Colours::transparentBlack,
                                                bounds.getRight(),
                                                bounds.getBottom(),
                                                true);
            g.setGradientFill(activeResponse);
            g.fillPath(shape);
        }

        g.setColour(juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(active ? 0.18f : 0.12f));
        g.drawLine(bounds.getX() + radius,
                   bounds.getY() + 1.0f,
                   bounds.getRight() - radius,
                   bounds.getY() + 1.0f,
                   1.0f);

        g.setColour((active ? juce::Colour { BlueBreeze::Colors::AccentBlue } : juce::Colour { BlueBreeze::Colors::PanelBorder })
                        .withAlpha(active ? 0.52f : 0.30f));
        g.strokePath(shape, juce::PathStrokeType(active ? 1.25f : 1.0f));

        g.setColour(juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(active ? 0.10f : 0.07f));
        g.strokePath(shape, juce::PathStrokeType(0.8f));
    }

    static void fillBlueBreezeTray(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        juce::Path shape;
        if (radius > 0.0f)
            shape.addRoundedRectangle(bounds, radius);
        else
            shape.addRectangle(bounds);

        juce::DropShadow trayShadow(juce::Colour { BlueBreeze::Colors::ControlShadow }.withAlpha(0.10f),
                                    14,
                                    { 0, 3 });
        trayShadow.drawForPath(g, shape);

        juce::ColourGradient base(juce::Colour { BlueBreeze::Colors::TrayTop }.interpolatedWith(juce::Colour { BlueBreeze::Colors::CanvasTop }, 0.08f),
                                  bounds.getX(),
                                  bounds.getY(),
                                  juce::Colour { BlueBreeze::Colors::TrayBottom }.interpolatedWith(juce::Colour { BlueBreeze::Colors::CanvasBottom }, 0.10f),
                                  bounds.getRight(),
                                  bounds.getBottom(),
                                  false);
        base.addColour(0.34, juce::Colour { BlueBreeze::Colors::SourceLight }.interpolatedWith(juce::Colour { BlueBreeze::Colors::TrayTop }, 0.64f));
        base.addColour(0.72, juce::Colour { BlueBreeze::Colors::TrayInset }.interpolatedWith(juce::Colour { BlueBreeze::Colors::TrayTop }, 0.12f));
        g.setGradientFill(base);
        g.fillPath(shape);

        juce::ColourGradient source(juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(0.16f),
                                    bounds.getX() + bounds.getWidth() * 0.14f,
                                    bounds.getY() + bounds.getHeight() * 0.10f,
                                    juce::Colours::transparentWhite,
                                    bounds.getRight(),
                                    bounds.getBottom(),
                                    true);
        g.setGradientFill(source);
        g.fillPath(shape);

        juce::ColourGradient faceLift(juce::Colour { BlueBreeze::Colors::CanvasTop }.withAlpha(0.11f),
                                      bounds.getX() + bounds.getWidth() * 0.08f,
                                      bounds.getY() + bounds.getHeight() * 0.04f,
                                      juce::Colours::transparentWhite,
                                      bounds.getX() + bounds.getWidth() * 0.52f,
                                      bounds.getY() + bounds.getHeight() * 0.32f,
                                      true);
        g.setGradientFill(faceLift);
        g.fillPath(shape);

        juce::ColourGradient lower(juce::Colours::transparentBlack,
                                   bounds.getCentreX(),
                                   bounds.getCentreY(),
                                   juce::Colour { BlueBreeze::Colors::GraphBgDeep }.withAlpha(0.11f),
                                   bounds.getRight(),
                                   bounds.getBottom(),
                                   true);
        g.setGradientFill(lower);
        g.fillPath(shape);

        juce::ColourGradient lowerInset(juce::Colours::transparentBlack,
                                        bounds.getCentreX(),
                                        bounds.getY() + bounds.getHeight() * 0.56f,
                                        juce::Colour { BlueBreeze::Colors::PanelInset }.withAlpha(0.08f),
                                        bounds.getCentreX(),
                                        bounds.getBottom(),
                                        false);
        g.setGradientFill(lowerInset);
        g.fillPath(shape);

        g.setColour(juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(0.18f));
        g.drawLine(bounds.getX() + radius,
                   bounds.getY() + 1.0f,
                   bounds.getRight() - radius,
                   bounds.getY() + 1.0f,
                   1.0f);
    }

    static void fillBlueBreezeDisplayWell(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        juce::Path shape;
        shape.addRoundedRectangle(bounds, radius);

        juce::DropShadow insetCast(displayWellBottom.withAlpha(0.58f),
                                   18,
                                   { 0, 5 });
        insetCast.drawForPath(g, shape);

        juce::ColourGradient well(displayWellTop,
                                  bounds.getX(),
                                  bounds.getY(),
                                  displayWellBottom,
                                  bounds.getX(),
                                  bounds.getBottom(),
                                  false);
        well.addColour(0.34, juce::Colour { BlueBreeze::Colors::DisplayMid });
        well.addColour(0.74, displayWellBottom.brighter(0.035f));
        g.setGradientFill(well);
        g.fillPath(shape);

        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(shape);

            juce::ColourGradient innerTop(juce::Colours::black.withAlpha(0.56f),
                                          bounds.getX(),
                                          bounds.getY(),
                                          juce::Colours::transparentBlack,
                                          bounds.getX(),
                                          bounds.getY() + bounds.getHeight() * 0.34f,
                                          false);
            g.setGradientFill(innerTop);
            g.fillRect(bounds);

            juce::ColourGradient blueResponse(juce::Colour { BlueBreeze::Colors::AccentBlue }.withAlpha(0.12f),
                                              bounds.getX() + bounds.getWidth() * 0.20f,
                                              bounds.getY() + bounds.getHeight() * 0.18f,
                                              juce::Colours::transparentBlack,
                                              bounds.getRight(),
                                              bounds.getBottom(),
                                              true);
            g.setGradientFill(blueResponse);
            g.fillRect(bounds);

            juce::ColourGradient lowerBloom(juce::Colours::transparentBlack,
                                            bounds.getCentreX(),
                                            bounds.getY() + bounds.getHeight() * 0.58f,
                                            juce::Colour { BlueBreeze::Colors::DisplayEdge }.withAlpha(0.14f),
                                            bounds.getCentreX(),
                                            bounds.getBottom(),
                                            false);
            g.setGradientFill(lowerBloom);
            g.fillRect(bounds);

            g.setColour(juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(0.13f));
            g.drawLine(bounds.getX() + radius,
                       bounds.getY() + 1.0f,
                       bounds.getRight() - radius,
                       bounds.getY() + 1.0f,
                       1.0f);
        }

        g.setColour(displayWellEdge.withAlpha(0.90f));
        g.strokePath(shape, juce::PathStrokeType(1.2f));

        g.setColour(juce::Colour { BlueBreeze::Colors::DisplayGlow }.withAlpha(0.28f));
        g.strokePath(shape, juce::PathStrokeType(2.2f));
    }

    static void drawBlueBreezePianoKnob(juce::Graphics& g,
                                        juce::Rectangle<float> bounds,
                                        float normalisedValue,
                                        bool highlighted,
                                        float rotaryStartAngle = juce::MathConstants<float>::pi * 1.25f,
                                        float rotaryEndAngle = juce::MathConstants<float>::pi * 2.75f)
    {
        const auto clampedValue = juce::jlimit(0.0f, 1.0f, normalisedValue);
        const auto insetBounds = bounds.reduced(2.0f);
        const auto side = juce::jmin(insetBounds.getWidth(), insetBounds.getHeight());
        const auto knobBounds = insetBounds.withSizeKeepingCentre(side * 0.72f, side * 0.72f);
        const auto centre = knobBounds.getCentre();
        const auto radius = knobBounds.getWidth() * 0.5f;
        const auto ringRadius = radius + juce::jmax(5.0f, side * 0.085f);
        const auto angle = rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle) * clampedValue;

        juce::Path ringPath;
        ringPath.addCentredArc(centre.x, centre.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour { BlueBreeze::Colors::KnobTrack }.withAlpha(0.34f));
        g.strokePath(ringPath, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        static constexpr int tickCount = 34;
        for (int i = 0; i < tickCount; ++i)
        {
            const auto t = static_cast<float>(i) / static_cast<float>(tickCount - 1);
            const auto tickAngle = rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle) * t;
            const auto isMajor = (i % 4) == 0;
            const auto inner = ringRadius + (isMajor ? 1.0f : 2.5f);
            const auto outer = ringRadius + (isMajor ? 7.0f : 5.0f);
            juce::Line<float> tick {
                centre.x + inner * std::sin(tickAngle),
                centre.y - inner * std::cos(tickAngle),
                centre.x + outer * std::sin(tickAngle),
                centre.y - outer * std::cos(tickAngle)
            };
            g.setColour(juce::Colour { BlueBreeze::Colors::KnobTrack }.withAlpha(isMajor ? 0.48f : 0.28f));
            g.drawLine(tick, isMajor ? 1.15f : 0.9f);
        }

        juce::Path valueArc;
        valueArc.addCentredArc(centre.x, centre.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, angle, true);
        g.setColour(accent.withAlpha(highlighted ? 0.66f : 0.40f));
        g.strokePath(valueArc, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path knobPath;
        knobPath.addEllipse(knobBounds);
        if (highlighted)
        {
            juce::DropShadow responseGlow(juce::Colour { BlueBreeze::Colors::KnobGlow }.withAlpha(0.20f),
                                          18,
                                          {});
            responseGlow.drawForPath(g, knobPath);
        }

        juce::DropShadow shadow(juce::Colour { BlueBreeze::Colors::KnobShadow }.withAlpha(highlighted ? 0.46f : 0.34f),
                                highlighted ? 20 : 16,
                                { 0, highlighted ? 6 : 4 });
        shadow.drawForPath(g, knobPath);

        juce::ColourGradient body(juce::Colour { BlueBreeze::Colors::KnobBodyLight },
                                  knobBounds.getX() + knobBounds.getWidth() * 0.24f,
                                  knobBounds.getY() + knobBounds.getHeight() * 0.10f,
                                  juce::Colour { BlueBreeze::Colors::KnobBody },
                                  knobBounds.getRight(),
                                  knobBounds.getBottom(),
                                  false);
        body.addColour(0.38, juce::Colour { 0xFF15181A });
        body.addColour(0.76, juce::Colour { 0xFF040506 });
        g.setGradientFill(body);
        g.fillPath(knobPath);

        juce::ColourGradient gloss(juce::Colour { BlueBreeze::Colors::KnobHighlight }.withAlpha(highlighted ? 0.22f : 0.14f),
                                   knobBounds.getX() + knobBounds.getWidth() * 0.28f,
                                   knobBounds.getY() + knobBounds.getHeight() * 0.12f,
                                   juce::Colours::transparentWhite,
                                   knobBounds.getRight(),
                                   knobBounds.getCentreY(),
                                   true);
        g.setGradientFill(gloss);
        g.fillPath(knobPath);

        juce::ColourGradient rimLight(juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(highlighted ? 0.16f : 0.09f),
                                      knobBounds.getX() + knobBounds.getWidth() * 0.20f,
                                      knobBounds.getY() + knobBounds.getHeight() * 0.10f,
                                      juce::Colours::transparentWhite,
                                      knobBounds.getCentreX(),
                                      knobBounds.getBottom(),
                                      true);
        g.setGradientFill(rimLight);
        g.strokePath(knobPath, juce::PathStrokeType(highlighted ? 1.15f : 0.85f));

        g.setColour(juce::Colour { BlueBreeze::Colors::KnobEdge }.withAlpha(highlighted ? 0.64f : 0.46f));
        g.strokePath(knobPath, juce::PathStrokeType(highlighted ? 1.35f : 1.0f));

        const auto dotDistance = radius * 0.62f;
        const auto dotRadius = juce::jmax(2.1f, radius * 0.070f);
        const auto dotX = centre.x + dotDistance * std::sin(angle);
        const auto dotY = centre.y - dotDistance * std::cos(angle);
        g.setColour(juce::Colours::black.withAlpha(0.30f));
        g.fillEllipse(dotX - dotRadius + 0.7f, dotY - dotRadius + 1.0f, dotRadius * 2.0f, dotRadius * 2.0f);
        g.setColour(juce::Colour { BlueBreeze::Colors::KnobIndicator }.withAlpha(0.96f));
        g.fillEllipse(dotX - dotRadius, dotY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
    }

    static void drawAuroraGlassFrame(juce::Graphics& g,
                                     const juce::Rectangle<float>& bounds,
                                     float radius,
                                     bool strong = false,
                                     float lightIntensity = 1.0f)
    {
        const auto light = juce::jlimit(0.0f, 1.0f, lightIntensity);
        const auto framePresence = light;
        // 外描边：1.25px rgba(67,153,215,0.55)
        const auto outerColor = juce::Colour(0xFF4399D7).withAlpha((strong ? 0.65f : 0.55f) * framePresence);
        const auto fullOuterStroke = strong ? 1.35f : 1.25f;
        const auto outerStroke = 0.75f + (fullOuterStroke - 0.75f) * light;
        g.setColour(outerColor);
        if (radius > 0.0f)
            g.drawRoundedRectangle(bounds.reduced(0.5f), radius, outerStroke);
        else
            g.drawRect(bounds.reduced(0.5f), outerStroke);

        // 内描边：0.9px rgba(168,220,255,0.30)，内缩1.5px与外套边形成清晰双线
        if (radius > 1.5f)
        {
            const auto innerColor = juce::Colour(0xFFA8DCFF).withAlpha((strong ? 0.38f : 0.30f) * light);
            const auto innerStroke = 0.55f + 0.35f * light;
            g.setColour(innerColor);
            g.drawRoundedRectangle(bounds.reduced(1.80f), juce::jmax(0.0f, radius - 1.30f), innerStroke);
        }

        // 顶沿高光：2px rgba(180,230,255,0.38)
        const auto topColor = juce::Colour(0xFFB4E6FF).withAlpha((strong ? 0.48f : 0.38f) * light);
        g.setColour(topColor);
        const auto topInset = juce::jmin(radius, bounds.getWidth() * 0.24f);
        g.drawLine(bounds.getX() + topInset,
                   bounds.getY() + 1.0f,
                   bounds.getRight() - topInset,
                   bounds.getY() + 1.0f,
                   0.60f + 0.60f * light);

        // 顶高光向下4px渐隐
        juce::ColourGradient topFade(topColor.withAlpha(0.24f * light),
                                     bounds.getX() + topInset,
                                     bounds.getY() + 2.2f,
                                     juce::Colours::transparentBlack,
                                     bounds.getX() + topInset,
                                     bounds.getY() + 6.5f,
                                     false);
        g.setGradientFill(topFade);
        g.fillRect(bounds.getX() + topInset, bounds.getY() + 2.2f,
                   bounds.getWidth() - topInset * 2.0f, 4.3f);
    }

    static void drawAuroraSidebarShellFrame(juce::Graphics& g,
                                            const juce::Rectangle<float>& bounds,
                                            float radius)
    {
        g.setColour(auroraSidebarOuterRim.withAlpha(0.72f));
        if (radius > 0.0f)
            g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
        else
            g.drawRect(bounds.reduced(0.5f), 1.0f);

        if (radius > 1.5f)
        {
            g.setColour(auroraSidebarInnerRim.withAlpha(0.30f));
            g.drawRoundedRectangle(bounds.reduced(1.35f), juce::jmax(0.0f, radius - 0.9f), 0.80f);
        }

    }

    static void drawAuroraButtonChrome(juce::Graphics& g,
                                       const juce::Rectangle<float>& bounds,
                                       float radius,
                                       bool highlighted,
                                       bool down,
                                       bool active = false,
                                       juce::Colour glowColour = {},
                                       juce::Colour edgeColour = {},
                                       const juce::Path* shapeOverride = nullptr,
                                       float lightIntensity = 1.0f)
    {
        const auto light = juce::jlimit(0.0f, 1.0f, lightIntensity);
        const auto glowPresence = light * light;
        const auto edgePresence = 0.28f + 0.72f * light;
        const auto isPressed = down;
        const auto isActive = active || isPressed;
        const auto isHovered = highlighted && !isPressed;
        const auto stateFill = isActive ? auroraButtonActive : (isHovered ? auroraButtonHover : auroraButtonNormal);
        const auto lowLightFill = isActive ? juce::Colour(0xF0124678)
                                          : (isHovered ? juce::Colour(0xF01A364F)
                                                       : juce::Colour(0xF0172F46));
        const auto fill = lowLightFill.interpolatedWith(stateFill, light);
        const auto glow = glowColour.getAlpha() > 0 ? glowColour : (isActive ? knobGlow : panelGlow);
        const auto edge = edgeColour.getAlpha() > 0 ? edgeColour : (isActive ? knobGlow : glassEdge);

        juce::Path shape;
        if (shapeOverride != nullptr)
            shape = *shapeOverride;
        else if (radius > 0.0f)
            shape.addRoundedRectangle(bounds, radius);
        else
            shape.addRectangle(bounds);

        // 双层光晕：内层亮(halo core) + 外层柔(halo glow)，保持低亮度
        const auto fullInnerGlowRadius = isActive ? 15 : (isHovered ? 10 : 8);
        juce::DropShadow innerGlow(glow.withMultipliedAlpha((isActive ? 0.18f : (isHovered ? 0.14f : 0.035f)) * glowPresence),
                                   juce::roundToInt(4.0f + (static_cast<float>(fullInnerGlowRadius) - 4.0f) * light),
                                   {});
        innerGlow.drawForPath(g, shape);

        if ((isActive || isHovered) && light > 0.75f)
        {
            juce::Path outerHaloShape;
            if (radius > 0.0f)
                outerHaloShape.addRoundedRectangle(bounds.expanded(4.0f), radius + 4.0f);
            else
                outerHaloShape.addRectangle(bounds.expanded(4.0f));
            juce::DropShadow outerHalo(glow.withMultipliedAlpha(isActive ? 0.05f : 0.04f),
                                       isActive ? 24 : 18,
                                       {});
            outerHalo.drawForPath(g, outerHaloShape);
        }

        const auto topLight = fill.brighter((isPressed ? 0.02f : 0.04f) * light)
                                  .interpolatedWith(glassHighlight, (isActive ? 0.05f : 0.03f) * light);
        const auto midTone = fill.brighter((isHovered ? 0.02f : 0.005f) * light);
        const auto lowerTone = fill.darker(isPressed ? 0.22f : 0.10f);
        juce::ColourGradient chrome(topLight,
                                    bounds.getX(),
                                    bounds.getY(),
                                    lowerTone,
                                    bounds.getRight(),
                                    bounds.getBottom(),
                                    false);
        chrome.addColour(0.34, midTone);
        chrome.addColour(0.70, fill);
        g.setGradientFill(chrome);
        g.fillPath(shape);

        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(shape);

            juce::ColourGradient sourceLight(textPrimary.withAlpha((isActive ? 0.05f : 0.03f) * light),
                                             bounds.getX() + bounds.getWidth() * 0.12f,
                                             bounds.getY() + bounds.getHeight() * 0.08f,
                                             juce::Colours::white.withAlpha(0.0f),
                                             bounds.getRight(),
                                             bounds.getBottom(),
                                             true);
            g.setGradientFill(sourceLight);
            g.fillRect(bounds);

            // 纯冷蓝玻璃反光：使用高饱和蓝
            auto topBand = bounds.withHeight(bounds.getHeight() * 0.42f);
            const auto sheenBlue = juce::Colour { Aurora::Colors::KnobGlow }; // 高饱和蓝#1688FF
            const auto sheenColor = glassHighlight.interpolatedWith(sheenBlue, 0.20f);
            juce::ColourGradient topSheen(sheenColor.withMultipliedAlpha((isActive ? 0.20f : 0.15f) * light),
                                          topBand.getX(),
                                          topBand.getY(),
                                          juce::Colours::white.withAlpha(0.0f),
                                          topBand.getX(),
                                          topBand.getBottom(),
                                          false);
            g.setGradientFill(topSheen);
            g.fillRect(topBand);

            // 微棱高光：按钮顶部亮线，用冷白蓝色
            const auto bevelColor = sheenBlue.interpolatedWith(juce::Colours::white, 0.18f);
            g.setColour(bevelColor.withMultipliedAlpha((isActive ? 0.12f : 0.08f) * light));
            g.drawLine(bounds.getX() + radius * 0.55f, bounds.getY() + 1.5f,
                       bounds.getRight() - radius * 0.55f, bounds.getY() + 1.5f,
                       0.65f + 0.55f * light);

            auto bottomBand = bounds.withTop(bounds.getY() + bounds.getHeight() * 0.52f);
            const auto shadePresence = 0.25f + 0.75f * light;
            juce::ColourGradient bottomShade(juce::Colours::transparentBlack,
                                             bottomBand.getX(),
                                             bottomBand.getY(),
                                             juce::Colours::black.withAlpha((isPressed ? 0.56f : 0.44f) * shadePresence),
                                             bottomBand.getX(),
                                             bottomBand.getBottom(),
                                             false);
            g.setGradientFill(bottomShade);
            g.fillRect(bottomBand);

            g.setColour(edge.withMultipliedAlpha((isActive ? 0.36f : (isHovered ? 0.26f : 0.20f)) * edgePresence));
            g.strokePath(shape, juce::PathStrokeType(1.0f));
        }

        const auto fullStroke = isActive ? 1.45f : (isHovered ? 1.15f : 1.0f);
        const auto stroke = 0.72f + (fullStroke - 0.72f) * light;
        g.setColour(edge.withMultipliedAlpha((isActive ? 0.72f : (isHovered ? 0.60f : 0.50f)) * edgePresence));
        g.strokePath(shape, juce::PathStrokeType(stroke));

        // 边缘光扩散 - 所有状态可见但不形成白色光圈
        const auto fullGlowStroke = isActive ? 4.0f : (isHovered ? 3.0f : 2.0f);
        g.setColour(glow.withMultipliedAlpha((isActive ? 0.08f : (isHovered ? 0.055f : 0.030f)) * glowPresence));
        g.strokePath(shape, juce::PathStrokeType(0.75f + (fullGlowStroke - 0.75f) * light));

        // active/hover额外加强扩散
        if ((isActive || isHovered) && light > 0.75f)
        {
            g.setColour(glow.withMultipliedAlpha(isActive ? 0.05f : 0.025f));
            g.strokePath(shape, juce::PathStrokeType(isActive ? 7.0f : 4.5f));
        }

        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(shape);
            g.setColour(glassHighlight.withMultipliedAlpha((isActive ? 0.28f : 0.16f) * light));
            const auto topInset = juce::jmin(radius, bounds.getWidth() * 0.25f);
            g.drawLine(bounds.getX() + topInset, bounds.getY() + 1.0f,
                       bounds.getRight() - topInset, bounds.getY() + 1.0f,
                       0.65f + 0.35f * light);
        }
    }

    static void drawAuroraKnob(juce::Graphics& g,
                               const juce::Rectangle<float>& bounds,
                               float normalisedValue,
                               bool highlighted = false,
                               float rotaryStartAngle = juce::MathConstants<float>::pi * 1.25f,
                               float rotaryEndAngle = juce::MathConstants<float>::pi * 2.75f)
    {
        const auto clampedValue = juce::jlimit(0.0f, 1.0f, normalisedValue);
        const auto insetBounds = bounds.reduced(2.0f);
        const auto side = juce::jmin(insetBounds.getWidth(), insetBounds.getHeight());
        const auto knobBounds = insetBounds.withSizeKeepingCentre(side, side);
        const auto radius = juce::jmin(knobBounds.getWidth(), knobBounds.getHeight()) * 0.5f;
        const auto centre = knobBounds.getCentre();

        {
            const float alpha = highlighted ? 0.62f : 0.26f;
            const float radiusScale = highlighted ? 0.72f : 0.48f;
            juce::Path glowPath;
            glowPath.addEllipse(knobBounds);
            juce::DropShadow ds(knobGlow.withMultipliedAlpha(0.42f * alpha),
                                juce::roundToInt(18.0f * radiusScale),
                                {});
            ds.drawForPath(g, glowPath);
        }

        juce::ColourGradient body(knobBody.brighter(0.05f), knobBounds.getX(), knobBounds.getY(),
                                  juce::Colour { 0xFF01040A }, knobBounds.getRight(), knobBounds.getBottom(), false);
        body.addColour(0.55, knobBody);
        g.setGradientFill(body);
        g.fillEllipse(knobBounds);

        g.setColour(knobGlow.withAlpha(highlighted ? 0.46f : 0.26f));
        g.drawEllipse(knobBounds.expanded(1.4f).reduced(0.5f), highlighted ? 2.0f : 1.25f);

        g.setColour(knobRim.withAlpha(highlighted ? 0.92f : 0.68f));
        g.drawEllipse(knobBounds.reduced(0.5f), highlighted ? 1.45f : 1.15f);

        const auto angle = rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle) * clampedValue;
        juce::Path indicator;
        indicator.addCentredArc(centre.x, centre.y, radius - 4.0f, radius - 4.0f, 0.0f, rotaryStartAngle, angle, true);
        g.setColour(knobIndicator.withAlpha(highlighted ? 0.96f : 0.82f));
        g.strokePath(indicator, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const auto dotDistance = radius * 0.62f;
        const auto dotRadius = juce::jmax(1.8f, radius * 0.055f);
        const auto dotX = centre.x + dotDistance * std::sin(angle);
        const auto dotY = centre.y - dotDistance * std::cos(angle);
        g.setColour(textPrimary.withAlpha(highlighted ? 0.96f : 0.82f));
        g.fillEllipse(dotX - dotRadius, dotY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
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

        if (themeId == ThemeId::Aurora)
        {
            const auto shadowBase = juce::Colour { Aurora::Colors::BgDeep }.darker(0.85f);
            const auto coolLift = juce::Colour { Aurora::Colors::TraySideGlow };
            const auto glowAlpha = level == ShadowLevel::Ambient ? 0.15f : (level == ShadowLevel::Float ? 0.20f : 0.26f);
            const auto radius = level == ShadowLevel::Ambient ? 14 : (level == ShadowLevel::Float ? 20 : 28);
            const auto offsetY = level == ShadowLevel::Ambient ? 2 : (level == ShadowLevel::Float ? 4 : 7);
            ds.colour = shadowBase.interpolatedWith(coolLift, 0.18f).withAlpha(glowAlpha);
            ds.radius = radius;
            ds.offset = { 0, offsetY };
        }
        else if (themeId == ThemeId::DarkBlueGrey)
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
        else if (themeId == ThemeId::BlueBreeze)
        {
            const auto shadowBase = juce::Colour { BlueBreeze::Colors::ShadowColor };
            if (level == ShadowLevel::Ambient)
            {
                ds.colour = shadowBase.withAlpha(0.10f);
                ds.radius = 18;
                ds.offset = { 0, 4 };
            }
            else if (level == ShadowLevel::Float)
            {
                ds.colour = shadowBase.withAlpha(0.16f);
                ds.radius = 26;
                ds.offset = { 0, 7 };
            }
            else
            {
                ds.colour = shadowBase.withAlpha(0.22f);
                ds.radius = 34;
                ds.offset = { 0, 10 };
            }
        }
        else if (themeId == ThemeId::Overdose)
        {
            const auto shadowBase = juce::Colour { Overdose::Colors::SoftShadow };
            if (level == ShadowLevel::Ambient)
            {
                ds.colour = shadowBase.withAlpha(0.12f);
                ds.radius = 14;
                ds.offset = { 0, 4 };
            }
            else if (level == ShadowLevel::Float)
            {
                ds.colour = shadowBase.withAlpha(0.18f);
                ds.radius = 22;
                ds.offset = { 0, 7 };
            }
            else
            {
                ds.colour = shadowBase.withAlpha(0.24f);
                ds.radius = 30;
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

        if (themeId == ThemeId::BlueBreeze)
        {
            top = UIColors::gradientTop;
            bottom = UIColors::gradientBottom;
        }
        else if (themeId != ThemeId::DarkBlueGrey)
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

        if (themeId == ThemeId::Aurora)
        {
            fillAuroraGlass(g, bounds, radius);
            return;
        }

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

        if (themeId == ThemeId::BlueBreeze)
        {
            fillBlueBreezeSoftPanel(g, bounds, radius);
            return;
        }

        juce::ColourGradient base(UIColors::gradientTop, bounds.getX(), bounds.getY(),
                                  UIColors::gradientBottom, bounds.getRight(), bounds.getBottom(), false);
        base.addColour(0.54, UIColors::backgroundMedium);
        g.setGradientFill(base);
        if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
        else g.fillRect(bounds);

        auto topH = bounds.withTrimmedBottom(bounds.getHeight() * 0.45f);
        juce::ColourGradient softLight(UIColors::bevelLight.withAlpha(0.34f), topH.getX(), topH.getY(),
                                       UIColors::textPrimary.withAlpha(0.0f), topH.getX(), topH.getBottom(), false);
        g.setGradientFill(softLight);
        if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
        else g.fillRect(bounds);

        auto c = bounds.getCentre();
        juce::ColourGradient source(juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(0.18f),
                                    bounds.getX() + bounds.getWidth() * 0.16f,
                                    bounds.getY() + bounds.getHeight() * 0.10f,
                                    juce::Colours::transparentWhite,
                                    bounds.getRight(),
                                    bounds.getBottom(),
                                    true);
        g.setGradientFill(source);
        if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
        else g.fillRect(bounds);

        juce::ColourGradient lowerShade(juce::Colours::transparentBlack, c.x, c.y,
                                        juce::Colour { BlueBreeze::Colors::PanelInset }.withAlpha(0.34f),
                                        bounds.getRight(),
                                        bounds.getBottom(),
                                        true);
        g.setGradientFill(lowerShade);
        if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
        else g.fillRect(bounds);

        if (style.glowAlpha > 0.0f)
        {
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

        if (themeId == ThemeId::Aurora)
        {
            drawAuroraGlassFrame(g, bounds, radius, false);
            return;
        }

        float borderAlpha = 0.66f;
        float innerAlpha = 0.26f;
        if (themeId == ThemeId::DarkBlueGrey)
        {
            borderAlpha = 0.55f;  // 深色底上的边框需要更清晰，但仍保持克制
            innerAlpha = 0.06f;   // 内高光更弱，避免“发灰”
        }
        else if (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose)
        {
            borderAlpha = 0.26f;
            innerAlpha = 0.08f;
        }

        g.setColour(UIColors::panelBorder.withAlpha(borderAlpha));
        if (radius > 0.0f) g.drawRoundedRectangle(bounds.reduced(0.5f), radius, static_cast<float>(style.strokeThin));
        else g.drawRect(bounds.reduced(0.5f), static_cast<float>(style.strokeThin));

        // 内高光线 - 增加立体感
        g.setColour((themeId == ThemeId::BlueBreeze ? juce::Colour { BlueBreeze::Colors::SourceLight }
                    : (themeId == ThemeId::Overdose ? juce::Colour { Overdose::Colors::PanelHighlight }
                    : UIColors::textPrimary)).withAlpha(innerAlpha));
        if (radius > 1.0f) 
            g.drawRoundedRectangle(bounds.reduced(1.5f), radius - 1.0f, static_cast<float>(style.strokeThin));
        else 
            g.drawRect(bounds.reduced(1.5f), static_cast<float>(style.strokeThin));

        if ((themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) && radius > 2.0f)
        {
            juce::Path contour;
            contour.addRoundedRectangle(bounds.reduced(2.2f), radius - 1.7f);
            g.setColour((themeId == ThemeId::Overdose ? juce::Colour { Overdose::Colors::PanelTop }
                                                      : juce::Colour { BlueBreeze::Colors::CanvasTop }).withAlpha(0.12f));
            g.strokePath(contour, juce::PathStrokeType(0.85f));
        }

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

    // ========== Overdose Theme Helpers ==========

    static void fillOverdoseEditorBackground(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        juce::Path shape;
        if (radius > 0.0f)
            shape.addRoundedRectangle(bounds, radius);
        else
            shape.addRectangle(bounds);

        // 主渐变：CanvasTop → CanvasMid → CanvasBottom（粉→淡紫→薰衣草）
        juce::ColourGradient base(
            juce::Colour { Overdose::Colors::CanvasTop },
            bounds.getX(), bounds.getY(),
            juce::Colour { Overdose::Colors::CanvasBottom },
            bounds.getX(), bounds.getBottom(), false);
        base.addColour(0.45, juce::Colour { Overdose::Colors::CanvasMid });
        g.setGradientFill(base);
        g.fillPath(shape);

        // 顶部柔光洗白（模拟参考图顶部高光，加宽加亮）
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(shape);
            auto topBand = bounds.withHeight(bounds.getHeight() * 0.22f);
            juce::ColourGradient topWash(
                juce::Colour { Overdose::Colors::GlassHighlight }.withAlpha(0.16f),
                topBand.getCentreX(), topBand.getY(),
                juce::Colours::transparentWhite,
                topBand.getCentreX(), topBand.getBottom(), false);
            g.setGradientFill(topWash);
            g.fillRect(topBand);
        }

        // 左上对角柔光（模拟参考图的雾化光源，增强）
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(shape);
            auto cornerGlow = juce::Rectangle<float>(bounds.getX(), bounds.getY(),
                                                     bounds.getWidth() * 0.60f, bounds.getHeight() * 0.60f);
            juce::ColourGradient cornerLight(
                juce::Colour { Overdose::Colors::GlassSurface }.withAlpha(0.14f),
                cornerGlow.getTopLeft(),
                juce::Colours::transparentWhite,
                cornerGlow.getBottomRight(), false);
            g.setGradientFill(cornerLight);
            g.fillRect(cornerGlow);
        }

        // 中心径向柔光（模拟参考图的中心雾化，增强）
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(shape);
            auto centre = bounds.getCentre();
            auto maxRadius = juce::jmax(bounds.getWidth(), bounds.getHeight()) * 0.5f;
            juce::ColourGradient centreGlow(
                juce::Colour { Overdose::Colors::GlassHighlight }.withAlpha(0.12f),
                centre.x, centre.y,
                juce::Colours::transparentWhite,
                centre.x + maxRadius, centre.y, true);
            g.setGradientFill(centreGlow);
            g.fillRect(bounds);
        }

        // 右下粉色环境光晕（增强）
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(shape);
            auto bottomRight = juce::Rectangle<float>(bounds.getRight() - bounds.getWidth() * 0.50f,
                                                      bounds.getBottom() - bounds.getHeight() * 0.50f,
                                                      bounds.getWidth() * 0.50f,
                                                      bounds.getHeight() * 0.50f);
            juce::ColourGradient pinkGlow(
                juce::Colours::transparentWhite,
                bottomRight.getTopLeft(),
                juce::Colour { Overdose::Colors::PinkGlowSoft }.withAlpha(0.15f),
                bottomRight.getBottomRight(), false);
            g.setGradientFill(pinkGlow);
            g.fillRect(bottomRight);
        }

        // 底部粉色环境光晕
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(shape);
            auto bottomBand = bounds.withTrimmedTop(bounds.getHeight() * 0.72f);
            juce::ColourGradient bottomGlow(
                juce::Colours::transparentBlack,
                bottomBand.getCentreX(), bottomBand.getY(),
                juce::Colour { Overdose::Colors::PinkGlowSoft }.withAlpha(0.10f),
                bottomBand.getCentreX(), bottomBand.getBottom(), false);
            g.setGradientFill(bottomGlow);
            g.fillRect(bottomBand);
        }

        // 面板边框（精致但可见）
        g.setColour(juce::Colour { Overdose::Colors::PanelBorderSoft }.withAlpha(0.42f));
        if (radius > 0.0f)
            g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
        else
            g.drawRect(bounds.reduced(0.5f), 1.0f);
    }

    static void fillOverdosePanelBackground(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        juce::Path shape;
        shape.addRoundedRectangle(bounds, radius);

        // 阴影（微调外投影，营造悬浮感）
        juce::DropShadow shadow(
            juce::Colour { Overdose::Colors::SoftShadow }.withAlpha(0.44f),
            34, { 0, 13 });
        shadow.drawForPath(g, shape);

        // 主体渐变：PanelOpaqueTop → PanelOpaqueMid → PanelOpaqueBottom（增强层次）
        juce::ColourGradient body(
            juce::Colour { Overdose::Colors::PanelOpaqueTop },
            bounds.getX() + bounds.getWidth() * 0.10f, bounds.getY(),
            juce::Colour { Overdose::Colors::PanelOpaqueBottom },
            bounds.getRight(), bounds.getBottom(), false);
        body.addColour(0.30, juce::Colour { Overdose::Colors::PanelOpaqueMid });
        body.addColour(0.70, juce::Colour { Overdose::Colors::PanelOpaqueMid }.darker(0.05f));
        g.setGradientFill(body);
        g.fillPath(shape);

        // 顶部高光条（玻璃拟态特征，增强）
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(shape);
            g.setColour(juce::Colour { Overdose::Colors::GlassHighlight }.withAlpha(0.80f));
            g.drawLine(
                bounds.getX() + radius, bounds.getY() + 1.0f,
                bounds.getRight() - radius, bounds.getY() + 1.0f, 1.8f);

            // 顶部向下渐隐（增强）
            auto fadeBand = bounds.withHeight(bounds.getHeight() * 0.12f);
            juce::ColourGradient fade(
                juce::Colour { Overdose::Colors::GlassSurface }.withAlpha(0.30f),
                fadeBand.getCentreX(), fadeBand.getY(),
                juce::Colours::transparentWhite,
                fadeBand.getCentreX(), fadeBand.getBottom(), false);
            g.setGradientFill(fade);
            g.fillRect(fadeBand);
        }

        // 内阴影（底部边缘暗化，微调凹陷感）
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(shape);
            auto innerBottom = bounds.withTrimmedTop(bounds.getHeight() * 0.80f);
            juce::ColourGradient innerShade(
                juce::Colours::transparentBlack,
                innerBottom.getCentreX(), innerBottom.getY(),
                juce::Colour { Overdose::Colors::PanelInsetShadow }.withAlpha(0.28f),
                innerBottom.getCentreX(), innerBottom.getBottom(), false);
            g.setGradientFill(innerShade);
            g.fillRect(innerBottom);
        }

        // 外描边：粉色半透明（精致细线）
        g.setColour(juce::Colour { Overdose::Colors::PanelBorder }.withAlpha(0.65f));
        g.strokePath(shape, juce::PathStrokeType(1.2f));

        // 内描边：更浅的粉色
        if (radius > 1.5f)
        {
            g.setColour(juce::Colour { Overdose::Colors::PanelBorderSoft }.withAlpha(0.38f));
            g.drawRoundedRectangle(bounds.reduced(1.6f), radius - 1.2f, 0.85f);
            
            // 第三层描边（最内层，极淡）
            g.setColour(juce::Colour { Overdose::Colors::GlassHighlight }.withAlpha(0.16f));
            g.drawRoundedRectangle(bounds.reduced(2.4f), radius - 2.0f, 0.6f);
        }
    }

    static void drawOverdoseDisplayWell(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        juce::Path shape;
        shape.addRoundedRectangle(bounds, radius);

        // 外部粉色辉光（霓虹效果，更强）
        juce::DropShadow outerGlow(
            juce::Colour { Overdose::Colors::PinkGlow }.withAlpha(0.55f),
            32, {});
        outerGlow.drawForPath(g, shape);

        // 深色背景：极深黑紫色（参考图：接近纯黑带微紫，更深）
        juce::ColourGradient bg(
            juce::Colour(0xFF0A0415),  // 更深黑紫
            bounds.getX(), bounds.getY(),
            juce::Colour(0xFF04020D),  // 接近纯黑
            bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill(bg);
        g.fillPath(shape);

        // 内部顶部白色高光带（霓虹内辉光）
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(shape);
            auto topBand = bounds.withHeight(bounds.getHeight() * 0.25f);
            juce::ColourGradient topSheen(
                juce::Colour { Overdose::Colors::GlassHighlight }.withAlpha(0.25f),
                topBand.getCentreX(), topBand.getY(),
                juce::Colours::transparentBlack,
                topBand.getCentreX(), topBand.getBottom(), false);
            g.setGradientFill(topSheen);
            g.fillRect(topBand);
        }

        // 内部底部内阴影
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(shape);
            auto bottomBand = bounds.withTrimmedTop(bounds.getHeight() * 0.75f);
            juce::ColourGradient bottomShade(
                juce::Colours::transparentBlack,
                bottomBand.getCentreX(), bottomBand.getY(),
                juce::Colour { Overdose::Colors::DarkControlEdge }.withAlpha(0.32f),
                bottomBand.getCentreX(), bottomBand.getBottom(), false);
            g.setGradientFill(bottomShade);
            g.fillRect(bottomBand);
        }

        // 外描边：深紫色（精致细线）
        g.setColour(juce::Colour { Overdose::Colors::DarkControlEdge }.withAlpha(0.85f));
        g.strokePath(shape, juce::PathStrokeType(1.4f));

        // 粉色外轮廓光（霓虹效果，三层增强）
        g.setColour(juce::Colour { Overdose::Colors::PinkGlow }.withAlpha(0.60f));
        g.strokePath(shape, juce::PathStrokeType(3.5f));
        
        g.setColour(juce::Colour { Overdose::Colors::PrimaryPink }.withAlpha(0.30f));
        g.strokePath(shape, juce::PathStrokeType(5.5f));

        // 内描边：粉色微光
        if (radius > 1.5f)
        {
            g.setColour(juce::Colour { Overdose::Colors::PinkGlowSoft }.withAlpha(0.35f));
            g.drawRoundedRectangle(bounds.reduced(1.4f), radius - 1.0f, 1.0f);
        }
    }

    // 粉色玻璃拟态按钮壳：白→淡紫渐变 + 顶部白高光 + 底部淡紫内阴影 + 粉边
    // 供 M/S 按钮、侧栏工具按钮、工具栏按钮等小型控件复用（不绘制外投影，避免被控件边界裁切）
    static void fillOverdoseButtonShell(juce::Graphics& g,
                                        const juce::Rectangle<float>& bounds,
                                        float radius,
                                        const juce::Path* shapeOverride = nullptr)
    {
        juce::Path shape;
        if (shapeOverride != nullptr)
            shape = *shapeOverride;
        else if (radius > 0.0f)
            shape.addRoundedRectangle(bounds, radius);
        else
            shape.addRectangle(bounds);

        // 柔和阴影（增强立体感）
        juce::DropShadow shellShadow(
            juce::Colour { Overdose::Colors::SoftShadow }.withAlpha(0.18f),
            7, { 0, 2 });
        shellShadow.drawForPath(g, shape);

        // 主体渐变：更通透的白→淡紫（参考图：半透明玻璃质感，更通透）
        juce::ColourGradient body(
            juce::Colour(0xFFFFFCFF).withAlpha(0.82f),  // 更通透的顶部
            bounds.getX() + bounds.getWidth() * 0.10f, bounds.getY(),
            juce::Colour(0xFFF0E8F8).withAlpha(0.78f),  // 更淡的底部
            bounds.getRight(), bounds.getBottom(), false);
        body.addColour(0.40, juce::Colour(0xFFF8F2FC).withAlpha(0.80f));
        g.setGradientFill(body);
        g.fillPath(shape);

        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(shape);

            // 顶部白高光（更强，玻璃质感）
            const auto topInset = juce::jmin(radius, bounds.getWidth() * 0.18f);
            g.setColour(juce::Colour(0xFFFFFFFF).withAlpha(0.75f));
            g.drawLine(bounds.getX() + topInset, bounds.getY() + 1.0f,
                       bounds.getRight() - topInset, bounds.getY() + 1.0f, 1.2f);

            // 顶部向下渐隐（覆盖更大区域）
            auto fadeBand = bounds.withHeight(bounds.getHeight() * 0.30f);
            juce::ColourGradient fade(
                juce::Colour(0xFFFFFFFF).withAlpha(0.40f),
                fadeBand.getCentreX(), fadeBand.getY(),
                juce::Colours::transparentWhite,
                fadeBand.getCentreX(), fadeBand.getBottom(), false);
            g.setGradientFill(fade);
            g.fillRect(fadeBand);

            // 底部淡紫内阴影（更柔和）
            auto innerBottom = bounds.withTrimmedTop(bounds.getHeight() * 0.60f);
            juce::ColourGradient innerShade(
                juce::Colours::transparentBlack,
                innerBottom.getCentreX(), innerBottom.getY(),
                juce::Colour(0xFF8070B0).withAlpha(0.15f),
                innerBottom.getCentreX(), innerBottom.getBottom(), false);
            g.setGradientFill(innerShade);
            g.fillRect(innerBottom);
        }

        // 粉边（更精致更淡）
        g.setColour(juce::Colour { Overdose::Colors::PanelBorder }.withAlpha(0.32f));
        g.strokePath(shape, juce::PathStrokeType(0.7f));
        if (shapeOverride == nullptr && radius > 1.5f)
        {
            g.setColour(juce::Colour { Overdose::Colors::PanelBorderSoft }.withAlpha(0.18f));
            g.drawRoundedRectangle(bounds.reduced(1.4f), juce::jmax(0.0f, radius - 1.0f), 0.6f);
        }
    }

    // 金属银色旋钮：银色主体 + 粉色指示环 + 心形中心
    static void drawOverdoseKnob(juce::Graphics& g,
                                 juce::Rectangle<float> bounds,
                                 float normalisedValue,
                                 bool highlighted,
                                 float rotaryStartAngle = juce::MathConstants<float>::pi * 1.25f,
                                 float rotaryEndAngle = juce::MathConstants<float>::pi * 2.75f)
    {
        const auto clampedValue = juce::jlimit(0.0f, 1.0f, normalisedValue);
        const auto insetBounds = bounds.reduced(2.0f);
        const auto side = juce::jmin(insetBounds.getWidth(), insetBounds.getHeight());
        const auto knobBounds = insetBounds.withSizeKeepingCentre(side * 0.72f, side * 0.72f);
        const auto centre = knobBounds.getCentre();
        const auto radius = knobBounds.getWidth() * 0.5f;
        const auto ringRadius = radius + juce::jmax(5.0f, side * 0.085f);
        const auto angle = rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle) * clampedValue;

        // 粉色轨道环（参考图：极细完整圆形粉色环，几乎不可见）
        juce::Path ringPath;
        ringPath.addCentredArc(centre.x, centre.y, ringRadius, ringRadius, 0.0f, 0.0f, juce::MathConstants<float>::twoPi, true);
        g.setColour(juce::Colour { Overdose::Colors::PrimaryPink }.withAlpha(0.25f));
        g.strokePath(ringPath, juce::PathStrokeType(0.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // 粉刻度（参考图：极细极淡的刻度线）
        static constexpr int tickCount = 34;
        for (int i = 0; i < tickCount; ++i)
        {
            const auto t = static_cast<float>(i) / static_cast<float>(tickCount - 1);
            const auto tickAngle = rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle) * t;
            const auto isMajor = (i % 4) == 0;
            const auto inner = ringRadius + (isMajor ? 1.5f : 3.0f);
            const auto outer = ringRadius + (isMajor ? 6.0f : 4.5f);
            juce::Line<float> tick {
                centre.x + inner * std::sin(tickAngle),
                centre.y - inner * std::cos(tickAngle),
                centre.x + outer * std::sin(tickAngle),
                centre.y - outer * std::cos(tickAngle)
            };
            g.setColour((isMajor ? juce::Colour { Overdose::Colors::PrimaryPink }
                                 : juce::Colour { Overdose::Colors::KnobRim })
                            .withAlpha(isMajor ? 0.35f : 0.18f));
            g.drawLine(tick, isMajor ? 0.8f : 0.5f);
        }

        // 粉色指示弧（当前值到起点的弧，极细精致）
        juce::Path valueArc;
        valueArc.addCentredArc(centre.x, centre.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, angle, true);
        g.setColour(juce::Colour { Overdose::Colors::PrimaryPink }.withAlpha(highlighted ? 0.50f : 0.38f));
        g.strokePath(valueArc, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // 旋钮主体：金属银色渐变（参考图：银色金属质感）
        juce::Path knobPath;
        knobPath.addEllipse(knobBounds);
        if (highlighted)
        {
            juce::DropShadow responseGlow(juce::Colour { Overdose::Colors::PinkGlow }.withAlpha(0.20f),
                                           18, {});
            responseGlow.drawForPath(g, knobPath);
        }

        juce::DropShadow shadow(juce::Colour { Overdose::Colors::SoftShadow }.withAlpha(highlighted ? 0.55f : 0.45f),
                                highlighted ? 22 : 18,
                                { 0, highlighted ? 7 : 5 });
        shadow.drawForPath(g, knobPath);

        // 金属银色主体渐变（中性银色，减少紫色偏色）
        juce::ColourGradient body(juce::Colour(0xFFF4F2F7),  // 顶部更亮银
                                  knobBounds.getX() + knobBounds.getWidth() * 0.15f,
                                  knobBounds.getY() + knobBounds.getHeight() * 0.02f,
                                  juce::Colour(0xFFB8B4C0),  // 底部暗银
                                  knobBounds.getRight(), knobBounds.getBottom(), false);
        body.addColour(0.25, juce::Colour(0xFFE8E5EC));
        body.addColour(0.50, juce::Colour(0xFFD8D4DE));
        body.addColour(0.75, juce::Colour(0xFFC8C4D0));
        g.setGradientFill(body);
        g.fillPath(knobPath);

        // 顶部高光（金属反光）
        juce::ColourGradient gloss(juce::Colour { Overdose::Colors::GlassHighlight }.withAlpha(highlighted ? 0.40f : 0.30f),
                                   knobBounds.getX() + knobBounds.getWidth() * 0.25f,
                                   knobBounds.getY() + knobBounds.getHeight() * 0.08f,
                                   juce::Colours::transparentWhite,
                                   knobBounds.getRight(), knobBounds.getCentreY(), true);
        g.setGradientFill(gloss);
        g.fillPath(knobPath);

        // 顶部镜面高光点
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(knobPath);
            auto specular = juce::Rectangle<float>(knobBounds.getX() + knobBounds.getWidth() * 0.20f,
                                                   knobBounds.getY() + knobBounds.getHeight() * 0.10f,
                                                   knobBounds.getWidth() * 0.35f,
                                                   knobBounds.getHeight() * 0.18f);
            juce::ColourGradient spec(
                juce::Colour(0xFFFFFFFF).withAlpha(highlighted ? 0.75f : 0.60f),
                specular.getTopLeft(),
                juce::Colours::transparentWhite,
                specular.getBottomRight(), false);
            g.setGradientFill(spec);
            g.fillEllipse(specular);
        }

        // 银色描边（更细）
        g.setColour(juce::Colour(0xFFA8A0B8).withAlpha(highlighted ? 0.65f : 0.50f));
        g.strokePath(knobPath, juce::PathStrokeType(highlighted ? 1.2f : 0.9f));

        // 线状指针（粉色极细线，从中心向角度方向）
        {
            const auto pointerLen = radius * 0.58f;
            const auto pointerW = juce::jmax(0.6f, radius * 0.030f);  // 极细
            const auto px = centre.x + std::cos(angle) * pointerLen;
            const auto py = centre.y + std::sin(angle) * pointerLen;
            // 指针阴影
            g.setColour(juce::Colour { Overdose::Colors::SoftShadow }.withAlpha(0.20f));
            g.drawLine(centre.x + 0.3f, centre.y + 0.5f, px + 0.3f, py + 0.5f, pointerW);
            // 指针主体（粉色）
            g.setColour(juce::Colour { Overdose::Colors::PrimaryPink }.withAlpha(0.75f));
            g.drawLine(centre.x, centre.y, px, py, pointerW);
        }

        // 中心心形指示（参考图：极小的粉色心形，几乎不可见）
        {
            const auto heartSize = juce::jmax(1.2f, radius * 0.020f);  // 极小
            const auto hx = centre.x;
            const auto hy = centre.y - heartSize * 0.05f;
            
            // 绘制心形路径
            juce::Path heart;
            heart.startNewSubPath(hx, hy + heartSize * 0.35f);
            heart.cubicTo(hx - heartSize * 0.55f, hy - heartSize * 0.05f,
                         hx - heartSize * 0.55f, hy - heartSize * 0.50f,
                         hx, hy - heartSize * 0.10f);
            heart.cubicTo(hx + heartSize * 0.55f, hy - heartSize * 0.50f,
                         hx + heartSize * 0.55f, hy - heartSize * 0.05f,
                         hx, hy + heartSize * 0.35f);
            heart.closeSubPath();
            
            // 心形填充（亮粉渐变）
            juce::ColourGradient heartGrad(juce::Colour(0xFFFF90D0),
                                           hx - heartSize * 0.5f, hy - heartSize * 0.5f,
                                           juce::Colour(0xFFFF30A0),
                                           hx + heartSize * 0.5f, hy + heartSize * 0.4f, false);
            g.setGradientFill(heartGrad);
            g.fillPath(heart);
            
            // 心形高光（左上小亮点）
            g.setColour(juce::Colour(0xFFFFFFFF).withAlpha(0.65f));
            g.fillEllipse(hx - heartSize * 0.30f, hy - heartSize * 0.40f, heartSize * 0.35f, heartSize * 0.25f);
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
