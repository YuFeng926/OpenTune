#pragma once

#include <juce_graphics/juce_graphics.h>
#include "BlueBreezeTheme.h"
#include "DarkBlueGreyTheme.h"
#include "AuroraTheme.h"
#include "OverdoseTheme.h"

namespace OpenTune {

enum class ThemeId : int
{
    BlueBreeze = 0,
    DarkBlueGrey = 1,
    Aurora = 2,
    Overdose = 3
};

struct ThemeTokens
{
    juce::Colour primaryPurple;
    juce::Colour accent;
    juce::Colour lightPurple;
    juce::Colour darkPurple;

    juce::Colour backgroundDark;
    juce::Colour backgroundMedium;
    juce::Colour backgroundLight;

    juce::Colour gradientTop;
    juce::Colour gradientBottom;

    juce::Colour panelBorder;
    juce::Colour buttonNormal;
    juce::Colour buttonHover;
    juce::Colour buttonPressed;

    juce::Colour bevelLight;
    juce::Colour bevelDark;
    juce::Colour glowColor;

    juce::Colour textPrimary;
    juce::Colour textSecondary;
    juce::Colour textDisabled;
    juce::Colour textHighlight;

    juce::Colour rollBackground;
    juce::Colour laneC;
    juce::Colour laneOther;
    juce::Colour gridLine;

    juce::Colour originalF0;
    juce::Colour correctedF0;
    juce::Colour shadowTrack;

    juce::Colour noteBlock;
    juce::Colour noteBlockBorder;
    juce::Colour noteBlockSelected;
    juce::Colour noteBlockHover;

    juce::Colour playhead;
    juce::Colour timelineMarker;
    juce::Colour beatMarker;

    juce::Colour toolActive;
    juce::Colour toolInactive;
    juce::Colour buttonInactive;

    juce::Colour statusProcessing;
    juce::Colour statusReady;
    juce::Colour statusError;

    juce::Colour waveformFill;
    juce::Colour waveformOutline;

    juce::Colour scaleHighlight;

    juce::Colour knobBody;
    juce::Colour knobIndicator;
    juce::Colour displayWellTop;
    juce::Colour displayWellBottom;
    juce::Colour displayWellEdge;
    juce::Colour displayText;
    juce::Colour displayTextDim;
    juce::Colour darkControlFace;
    juce::Colour darkControlEdge;
    juce::Colour keyBedWhite;
    juce::Colour keyBedBlack;
    juce::Colour keyBedDivider;

    juce::Colour glassSurface;
    juce::Colour glassHighlight;
    juce::Colour glassEdge;
    juce::Colour panelGlow;
    juce::Colour auroraButtonNormal;
    juce::Colour auroraButtonHover;
    juce::Colour auroraButtonActive;
    juce::Colour pianoRollBackground;
    juce::Colour pianoRollLane;
    juce::Colour pianoRollGrid;
    juce::Colour pianoRollWaveform;
    juce::Colour sidebarTrackFade;
    juce::Colour auroraSidebarShellTop;
    juce::Colour auroraSidebarShellMid;
    juce::Colour auroraSidebarShellBottom;
    juce::Colour auroraSidebarTopLip;
    juce::Colour auroraSidebarOuterRim;
    juce::Colour auroraSidebarInnerRim;
    juce::Colour auroraSidebarEdgeAura;
    juce::Colour auroraSidebarCornerBloom;
    juce::Colour knobRim;
    juce::Colour knobGlow;

    float cornerRadius = 8.0f;
};

struct ThemeStyle
{
    float panelRadius = 16.0f;   // Unified with BlueBreeze
    float controlRadius = 10.0f; // Unified with BlueBreeze
    float fieldRadius = 10.0f;   // Unified with BlueBreeze
    float knobRadius = 999.0f;

    float strokeThin = 1.0f;
    float strokeThick = 2.0f;
    float focusRingThickness = 2.0f;

