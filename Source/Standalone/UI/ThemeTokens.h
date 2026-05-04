#pragma once

#include <juce_graphics/juce_graphics.h>
#include "BlueBreezeTheme.h"
#include "DarkBlueGreyTheme.h"
#include "AuroraTheme.h"

namespace OpenTune {

enum class ThemeId : int
{
    BlueBreeze = 0,
    DarkBlueGrey = 1,
    Aurora = 2
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

            BlueBreeze::Style::ShadowAlpha,
            BlueBreeze::Style::ShadowRadius,
            { 0, 4 },

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
            juce::Colour { BlueBreeze::Colors::ActiveWhite },
            juce::Colour { 0xFFD6E4ED },
            juce::Colour { 0xFF5A6A75 },

            juce::Colour { BlueBreeze::Colors::SidebarBg },
            juce::Colour { 0xFFD4E0E8 },
            juce::Colour { 0xFFE7EEF3 },

            juce::Colour { BlueBreeze::Colors::GraphBgLight },
            juce::Colour { BlueBreeze::Colors::GraphBgDeep },

            juce::Colour { BlueBreeze::Colors::PanelBorder },
            juce::Colour { 0x00000000 },
            juce::Colour { BlueBreeze::Colors::HoverOverlay },
            juce::Colour { BlueBreeze::Colors::ActiveWhite },

            juce::Colour { 0xFFFFFFFF },
            juce::Colour { 0xFF8FA2AF },
            juce::Colour { BlueBreeze::Colors::AccentBlue },

            juce::Colour { BlueBreeze::Colors::TextDark },
            juce::Colour { BlueBreeze::Colors::TextDim },
            juce::Colour { 0xFFA1AFBA },
            juce::Colour { BlueBreeze::Colors::AccentBlue },

            juce::Colour { BlueBreeze::Colors::GraphBgLight }, // rollBackground
            juce::Colour { 0x0FFFFFFF },
            juce::Colour { 0x08FFFFFF },
            juce::Colour { 0x20FFFFFF },

            juce::Colour { BlueBreeze::Colors::NodeRed },
            juce::Colour { BlueBreeze::Colors::AccentBlue },
            juce::Colour { 0x3060A5FA },

            juce::Colour { BlueBreeze::Colors::NodeYellow },
            juce::Colour { 0xFFE5C75E },
            juce::Colour { 0xFFFFE8A0 },
            juce::Colour { 0xFFFBE090 },

            juce::Colour { BlueBreeze::Colors::ActiveWhite },
            juce::Colour { BlueBreeze::Colors::AccentBlue },
            juce::Colour { 0x303B4D5A },

            juce::Colour { BlueBreeze::Colors::ActiveWhite },
            juce::Colour { BlueBreeze::Colors::TextDim },
            juce::Colour { 0x18000000 },

            juce::Colour { 0xFFF39C12 },
            juce::Colour { 0xFF2ECC71 },
            juce::Colour { BlueBreeze::Colors::NodeRed },

            juce::Colour { 0x25FFFFFF },
            juce::Colour { 0x60FFFFFF },

            juce::Colour { 0x60FBBF24 },

            juce::Colour { BlueBreeze::Colors::KnobBody },
            juce::Colour { BlueBreeze::Colors::KnobIndicator },

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

            juce::Colour { Aurora::Colors::BgDeep }, // rollBackground
            juce::Colour { 0x08FFFFFF }, // laneC
            juce::Colour { 0x03FFFFFF }, // laneOther
            juce::Colour { Aurora::Colors::BorderLight }, // gridLine

            juce::Colour { 0xFFFF0000 }, // originalF0 - 正红色
            juce::Colour { 0xFF00FFFF },   // correctedF0 - 青色
            juce::Colour { 0x40000000 }, // shadowTrack

            juce::Colour { Aurora::Colors::Cyan }, // noteBlock
            juce::Colour { Aurora::Colors::TextPrimary }, // noteBlockBorder
            juce::Colour { Aurora::Colors::ElectricBlue }, // noteBlockSelected
            juce::Colour { Aurora::Colors::Magenta }, // noteBlockHover

            juce::Colour { Aurora::Colors::Cyan }, // playhead
            juce::Colour { Aurora::Colors::TextSecondary }, // timelineMarker
            juce::Colour { Aurora::Colors::TextDim }, // beatMarker

            juce::Colour { Aurora::Colors::Cyan }, // toolActive
            juce::Colour { Aurora::Colors::TextDim }, // toolInactive
            juce::Colour { Aurora::Colors::BgSurface }, // buttonInactive

            juce::Colour { Aurora::Colors::Warning }, // statusProcessing
            juce::Colour { Aurora::Colors::Success }, // statusReady
            juce::Colour { Aurora::Colors::Error }, // statusError

            juce::Colour { 0x4000F0FF }, // waveformFill
            juce::Colour { 0x8000F0FF }, // waveformOutline

            juce::Colour { 0x20FFFFFF }, // scaleHighlight

            juce::Colour { Aurora::Colors::KnobBody },
            juce::Colour { Aurora::Colors::KnobIndicator },

            Aurora::Style::PanelRadius
        };
        return tokens;
    }
};

} // namespace OpenTune
