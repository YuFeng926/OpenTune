#pragma once

#include <juce_graphics/juce_graphics.h>

namespace OpenTune {
namespace Overdose {

    struct Colors
    {
        static const juce::uint32 CanvasTop          = 0xFFF9F1F7;
        static const juce::uint32 CanvasMid          = 0xFFE8E8F0;
        static const juce::uint32 CanvasBottom       = 0xFFDCDCE4;

        static const juce::uint32 BackgroundDark     = 0xFFDCDCE4;
        static const juce::uint32 BackgroundMedium   = 0xFFE8E8F0;
        static const juce::uint32 BackgroundLight    = 0xFFF4F0F6;
        static const juce::uint32 BackgroundWarm     = 0xFFFFEAF4;

        static const juce::uint32 GradientTop        = 0xFFF9F1F7;
        static const juce::uint32 GradientBottom     = 0xFFDCDCE4;

        static const juce::uint32 PanelTop           = 0xE0F8F0F8;
        static const juce::uint32 PanelMid           = 0xB8E0E0E8;
        static const juce::uint32 PanelBottom        = 0xB0D0D0E0;
        static const juce::uint32 PanelOpaqueTop     = 0xFFF8F0F8;
        static const juce::uint32 PanelOpaqueMid     = 0xFFE0DDE6;
        static const juce::uint32 PanelOpaqueBottom  = 0xFFD0D0D8;

        static const juce::uint32 PanelBorder        = 0x80FFD4E8;
        static const juce::uint32 PanelBorderSoft    = 0x30FFE0EC;
        static const juce::uint32 PanelHighlight     = 0xCFFFFFFF;
        static const juce::uint32 PanelInsetShadow   = 0x308D8590;

        static const juce::uint32 PrimaryPink        = 0xFFFF7CBD;
        static const juce::uint32 AccentPink         = 0xFFF098C0;
        static const juce::uint32 HotPink            = 0xFFFF5FA8;
        static const juce::uint32 SoftPink           = 0xFFFFC8E4;
        static const juce::uint32 PalePink           = 0xFFFFE0F0;
        static const juce::uint32 PinkGlow           = 0x52FF80B8;
        static const juce::uint32 PinkGlowSoft       = 0x24FFD4E8;

        static const juce::uint32 CandyOrange        = 0xFFF8A818;
        static const juce::uint32 CandyOrangeSoft    = 0xFFFFB030;
        static const juce::uint32 Mint               = 0xFF7CDCCC;
        static const juce::uint32 Sky                = 0xFF8CD0EC;

        static const juce::uint32 ButtonNormal       = 0xFFE8E8F0;
        static const juce::uint32 ButtonHover        = 0xFFF6F3FA;
        static const juce::uint32 ButtonPressed      = 0xFFD6D8E2;

        static const juce::uint32 ButtonActiveTop    = 0xFFFFE0F0;
        static const juce::uint32 ButtonActiveMid    = 0xFFFFC8E4;
        static const juce::uint32 ButtonActiveBottom = 0xFFF098C0;
        static const juce::uint32 ButtonActiveGlow   = 0x66FF80B8;

        static const juce::uint32 FieldTop           = 0xFFF8F8FF;
        static const juce::uint32 FieldMid           = 0xFFEAEAF2;
        static const juce::uint32 FieldBottom        = 0xFFDADDE6;
        static const juce::uint32 FieldEdge          = 0xFFCCD0D8;

        static const juce::uint32 DarkControlFace    = 0xFF202830;
        static const juce::uint32 DarkControlEdge    = 0xFF707880;
        static const juce::uint32 DarkControlPressed = 0xFF101820;

        static const juce::uint32 SliderTrack        = 0x70202830;
        static const juce::uint32 SliderTrackVisual  = 0xFFA5A6AE;
        static const juce::uint32 SliderThumb        = 0xFFF8F8FF;
        static const juce::uint32 SliderThumbEdge    = 0xFFA8B0C0;
        static const juce::uint32 SliderDot          = 0xFFFF7CC0;

        static const juce::uint32 ScrollbarTrack     = 0x50182020;
        static const juce::uint32 ScrollbarThumb     = 0xF0F8F0F8;
        static const juce::uint32 ScrollbarThumbEdge = 0xFFE0E0E8;

        static const juce::uint32 TextPrimary        = 0xFF343744;
        static const juce::uint32 TextSecondary      = 0xFF7E8391;
        static const juce::uint32 TextDisabled       = 0xFFB2B5C1;
        static const juce::uint32 TextHighlight      = 0xFFFF7CBD;
        static const juce::uint32 TextOnPink         = 0xFFFFFFFF;
        static const juce::uint32 TextOnDark         = 0xFFF8F8FF;

