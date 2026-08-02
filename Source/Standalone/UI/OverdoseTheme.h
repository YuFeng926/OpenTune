#pragma once

#include <juce_graphics/juce_graphics.h>

namespace OpenTune {
namespace Overdose {

    struct Colors
    {
        // 暗紫灰背景（Overdose 2 新风格：护眼暗色底）
        static const juce::uint32 CanvasTop          = 0xFF9A93BB;
        static const juce::uint32 CanvasMid          = 0xFFA09CBC;
        static const juce::uint32 CanvasBottom       = 0xFF9590B8;

        static const juce::uint32 BackgroundDark     = 0xFF8A84A8;
        static const juce::uint32 BackgroundMedium   = 0xFF9A93BB;
        static const juce::uint32 BackgroundLight    = 0xFFA59FC0;
        static const juce::uint32 BackgroundWarm     = 0xFFB0A8C8;

        static const juce::uint32 GradientTop        = 0xFFA09CBC;
        static const juce::uint32 GradientBottom     = 0xFF8A84A8;

        // 浅紫玻璃面板（中等明度，立体感）
        static const juce::uint32 PanelTop           = 0xE8D1C5E5;
        static const juce::uint32 PanelMid           = 0xC8C4B9DB;
        static const juce::uint32 PanelBottom        = 0xB8B9AED0;
        static const juce::uint32 PanelOpaqueTop     = 0xFFD1C5E5;
        static const juce::uint32 PanelOpaqueMid     = 0xFFC4B9DB;
        static const juce::uint32 PanelOpaqueBottom  = 0xFFB9AED0;

        static const juce::uint32 PanelBorder        = 0xA0C4B9DB;
        static const juce::uint32 PanelBorderSoft    = 0x30D1C5E5;
        static const juce::uint32 PanelHighlight     = 0xDFFFFFFF;
        static const juce::uint32 PanelInsetShadow   = 0x3051406F;

        static const juce::uint32 PrimaryPink        = 0xFFFF2097;
        static const juce::uint32 AccentPink         = 0xFFC21A86;
        static const juce::uint32 HotPink            = 0xFFFF2097;
        static const juce::uint32 SoftPink           = 0xFFFFC8E4;
        static const juce::uint32 PalePink           = 0xFFFFE0F0;
        static const juce::uint32 PinkGlow           = 0x52FF2097;
        static const juce::uint32 PinkGlowSoft       = 0x24FF2097;
        static const juce::uint32 TrackCardActive    = 0xFFFFE9F5;
        static const juce::uint32 TrackCardInactive  = 0xFFF4ECFA;

        static const juce::uint32 SidebarTrackFade   = 0xFF9A93BB;
        static const juce::uint32 SidebarShellTop    = 0xFFB0A8C8;
        static const juce::uint32 SidebarShellMid    = 0xFFA59FC0;
        static const juce::uint32 SidebarShellBottom = 0x309A93BB;
        static const juce::uint32 SidebarTopLip      = 0x7051406F;
        static const juce::uint32 SidebarOuterRim    = 0x24FF2097;
        static const juce::uint32 SidebarInnerRim    = 0x18D1C5E5;
        static const juce::uint32 SidebarEdgeAura    = 0x14D1C5E5;
        static const juce::uint32 SidebarCornerBloom = 0xFFB0A8C8;

        static const juce::uint32 CandyOrange        = 0xFFF8A818;
        static const juce::uint32 CandyOrangeSoft    = 0xFFFFB030;
        static const juce::uint32 Mint               = 0xFF7CDCCC;
        static const juce::uint32 Sky                = 0xFF8CD0EC;

        static const juce::uint32 ButtonNormal       = 0xFFD1C5E5;
        static const juce::uint32 ButtonHover        = 0xFFD9CDE9;
        static const juce::uint32 ButtonPressed      = 0xFFB9AED0;

        static const juce::uint32 ButtonActiveTop    = 0xFFFF2097;
        static const juce::uint32 ButtonActiveMid    = 0xFFFF2097;
        static const juce::uint32 ButtonActiveBottom = 0xFFC21A86;
        static const juce::uint32 ButtonActiveGlow   = 0x66FF2097;

        static const juce::uint32 FieldTop           = 0xFFD1C5E5;
        static const juce::uint32 FieldMid           = 0xFFC4B9DB;
        static const juce::uint32 FieldBottom        = 0xFFB9AED0;
        static const juce::uint32 FieldEdge          = 0xFFB0A8C8;

        // 深色控件（时间码井等）：黑紫红
        static const juce::uint32 DarkControlFace    = 0xFF211730;
        static const juce::uint32 DarkControlEdge    = 0xFF38203F;
        static const juce::uint32 DarkControlPressed = 0xFF150D20;

        static const juce::uint32 SliderTrack        = 0x70B0A8C8;
        static const juce::uint32 SliderTrackVisual  = 0xFFB0A8C8;
        static const juce::uint32 SliderThumb        = 0xFFD1C5E5;
        static const juce::uint32 SliderThumbEdge    = 0xFFB0A8C8;
        static const juce::uint32 SliderDot          = 0xFFFF2097;

        static const juce::uint32 ScrollbarTrack     = 0x509A93BB;
        static const juce::uint32 ScrollbarThumb     = 0xF0FF2097;
        static const juce::uint32 ScrollbarThumbEdge = 0xFFC21A86;
        static const juce::uint32 ScrollbarThumbTop  = 0xFFFFD8E8;
        static const juce::uint32 ScrollbarThumbPink = 0xFFFF76BF;