    float shadowAlpha = 0.25f;   // Unified with BlueBreeze
    int shadowRadius = 10;       // Unified with BlueBreeze
    juce::Point<int> shadowOffset { 0, 4 }; // Unified with BlueBreeze

    float glowAlpha = 0.0f;
    float glowRadius = 0.0f;

    float bevelWidth = 2.0f;
    float bevelIntensity = 0.3f;

    float animationDurationMs = 150.0f;
    float hoverGlowIntensity = 0.8f;

    juce::Colour vuLow;
    juce::Colour vuMid;
    juce::Colour vuHigh;
    juce::Colour vuClip;

    juce::Colour timeActive;
    juce::Colour timeInactive;

    bool timeSegmentStyle = true;
};

class Theme
{
public:
    static const ThemeTokens& getTokens(ThemeId themeId)
    {
        switch (themeId)
        {
            case ThemeId::BlueBreeze:
                return blueBreezeTokens();
            case ThemeId::DarkBlueGrey:
                return darkBlueGreyTokens();
            case ThemeId::Aurora:
                return auroraTokens();
            case ThemeId::Overdose:
                return overdoseTokens();
            default:
                return blueBreezeTokens();
        }
    }

    static const ThemeStyle& getStyle(ThemeId themeId)
    {
        switch (themeId)
        {
            case ThemeId::BlueBreeze:
                return blueBreezeStyle();
            case ThemeId::DarkBlueGrey:
                return darkBlueGreyStyle();
            case ThemeId::Aurora:
                return auroraStyle();
            case ThemeId::Overdose:
                return overdoseStyle();
            default:
                return blueBreezeStyle();
        }
    }

private:
    static const ThemeStyle& blueBreezeStyle()
    {
        static const ThemeStyle style {
            BlueBreeze::Style::PanelRadius,
            BlueBreeze::Style::ControlRadius,
            BlueBreeze::Style::ControlRadius,
            BlueBreeze::Style::KnobRadius,

            BlueBreeze::Style::StrokeThin,
            BlueBreeze::Style::StrokeThick,
            2.0f,

            0.16f,
            18,
            { 0, 5 },

            0.0f,
            0.0f,

            0.0f,
            0.0f,

            150.0f,
            BlueBreeze::Style::HoverGlowAmount,

            juce::Colour { BlueBreeze::Colors::AccentBlue },
            juce::Colour { BlueBreeze::Colors::NodeYellow },
            juce::Colour { BlueBreeze::Colors::NodeRed },
            juce::Colour { 0xFFFF4A4A },

            juce::Colour { BlueBreeze::Colors::AccentBlue },
            juce::Colour { BlueBreeze::Colors::TextDim }.withAlpha(0.2f),

            true
        };

        return style;
    }

    static const ThemeStyle& darkBlueGreyStyle()
    {
        static const ThemeStyle style {
            DarkBlueGrey::Style::PanelRadius,
            DarkBlueGrey::Style::ControlRadius,
            DarkBlueGrey::Style::FieldRadius,
            DarkBlueGrey::Style::KnobRadius,

            DarkBlueGrey::Style::StrokeThin,
            DarkBlueGrey::Style::StrokeThick,
            DarkBlueGrey::Style::FocusRingThickness,

            DarkBlueGrey::Style::ShadowAlpha,
            DarkBlueGrey::Style::ShadowRadius,
            { DarkBlueGrey::Style::ShadowOffsetX, DarkBlueGrey::Style::ShadowOffsetY },

            DarkBlueGrey::Style::GlowAlpha,
            DarkBlueGrey::Style::GlowRadius,

            DarkBlueGrey::Style::BevelWidth,
            DarkBlueGrey::Style::BevelIntensity,

            DarkBlueGrey::Style::AnimationDurationMs,
            DarkBlueGrey::Style::HoverGlowIntensity,

            juce::Colour { DarkBlueGrey::Colors::VULow },
            juce::Colour { DarkBlueGrey::Colors::VUMid },
            juce::Colour { DarkBlueGrey::Colors::VUHigh },
            juce::Colour { DarkBlueGrey::Colors::VUClip },

            juce::Colour { DarkBlueGrey::Colors::TimeActive },
            juce::Colour { DarkBlueGrey::Colors::TimeInactive },

            false
        };

        return style;
    }

