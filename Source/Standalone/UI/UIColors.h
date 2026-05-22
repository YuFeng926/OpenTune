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
    static inline juce::Colour originalF0 { 0xFFFF6666 };   // 柔和红色
    static inline juce::Colour correctedF0 { 0xFF2DFFA0 };  // 高饱和青绿色
    static inline juce::Colour shadowTrack { 0x402DFFA0 };

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
    static inline juce::Colour glassSurface { 0xD40E2237 };
    static inline juce::Colour glassHighlight { 0x229EDFFF };
    static inline juce::Colour glassEdge { 0x805CC8FF };
    static inline juce::Colour panelGlow { 0x521A78D0 };
    static inline juce::Colour auroraButtonNormal { 0xC00B1728 };
    static inline juce::Colour auroraButtonHover { 0xD1112A44 };
    static inline juce::Colour auroraButtonActive { 0xE51B5F9E };
    static inline juce::Colour pianoRollBackground { 0xFF0C1D2F };
    static inline juce::Colour pianoRollLane { 0xFF87B6D4 };
    static inline juce::Colour pianoRollGrid { 0xFF9BD5FF };
    static inline juce::Colour pianoRollWaveform { 0xFF8EB8CE };
    static inline juce::Colour sidebarTrackFade { 0x7A1F7BFF };
    static inline juce::Colour knobRim { 0x9A4FC3FF };
    static inline juce::Colour knobGlow { 0x821688FF };

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
        sidebarTrackFade = tokens.sidebarTrackFade;
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

    static void fillAuroraGlass(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        juce::ColourGradient body(glassHighlight.withMultipliedAlpha(0.50f), bounds.getX(), bounds.getY(),
                                  backgroundDark.brighter(0.03f), bounds.getX(), bounds.getBottom(), false);
        body.addColour(0.35, glassSurface);
        body.addColour(0.82, juce::Colour { 0xFF0A1B2C });
        body.addColour(1.0, juce::Colour { 0xFF081827 });
        g.setGradientFill(body);
        if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
        else g.fillRect(bounds);

        juce::ColourGradient side(panelGlow.withAlpha(0.20f), bounds.getX(), bounds.getCentreY(),
                                  juce::Colours::transparentBlack, bounds.getRight(), bounds.getBottom(), true);
        g.setGradientFill(side);
        if (radius > 0.0f) g.fillRoundedRectangle(bounds, radius);
        else g.fillRect(bounds);
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
                                     bool strong = false)
    {
        const auto stroke = strong ? 1.45f : 0.9f;
        g.setColour(glassEdge.withMultipliedAlpha(strong ? 1.0f : 0.58f));
        if (radius > 0.0f) g.drawRoundedRectangle(bounds.reduced(0.5f), radius, stroke);
        else g.drawRect(bounds.reduced(0.5f), stroke);

        g.setColour(glassHighlight.withMultipliedAlpha(strong ? 0.78f : 0.42f));
        const auto topY = bounds.getY() + 1.0f;
        g.drawLine(bounds.getX() + juce::jmin(radius, bounds.getWidth() * 0.25f), topY,
                   bounds.getRight() - juce::jmin(radius, bounds.getWidth() * 0.25f), topY,
                   1.0f);
    }

    static void drawAuroraButtonChrome(juce::Graphics& g,
                                       const juce::Rectangle<float>& bounds,
                                       float radius,
                                       bool highlighted,
                                       bool down,
                                       bool active = false,
                                       juce::Colour glowColour = {},
                                       juce::Colour edgeColour = {},
                                       const juce::Path* shapeOverride = nullptr)
    {
        const auto isPressed = down;
        const auto isActive = active || isPressed;
        const auto isHovered = highlighted && !isPressed;
        const auto fill = isActive ? auroraButtonActive : (isHovered ? auroraButtonHover : auroraButtonNormal);
        const auto glow = glowColour.getAlpha() > 0 ? glowColour : (isActive ? knobGlow : panelGlow);
        const auto edge = edgeColour.getAlpha() > 0 ? edgeColour : (isActive ? knobGlow : glassEdge);

        juce::Path shape;
        if (shapeOverride != nullptr)
            shape = *shapeOverride;
        else if (radius > 0.0f)
            shape.addRoundedRectangle(bounds, radius);
        else
            shape.addRectangle(bounds);

        juce::DropShadow outerGlow(glow.withMultipliedAlpha(isActive ? 0.20f : (isHovered ? 0.12f : 0.055f)),
                                   isActive ? 13 : 9,
                                   {});
        outerGlow.drawForPath(g, shape);

        const auto topLight = fill.brighter(isPressed ? 0.08f : 0.16f).interpolatedWith(glassHighlight, isActive ? 0.18f : 0.14f);
        const auto midTone = fill.brighter(isHovered ? 0.07f : 0.02f);
        const auto lowerTone = fill.darker(isPressed ? 0.22f : 0.10f);
        juce::ColourGradient chrome(topLight, bounds.getX(), bounds.getY(),
                                    lowerTone, bounds.getRight(), bounds.getBottom(), false);
        chrome.addColour(0.34, midTone);
        chrome.addColour(0.70, fill);
        g.setGradientFill(chrome);
        g.fillPath(shape);

        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(shape);

            juce::ColourGradient sourceLight(textPrimary.withAlpha(isActive ? 0.15f : 0.105f),
                                             bounds.getX() + bounds.getWidth() * 0.12f,
                                             bounds.getY() + bounds.getHeight() * 0.08f,
                                             juce::Colours::white.withAlpha(0.0f),
                                             bounds.getRight(),
                                             bounds.getBottom(),
                                             true);
            g.setGradientFill(sourceLight);
            g.fillRect(bounds);

            auto topBand = bounds.withHeight(bounds.getHeight() * 0.44f);
            juce::ColourGradient topSheen(glassHighlight.withMultipliedAlpha(isActive ? 0.44f : 0.36f),
                                          topBand.getX(),
                                          topBand.getY(),
                                          juce::Colours::white.withAlpha(0.0f),
                                          topBand.getX(),
                                          topBand.getBottom(),
                                          false);
            g.setGradientFill(topSheen);
            g.fillRect(topBand);

            auto bottomBand = bounds.withTop(bounds.getY() + bounds.getHeight() * 0.56f);
            juce::ColourGradient bottomShade(juce::Colours::transparentBlack,
                                             bottomBand.getX(),
                                             bottomBand.getY(),
                                             juce::Colours::black.withAlpha(isPressed ? 0.34f : 0.24f),
                                             bottomBand.getX(),
                                             bottomBand.getBottom(),
                                             false);
            g.setGradientFill(bottomShade);
            g.fillRect(bottomBand);

            g.setColour(edge.withMultipliedAlpha(isActive ? 0.42f : (isHovered ? 0.30f : 0.20f)));
            g.strokePath(shape, juce::PathStrokeType(1.0f));
        }

        const auto stroke = isActive ? 1.45f : (isHovered ? 1.15f : 1.0f);
        g.setColour(edge.withMultipliedAlpha(isActive ? 0.94f : (isHovered ? 0.70f : 0.52f)));
        g.strokePath(shape, juce::PathStrokeType(stroke));

        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(shape);
            g.setColour(glassHighlight.withMultipliedAlpha(isActive ? 0.78f : 0.48f));
            const auto topInset = juce::jmin(radius, bounds.getWidth() * 0.25f);
            g.drawLine(bounds.getX() + topInset, bounds.getY() + 1.0f,
                       bounds.getRight() - topInset, bounds.getY() + 1.0f,
                       1.0f);
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
            const auto glowAlpha = level == ShadowLevel::Ambient ? 0.22f : (level == ShadowLevel::Float ? 0.30f : 0.38f);
            const auto radius = level == ShadowLevel::Ambient ? 20 : (level == ShadowLevel::Float ? 28 : 36);
            const auto offsetY = level == ShadowLevel::Ambient ? 3 : (level == ShadowLevel::Float ? 5 : 8);
            ds.colour = panelGlow.withMultipliedAlpha(glowAlpha);
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