        static const juce::uint32 TextPrimary        = 0xFF34285E;
        static const juce::uint32 TextSecondary      = 0xFF6A5A8A;
        static const juce::uint32 TextDisabled       = 0xFF8A7AA5;
        static const juce::uint32 TextHighlight      = 0xFFFF2097;
        static const juce::uint32 TextOnPink         = 0xFFFFFFFF;
        static const juce::uint32 TextOnDark         = 0xFFFFFFFF;
        static const juce::uint32 LoadingBackdrop    = 0xFF211730;
        static const juce::uint32 LoadingAccent      = 0xFFFF2097;

        // 滚动区（暗紫灰底）
        static const juce::uint32 RollBackground     = 0xFF9A93BB;
        static const juce::uint32 RollBackgroundTop  = 0xFFA09CBC;
        static const juce::uint32 LaneC              = 0x22D1C5E5;
        static const juce::uint32 LaneOther          = 0x149A93BB;
        static const juce::uint32 GridLine           = 0x80B0A8C8;
        static const juce::uint32 GridLineStrong     = 0xA0C4B9DB;

        static const juce::uint32 WaveformFill       = 0x60FF2097;
        static const juce::uint32 WaveformOutline    = 0xFFFF2097;
        static const juce::uint32 PianoWaveform      = 0xFFFF2097;

        static const juce::uint32 OriginalF0         = 0xFFFF2097;
        static const juce::uint32 CorrectedF0        = 0xFF6A92FF;
        static const juce::uint32 ShadowTrack        = 0x306A92FF;

        static const juce::uint32 NoteBlock          = 0x80FF2097;
        static const juce::uint32 NoteBlockBorder    = 0xFFFF2097;
        static const juce::uint32 NoteBlockSelected  = 0xA0FF2097;
        static const juce::uint32 NoteBlockHover     = 0x90FF2097;

        static const juce::uint32 Playhead           = 0xFFFF2097;
        static const juce::uint32 PlayheadGlow       = 0x50FF2097;
        static const juce::uint32 TimelineMarker     = 0xFFFF2097;
        static const juce::uint32 BeatMarker         = 0x66D1C5E5;

        // 钢琴键盘（暗色底）
        static const juce::uint32 KeyBedWhite        = 0xFFD1C5E5;
        static const juce::uint32 KeyBedWhiteBottom  = 0xFFB9AED0;
        static const juce::uint32 KeyBedBlack        = 0xFF2D2A3E;
        static const juce::uint32 KeyBedBlackBottom  = 0xFF1A1828;
        static const juce::uint32 KeyBedDivider      = 0xFFB0A8C8;
        static const juce::uint32 KeyPressedGlow     = 0x66FF2097;

        // 旋钮（浅紫玻璃）
        static const juce::uint32 KnobBody           = 0xFFD1C5E5;
        static const juce::uint32 KnobRim            = 0xFFB0A8C8;
        static const juce::uint32 KnobRimDark        = 0xFFC21A86;
        static const juce::uint32 KnobIndicator      = 0xFFFF2097;
        static const juce::uint32 KnobGlow           = 0x52FF2097;

        static const juce::uint32 CompactKnobBody    = 0xFFD1C5E5;
        static const juce::uint32 CompactKnobRim     = 0xFFB0A8C8;
        static const juce::uint32 CompactKnobPointer = 0xFFFF2097;

        static const juce::uint32 ToolActive         = 0xFFFF2097;
        static const juce::uint32 ToolInactive       = 0xFF8A7AA5;
        static const juce::uint32 ButtonInactive     = 0xFFB9AED0;
        static const juce::uint32 FocusRing          = 0xB0FF2097;

        static const juce::uint32 StatusProcessing   = 0xFFFFB030;
        static const juce::uint32 StatusReady        = 0xFF7CDCCC;
        static const juce::uint32 StatusError        = 0xFFFF4F77;

        static const juce::uint32 VULow              = 0xFF7CDCCC;
        static const juce::uint32 VUMid              = 0xFFFF2097;
        static const juce::uint32 VUHigh             = 0xFFFFB030;
        static const juce::uint32 VUClip             = 0xFFFF4F77;

        // 玻璃效果（暗色底适配）
        static const juce::uint32 GlassSurface       = 0xDDD1C5E5;
        static const juce::uint32 GlassHighlight     = 0xBFFFFFFF;
        static const juce::uint32 GlassEdge          = 0x80D1C5E5;
        static const juce::uint32 PanelGlow          = 0x32FF2097;
        static const juce::uint32 SoftShadow         = 0x3038294F;
    };

    struct Style
    {
        static constexpr float PanelRadius        = 14.0f;
        static constexpr float ControlRadius      = 10.0f;
        static constexpr float FieldRadius        = 10.0f;
        static constexpr float KnobRadius         = 999.0f;

        static constexpr float StrokeThin         = 1.0f;
        static constexpr float StrokeThick        = 1.5f;
        static constexpr float FocusRingThickness = 1.5f;

        static constexpr float ShadowAlpha        = 0.12f;
        static constexpr int   ShadowRadius       = 12;
        static constexpr int   ShadowOffsetX      = 0;
        static constexpr int   ShadowOffsetY      = 4;

        static constexpr float GlowAlpha          = 0.28f;
        static constexpr float GlowRadius         = 12.0f;

        static constexpr float BevelWidth         = 1.0f;
        static constexpr float BevelIntensity     = 0.18f;

        static constexpr float AnimationDurationMs = 180.0f;
        static constexpr float HoverGlowIntensity  = 0.55f;

        static constexpr float CornerRadius        = 14.0f;
    };

} // namespace Overdose
} // namespace OpenTune