    static const ThemeTokens& blueBreezeTokens()
    {
        static const ThemeTokens tokens {
            juce::Colour { BlueBreeze::Colors::AccentBlue },
            juce::Colour { BlueBreeze::Colors::AccentBlue },
            juce::Colour { BlueBreeze::Colors::AccentBlueSoft },
            juce::Colour { BlueBreeze::Colors::GraphBgDeep },

            juce::Colour { BlueBreeze::Colors::GraphBgDeep },
            juce::Colour { BlueBreeze::Colors::FieldFogMid },
            juce::Colour { BlueBreeze::Colors::FieldFogTop },

            juce::Colour { BlueBreeze::Colors::CanvasTop },
            juce::Colour { BlueBreeze::Colors::CanvasBottom },

            juce::Colour { BlueBreeze::Colors::PanelBorder },
            juce::Colour { BlueBreeze::Colors::ControlBottom },
            juce::Colour { BlueBreeze::Colors::ControlHover },
            juce::Colour { BlueBreeze::Colors::ControlPressed },

            juce::Colour { BlueBreeze::Colors::SourceLight },
            juce::Colour { BlueBreeze::Colors::PanelInset },
            juce::Colour { BlueBreeze::Colors::AccentBlue },

            juce::Colour { BlueBreeze::Colors::TextDark },
            juce::Colour { BlueBreeze::Colors::TextDim },
            juce::Colour { 0xFFA1AFBA },
            juce::Colour { BlueBreeze::Colors::AccentBlue },

            juce::Colour { BlueBreeze::Colors::FieldFogMid },
            juce::Colour { BlueBreeze::Colors::LaneSoft }.withAlpha(0.16f),
            juce::Colour { BlueBreeze::Colors::GraphBgDeep }.withAlpha(0.14f),
            juce::Colour { BlueBreeze::Colors::GridSoft }.withAlpha(0.18f),

            juce::Colour { 0xFFD24A3A }, // originalF0
            juce::Colour { 0xFF196FC4 }, // correctedF0
            juce::Colour { 0x30196FC4 }, // shadowTrack

            juce::Colour { 0xFF235AA8 },
            juce::Colour { 0xFF3A69A2 },
            juce::Colour { 0xFF2F6FC4 },
            juce::Colour { 0xFF2A63B8 },

            juce::Colour { BlueBreeze::Colors::SourceLight },
            juce::Colour { BlueBreeze::Colors::AccentBlue },
            juce::Colour { BlueBreeze::Colors::GridSoft }.withAlpha(0.24f),

            juce::Colour { BlueBreeze::Colors::ActiveWhite },
            juce::Colour { BlueBreeze::Colors::TextDim },
            juce::Colour { 0x18000000 },

            juce::Colour { 0xFFF39C12 },
            juce::Colour { 0xFF2ECC71 },
            juce::Colour { BlueBreeze::Colors::NodeRed },

            juce::Colour { 0x600C3C4A },
            juce::Colour { 0xFF0C3C4A },

            juce::Colour { 0x305AA8E6 },

            juce::Colour { BlueBreeze::Colors::KnobBody },
            juce::Colour { BlueBreeze::Colors::KnobIndicator },
            juce::Colour { BlueBreeze::Colors::DisplayTop },
            juce::Colour { BlueBreeze::Colors::DisplayBottom },
            juce::Colour { BlueBreeze::Colors::DisplayEdge },
            juce::Colour { BlueBreeze::Colors::DisplayText },
            juce::Colour { BlueBreeze::Colors::DisplayTextDim },
            juce::Colour { BlueBreeze::Colors::DarkFaceBottom },
            juce::Colour { BlueBreeze::Colors::DarkFaceEdge },
            juce::Colour { BlueBreeze::Colors::KeyBedTop },
            juce::Colour { BlueBreeze::Colors::KeyBlackTop },
            juce::Colour { BlueBreeze::Colors::KeyBedDivider },

            juce::Colour { BlueBreeze::Colors::ControlBottom }.withAlpha(0.58f),
            juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(0.56f),
            juce::Colour { BlueBreeze::Colors::PanelBorder },
            juce::Colour { BlueBreeze::Colors::AccentGlow },
            juce::Colour { BlueBreeze::Colors::ControlBottom },
            juce::Colour { BlueBreeze::Colors::ControlHover },
            juce::Colour { BlueBreeze::Colors::ControlPressed },
            juce::Colour { BlueBreeze::Colors::FieldFogBottom },
            juce::Colour { BlueBreeze::Colors::LaneSoft }.withAlpha(0.14f),
            juce::Colour { BlueBreeze::Colors::GridSoft }.withAlpha(0.14f),
            juce::Colour { 0xFF0C3C4A },
            juce::Colour { BlueBreeze::Colors::FieldFogBottom },
            juce::Colour { BlueBreeze::Colors::FieldFogMid },
            juce::Colour { BlueBreeze::Colors::GraphBgDeep },
            juce::Colour { BlueBreeze::Colors::SourceLight }.withAlpha(0.18f),
            juce::Colour { BlueBreeze::Colors::PanelBorder }.withAlpha(0.50f),
            juce::Colour { BlueBreeze::Colors::AccentBlue }.withAlpha(0.18f),
            juce::Colour { BlueBreeze::Colors::AccentBlue }.withAlpha(0.10f),
            juce::Colour { BlueBreeze::Colors::AccentBlue }.withAlpha(0.08f),
            juce::Colour { BlueBreeze::Colors::GraphBgDeep },
            juce::Colour { BlueBreeze::Colors::KnobEdge },
            juce::Colour { BlueBreeze::Colors::KnobGlow },

            BlueBreeze::Style::PanelRadius
        };

        return tokens;
    }