        static const juce::uint32 RollBackground     = 0xFFE0E0E8;
        static const juce::uint32 RollBackgroundTop  = 0xFFF4F0F6;
        static const juce::uint32 LaneC              = 0x22FFFFFF;
        static const juce::uint32 LaneOther          = 0x14D8D8E0;
        static const juce::uint32 GridLine           = 0x45C8CAD3;
        static const juce::uint32 GridLineStrong     = 0x70AEB3C0;

        static const juce::uint32 WaveformFill       = 0x66B8BBC6;
        static const juce::uint32 WaveformOutline    = 0x90AEB3C0;

        static const juce::uint32 OriginalF0         = 0xFFD24A3A;
        static const juce::uint32 CorrectedF0        = 0xFF2EC7F8;
        static const juce::uint32 ShadowTrack        = 0x302EC7F8;

        static const juce::uint32 NoteBlock          = 0xFF72D8F7;
        static const juce::uint32 NoteBlockBorder    = 0xFFB6F0FF;
        static const juce::uint32 NoteBlockSelected  = 0xFF9CEAFF;
        static const juce::uint32 NoteBlockHover     = 0xFF8CE4FF;

        static const juce::uint32 Playhead           = 0xFFFF7CBD;
        static const juce::uint32 PlayheadGlow       = 0x50FF80BD;
        static const juce::uint32 TimelineMarker     = 0xFFFF7CBD;
        static const juce::uint32 BeatMarker         = 0x66AEB3C0;

        static const juce::uint32 KeyBedWhite        = 0xFFE8E8F0;
        static const juce::uint32 KeyBedWhiteBottom  = 0xFFE0E8E8;
        static const juce::uint32 KeyBedBlack        = 0xFF303840;
        static const juce::uint32 KeyBedBlackBottom  = 0xFF202830;
        static const juce::uint32 KeyBedDivider      = 0xFFCED2DC;
        static const juce::uint32 KeyPressedGlow     = 0x66FF80BD;

        static const juce::uint32 KnobBody           = 0xFFF8F8FF;
        static const juce::uint32 KnobRim            = 0xFFA8B0C0;
        static const juce::uint32 KnobRimDark        = 0xFF707888;
        static const juce::uint32 KnobIndicator      = 0xFFFF7CBD;
        static const juce::uint32 KnobGlow           = 0x52FF80B8;

        static const juce::uint32 CompactKnobBody    = 0xFF101818;
        static const juce::uint32 CompactKnobRim     = 0xFF405050;
        static const juce::uint32 CompactKnobPointer = 0xFFF8C878;

        static const juce::uint32 ToolActive         = 0xFFFF7CBD;
        static const juce::uint32 ToolInactive       = 0xFF7E8391;
        static const juce::uint32 ButtonInactive     = 0xFFE8E8F0;
        static const juce::uint32 FocusRing          = 0xB0FF80BD;

        static const juce::uint32 StatusProcessing   = 0xFFFFB030;
        static const juce::uint32 StatusReady        = 0xFF7CDCCC;
        static const juce::uint32 StatusError        = 0xFFFF4F77;

        static const juce::uint32 VULow              = 0xFF7CDCCC;
        static const juce::uint32 VUMid              = 0xFFFF7CBD;
        static const juce::uint32 VUHigh             = 0xFFFFB030;
        static const juce::uint32 VUClip             = 0xFFFF4F77;

        static const juce::uint32 GlassSurface       = 0xDDF8F0F8;
        static const juce::uint32 GlassHighlight     = 0xBFFFFFFF;
        static const juce::uint32 GlassEdge          = 0x80FFD4E8;
        static const juce::uint32 PanelGlow          = 0x32FF80B8;
        static const juce::uint32 SoftShadow         = 0x3C8D8590;
    };

    struct Style
    {
        static constexpr float PanelRadius        = 22.0f;
        static constexpr float ControlRadius      = 14.0f;
        static constexpr float FieldRadius        = 11.0f;
        static constexpr float KnobRadius         = 999.0f;

        static constexpr float StrokeThin         = 1.0f;
        static constexpr float StrokeThick        = 2.0f;
        static constexpr float FocusRingThickness = 2.0f;

        static constexpr float ShadowAlpha        = 0.18f;
        static constexpr int   ShadowRadius       = 18;
        static constexpr int   ShadowOffsetX      = 0;
        static constexpr int   ShadowOffsetY      = 6;

        static constexpr float GlowAlpha          = 0.36f;
        static constexpr float GlowRadius         = 18.0f;

        static constexpr float BevelWidth         = 1.5f;
        static constexpr float BevelIntensity     = 0.22f;

        static constexpr float AnimationDurationMs = 180.0f;
        static constexpr float HoverGlowIntensity  = 0.65f;

        static constexpr float CornerRadius        = 22.0f;
    };

} // namespace Overdose
} // namespace OpenTune