    static const ThemeTokens& darkBlueGreyTokens()
    {
        static const ThemeTokens tokens {
            juce::Colour { DarkBlueGrey::Colors::PrimaryBlue },
            juce::Colour { DarkBlueGrey::Colors::Accent },
            juce::Colour { DarkBlueGrey::Colors::LightBlue },
            juce::Colour { DarkBlueGrey::Colors::DarkBlue },

            juce::Colour { DarkBlueGrey::Colors::BackgroundDark },
            juce::Colour { DarkBlueGrey::Colors::BackgroundMedium },
            juce::Colour { DarkBlueGrey::Colors::BackgroundLight },

            juce::Colour { DarkBlueGrey::Colors::GradientTop },
            juce::Colour { DarkBlueGrey::Colors::GradientBottom },

            juce::Colour { DarkBlueGrey::Colors::PanelBorder },
            juce::Colour { DarkBlueGrey::Colors::ButtonNormal },
            juce::Colour { DarkBlueGrey::Colors::ButtonHover },
            juce::Colour { DarkBlueGrey::Colors::ButtonPressed },

            juce::Colour { DarkBlueGrey::Colors::BevelLight },
            juce::Colour { DarkBlueGrey::Colors::BevelDark },
            juce::Colour { DarkBlueGrey::Colors::GlowColor },

            juce::Colour { DarkBlueGrey::Colors::TextPrimary },
            juce::Colour { DarkBlueGrey::Colors::TextSecondary },
            juce::Colour { DarkBlueGrey::Colors::TextDisabled },
            juce::Colour { DarkBlueGrey::Colors::TextHighlight },

            juce::Colour { DarkBlueGrey::Colors::RollBackground },
            juce::Colour { DarkBlueGrey::Colors::LaneC },
            juce::Colour { DarkBlueGrey::Colors::LaneOther },
            juce::Colour { DarkBlueGrey::Colors::GridLine },

            juce::Colour { DarkBlueGrey::Colors::OriginalF0 },
            juce::Colour { DarkBlueGrey::Colors::CorrectedF0 },
            juce::Colour { DarkBlueGrey::Colors::ShadowTrack },

            juce::Colour { DarkBlueGrey::Colors::NoteBlock },
            juce::Colour { DarkBlueGrey::Colors::NoteBlockBorder },
            juce::Colour { DarkBlueGrey::Colors::NoteBlockSelected },
            juce::Colour { DarkBlueGrey::Colors::NoteBlockHover },

            juce::Colour { DarkBlueGrey::Colors::Playhead },
            juce::Colour { DarkBlueGrey::Colors::TimelineMarker },
            juce::Colour { DarkBlueGrey::Colors::BeatMarker },

            juce::Colour { DarkBlueGrey::Colors::ToolActive },
            juce::Colour { DarkBlueGrey::Colors::ToolInactive },
            juce::Colour { DarkBlueGrey::Colors::ButtonInactive },

            juce::Colour { DarkBlueGrey::Colors::StatusProcessing },
            juce::Colour { DarkBlueGrey::Colors::StatusReady },
            juce::Colour { DarkBlueGrey::Colors::StatusError },

            juce::Colour { DarkBlueGrey::Colors::WaveformFill },
            juce::Colour { DarkBlueGrey::Colors::WaveformOutline },

            juce::Colour { DarkBlueGrey::Colors::ScaleHighlight },

            juce::Colour { DarkBlueGrey::Colors::KnobBody },
            juce::Colour { DarkBlueGrey::Colors::KnobIndicator },
            juce::Colour { DarkBlueGrey::Colors::BackgroundDark },
            juce::Colour { DarkBlueGrey::Colors::BackgroundDark }.darker(0.18f),
            juce::Colour { DarkBlueGrey::Colors::PanelBorder },
            juce::Colour { DarkBlueGrey::Colors::TextPrimary },
            juce::Colour { DarkBlueGrey::Colors::TextDisabled },
            juce::Colour { DarkBlueGrey::Colors::BackgroundDark },
            juce::Colour { DarkBlueGrey::Colors::PanelBorder },
            juce::Colour { DarkBlueGrey::Colors::TextPrimary },
            juce::Colour { DarkBlueGrey::Colors::BackgroundDark },
            juce::Colour { DarkBlueGrey::Colors::PanelBorder },

            juce::Colour { DarkBlueGrey::Colors::BackgroundMedium },
            juce::Colour { DarkBlueGrey::Colors::BevelLight },
            juce::Colour { DarkBlueGrey::Colors::PanelBorder },
            juce::Colour { DarkBlueGrey::Colors::GlowColor }.withAlpha(0.25f),
            juce::Colour { DarkBlueGrey::Colors::ButtonNormal },
            juce::Colour { DarkBlueGrey::Colors::ButtonHover },
            juce::Colour { DarkBlueGrey::Colors::ButtonPressed },
            juce::Colour { DarkBlueGrey::Colors::RollBackground },
            juce::Colour { DarkBlueGrey::Colors::LaneC },
            juce::Colour { DarkBlueGrey::Colors::GridLine },
            juce::Colour { DarkBlueGrey::Colors::WaveformFill },
            juce::Colour { DarkBlueGrey::Colors::ShadowTrack },
            juce::Colour { DarkBlueGrey::Colors::BackgroundMedium },
            juce::Colour { DarkBlueGrey::Colors::BackgroundDark },
            juce::Colour { DarkBlueGrey::Colors::BackgroundDark }.darker(0.18f),
            juce::Colour { DarkBlueGrey::Colors::BevelLight }.withAlpha(0.16f),
            juce::Colour { DarkBlueGrey::Colors::PanelBorder }.withAlpha(0.64f),
            juce::Colour { DarkBlueGrey::Colors::GlowColor }.withAlpha(0.16f),
            juce::Colour { DarkBlueGrey::Colors::GlowColor }.withAlpha(0.08f),
            juce::Colour { DarkBlueGrey::Colors::GlowColor }.withAlpha(0.06f),
            juce::Colour { DarkBlueGrey::Colors::KnobIndicator },
            juce::Colour { DarkBlueGrey::Colors::GlowColor }.withAlpha(0.25f),

            DarkBlueGrey::Style::CornerRadius
        };

        return tokens;
    }

    static const ThemeStyle& auroraStyle()
    {
        static const ThemeStyle style {
            Aurora::Style::PanelRadius,
            Aurora::Style::ControlRadius,
            Aurora::Style::FieldRadius,
            Aurora::Style::ControlRadius, // knobRadius (using control radius for consistent look)

            Aurora::Style::StrokeThin,
            Aurora::Style::StrokeThick,
            2.0f, // focusRingThickness

            Aurora::Style::ShadowAlpha,
            Aurora::Style::ShadowRadius,
            { Aurora::Style::ShadowOffsetX, Aurora::Style::ShadowOffsetY },

            Aurora::Style::GlowAmount,
            12.0f, // Glow radius

            0.0f, // Bevel width (flat/glass)
            0.0f, // Bevel intensity

            200.0f, // Animation duration
            1.0f,   // Hover glow intensity

            juce::Colour { Aurora::Colors::Cyan },
            juce::Colour { Aurora::Colors::Warning },
            juce::Colour { Aurora::Colors::Error },
            juce::Colour { Aurora::Colors::Magenta },

            juce::Colour { Aurora::Colors::Cyan },
            juce::Colour { Aurora::Colors::TextDim },

            false // Time segment style
        };
        return style;
    }

    static const ThemeStyle& overdoseStyle()
    {
        static const ThemeStyle style {
            22.0f, 14.0f, 11.0f, 999.0f,
            1.0f, 2.0f, 2.0f,
            0.18f, 18, { 0, 6 },
            0.36f, 18.0f,
            1.5f, 0.22f,
            180.0f, 0.65f,
            juce::Colour(0xFF7CDCCC), juce::Colour(0xFFFF7CBD), juce::Colour(0xFFFFB030), juce::Colour(0xFFFF4F77),
            juce::Colour(0xFFFF7CBD), juce::Colour(0xFF7E8391),
            false // timeSegmentStyle
        };
        return style;
    }

    static const ThemeTokens& auroraTokens()
    {
        static const ThemeTokens tokens {
            juce::Colour { Aurora::Colors::Violet }, // primaryPurple
            juce::Colour { Aurora::Colors::Cyan },   // accent
            juce::Colour { Aurora::Colors::ElectricBlue }, // lightPurple
            juce::Colour { Aurora::Colors::BgDeep }, // darkPurple

            juce::Colour { Aurora::Colors::BgDeep },    // backgroundDark
            juce::Colour { Aurora::Colors::BgSurface }, // backgroundMedium
            juce::Colour { Aurora::Colors::BgOverlay }, // backgroundLight

            juce::Colour { Aurora::Colors::BgDeep }, // gradientTop
            juce::Colour { Aurora::Colors::BgDeep }, // gradientBottom

            juce::Colour { Aurora::Colors::BorderLight }, // panelBorder
            juce::Colour { Aurora::Colors::BgSurface },   // buttonNormal
            juce::Colour { Aurora::Colors::BorderGlow },  // buttonHover
            juce::Colour { Aurora::Colors::Cyan },        // buttonPressed

            juce::Colour { 0x00000000 }, // bevelLight
            juce::Colour { 0x00000000 }, // bevelDark
            juce::Colour { Aurora::Colors::Cyan }, // glowColor

            juce::Colour { Aurora::Colors::TextPrimary },
            juce::Colour { Aurora::Colors::TextSecondary },
            juce::Colour { Aurora::Colors::TextDim },
            juce::Colour { Aurora::Colors::Cyan }, // textHighlight

            juce::Colour { Aurora::Colors::PianoRollBg }, // rollBackground
            juce::Colour { 0x08FFFFFF }, // laneC
            juce::Colour { 0x03FFFFFF }, // laneOther
            juce::Colour { Aurora::Colors::BorderLight }, // gridLine

            juce::Colour { Aurora::Colors::OriginalF0 }, // originalF0
            juce::Colour { Aurora::Colors::CorrectedF0 }, // correctedF0
            juce::Colour { Aurora::Colors::ShadowTrack }, // shadowTrack

            juce::Colour { Aurora::Colors::NoteBlock }, // noteBlock
            juce::Colour { Aurora::Colors::NoteBlockEdge }, // noteBlockBorder
            juce::Colour { Aurora::Colors::NoteBlockHot }, // noteBlockSelected
            juce::Colour { Aurora::Colors::NoteBlockHover }, // noteBlockHover

            juce::Colour { Aurora::Colors::Cyan }, // playhead
            juce::Colour { Aurora::Colors::TextSecondary }, // timelineMarker
            juce::Colour { Aurora::Colors::TextDim }, // beatMarker

            juce::Colour { Aurora::Colors::Cyan }, // toolActive
            juce::Colour { Aurora::Colors::TextDim }, // toolInactive
            juce::Colour { Aurora::Colors::BgSurface }, // buttonInactive

            juce::Colour { Aurora::Colors::Warning }, // statusProcessing
            juce::Colour { Aurora::Colors::Success }, // statusReady
            juce::Colour { Aurora::Colors::Error }, // statusError

            juce::Colour { 0x600C3C4A }, // waveformFill
            juce::Colour { 0xFF0C3C4A }, // waveformOutline

            juce::Colour { 0x20FFFFFF }, // scaleHighlight

            juce::Colour { Aurora::Colors::KnobGlassBody },
            juce::Colour { Aurora::Colors::KnobIndicator },
            juce::Colour { Aurora::Colors::BgDeep },
            juce::Colour { Aurora::Colors::BgDeep }.darker(0.20f),
            juce::Colour { Aurora::Colors::GlassEdge },
            juce::Colour { Aurora::Colors::TextPrimary },
            juce::Colour { Aurora::Colors::TextDim },
            juce::Colour { Aurora::Colors::BgDeep },
            juce::Colour { Aurora::Colors::GlassEdge },
            juce::Colour { Aurora::Colors::TextPrimary },
            juce::Colour { Aurora::Colors::BgDeep },
            juce::Colour { Aurora::Colors::BorderLight },

            juce::Colour { Aurora::Colors::GlassSurface },
            juce::Colour { Aurora::Colors::GlassHighlight },
            juce::Colour { Aurora::Colors::GlassEdge },
            juce::Colour { Aurora::Colors::PanelGlow },
            juce::Colour { Aurora::Colors::ButtonNormal },
            juce::Colour { Aurora::Colors::ButtonHover },
            juce::Colour { Aurora::Colors::ButtonActive },
            juce::Colour { Aurora::Colors::PianoRollBg },
            juce::Colour { Aurora::Colors::PianoLane },
            juce::Colour { Aurora::Colors::PianoGrid },
            juce::Colour { Aurora::Colors::PianoWaveform },
            juce::Colour { Aurora::Colors::SidebarTrackFade },
            juce::Colour { Aurora::Colors::SidebarShellTop },
            juce::Colour { Aurora::Colors::SidebarShellMid },
            juce::Colour { Aurora::Colors::SidebarShellBottom },
            juce::Colour { Aurora::Colors::SidebarTopLip },
            juce::Colour { Aurora::Colors::SidebarOuterRim },
            juce::Colour { Aurora::Colors::SidebarInnerRim },
            juce::Colour { Aurora::Colors::SidebarEdgeAura },
            juce::Colour { Aurora::Colors::SidebarCornerBloom },
            juce::Colour { Aurora::Colors::KnobRim },
            juce::Colour { Aurora::Colors::KnobGlow },

            Aurora::Style::PanelRadius
        };
        return tokens;
    }

    static const ThemeTokens& overdoseTokens()
    {
        static const ThemeTokens tokens {
            juce::Colour(0xFFFF7CBD), juce::Colour(0xFFF098C0),
            juce::Colour(0xFFFFC8E4), juce::Colour(0xFFFF5FA8),
            juce::Colour(0xFFDCDCE4), juce::Colour(0xFFE8E8F0), juce::Colour(0xFFF4F0F6),
            juce::Colour(0xFFF9F1F7), juce::Colour(0xFFDCDCE4),
            juce::Colour(0x80FFD4E8), juce::Colour(0xFFE8E8F0), juce::Colour(0xFFF6F3FA), juce::Colour(0xFFD6D8E2),
            juce::Colour(0xCFFFFFFF), juce::Colour(0x3C8D8590), juce::Colour(0xFFFF7CBD),
            juce::Colour(0xFF343744), juce::Colour(0xFF7E8391), juce::Colour(0xFFB2B5C1), juce::Colour(0xFFFF7CBD),
            juce::Colour(0xFFE0E0E8), juce::Colour(0x22FFFFFF), juce::Colour(0x14D8D8E0), juce::Colour(0x45C8CAD3),
            juce::Colour(0xFFD24A3A), juce::Colour(0xFF196FC4), juce::Colour(0x30196FC4),
            juce::Colour(0xFF235AA8), juce::Colour(0xFF3A69A2), juce::Colour(0xFF2F6FC4), juce::Colour(0xFF2A63B8),
            juce::Colour(0xFFFF7CBD), juce::Colour(0xFFFF7CBD), juce::Colour(0x66AEB3C0),
            juce::Colour(0xFFFF7CBD), juce::Colour(0xFF7E8391), juce::Colour(0xFFE8E8F0),
            juce::Colour(0xFFFFB030), juce::Colour(0xFF7CDCCC), juce::Colour(0xFFFF4F77),
            juce::Colour(0x600C3C4A), juce::Colour(0xFF0C3C4A),
            juce::Colour(0x24FFD4E8),
            juce::Colour(0xFFF8F8FF), juce::Colour(0xFFFF7CBD), juce::Colour(0xFFF8F8FF), juce::Colour(0xFFDADDE6), juce::Colour(0xFFCCD0D8), juce::Colour(0xFF343744), juce::Colour(0xFFB2B5C1),
            juce::Colour(0xFF202830), juce::Colour(0xFF707880),
            juce::Colour(0xFFE8E8F0), juce::Colour(0xFF303840), juce::Colour(0xFFCED2DC),
            juce::Colour(0xDDF8F0F8), juce::Colour(0xBFFFFFFF), juce::Colour(0x80FFD4E8), juce::Colour(0x32FF80B8),
            juce::Colour(0xFFE8E8F0), juce::Colour(0xFFF6F3FA), juce::Colour(0xFFFFE0F0),
            juce::Colour(0xFFF4F0F6), juce::Colour(0x22FFFFFF), juce::Colour(0x70AEB3C0), juce::Colour(0xFF0C3C4A),
            juce::Colour(0xFFF4F0F6), juce::Colour(0xFFE8E8F0), juce::Colour(0xFFDADDE6),
            juce::Colour(0x30FFFFFF), juce::Colour(0x70AEB3C0), juce::Colour(0x24FFD4E8),
            juce::Colour(0x18FFD4E8), juce::Colour(0x14FFD4E8), juce::Colour(0xFFCCD0D8),
            juce::Colour(0xFFA8B0C0), juce::Colour(0x52FF80B8),
            22.0f
        };
        return tokens;
    }
};

} // namespace OpenTune
