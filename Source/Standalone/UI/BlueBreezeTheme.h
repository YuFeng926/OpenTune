#pragma once

#include <juce_graphics/juce_graphics.h>

namespace OpenTune {
namespace BlueBreeze {

    // Blue Breeze - misted light panels with one cool top-left light source.
    struct Colors
    {
        static const juce::uint32 CanvasTop     = 0xFFF9FCFF;
        static const juce::uint32 CanvasBottom  = 0xFFE4EDF5;
        static const juce::uint32 PanelTop      = 0xFFF4F8FC;
        static const juce::uint32 PanelBottom   = 0xFFD7E1EA;
        static const juce::uint32 PanelInset    = 0xFFA8B8C6;
        static const juce::uint32 SourceLight   = 0xECFFFFFF;

        static const juce::uint32 GraphBgDeep   = 0xFF8394A6;
        static const juce::uint32 GraphBgMid    = 0xFF9AACB9;
        static const juce::uint32 GraphBgLight  = 0xFFB8C5D1;
        static const juce::uint32 GridSoft      = 0xFFD3DCE4;
        static const juce::uint32 LaneSoft      = 0xFF94A4B2;
        static const juce::uint32 FieldFogTop   = 0xFFB0C0CD;
        static const juce::uint32 FieldFogMid   = 0xFFA1B1BF;
        static const juce::uint32 FieldFogBottom = 0xFF8B9CAD;
        static const juce::uint32 FieldFogRim   = 0xFFE8EEF4;
        static const juce::uint32 TrayTop       = 0xFFF1F6FB;
        static const juce::uint32 TrayBottom    = 0xFFD1DCE6;
        static const juce::uint32 TrayInset     = 0xFFB0BFCC;
        static const juce::uint32 DisplayTop    = 0xFF111D27;
        static const juce::uint32 DisplayMid    = 0xFF182632;
        static const juce::uint32 DisplayBottom = 0xFF080F17;
        static const juce::uint32 DisplayEdge   = 0xFF4A6A7D;
        static const juce::uint32 DisplayGlow   = 0x2F5BAFEA;
        static const juce::uint32 DisplayText   = 0xFFBED0DF;
        static const juce::uint32 DisplayTextDim = 0x339CB6C8;

        // Sidebar / Panels (Pale Fog)
        static const juce::uint32 SidebarBg     = CanvasBottom;
        static const juce::uint32 PanelBorder   = 0xFFA4B3C0;

        // Controls
        static const juce::uint32 ControlTop        = 0xFFFFFFFF;
        static const juce::uint32 ControlBottom     = 0xFFEAF1F7;
        static const juce::uint32 ControlPressed    = 0xFFDCE5EE;
        static const juce::uint32 ControlHover      = 0xFFFCFEFF;
        static const juce::uint32 ControlShadow     = 0x44687B8C;

        static const juce::uint32 KnobBody          = 0xFF080A0C;
        static const juce::uint32 KnobBodyLight     = 0xFF31363B;
        static const juce::uint32 KnobHighlight     = 0x98FBFDFE;
        static const juce::uint32 KnobEdge          = 0xFF677B8A;
        static const juce::uint32 KnobIndicator     = 0xFFF8FBFD;
        static const juce::uint32 KnobTrack         = 0x77808E9A;
        static const juce::uint32 KnobShadow        = 0x8A4A5B6B;
        static const juce::uint32 KnobGlow          = 0x2E4EA7E8;
        static const juce::uint32 DarkFaceTop       = 0xFF2C3742;
        static const juce::uint32 DarkFaceBottom    = 0xFF080A0C;
        static const juce::uint32 DarkFaceEdge      = 0xFF5C6B77;

        // Piano key bed
        static const juce::uint32 KeyBedTop         = 0xFFFFFFFF;
        static const juce::uint32 KeyBedBottom      = 0xFFF8FBFD;
        static const juce::uint32 KeyBedDivider     = 0xFFD9E2E9;
        static const juce::uint32 KeyBlackTop       = 0xFF111418;
        static const juce::uint32 KeyBlackBottom    = 0xFF020304;
        static const juce::uint32 KeyPressedGlow    = 0x5562B0EA;

        // Interaction States
        static const juce::uint32 ActiveWhite   = ControlTop;
        static const juce::uint32 HoverOverlay  = 0x5CEAF7FF;
        static const juce::uint32 ShadowColor   = 0xFF5F6D7B;

        // Accent / Nodes
        static const juce::uint32 NodeRed       = 0xFFE07A7A; // Soft Red
        static const juce::uint32 NodeYellow    = 0xFFC88426; // Warm gold
        static const juce::uint32 NodePurple    = 0xFF9B59B6; // Soft Purple
        static const juce::uint32 AccentBlue    = 0xFF63B1EA; // Clear note blue
        static const juce::uint32 AccentBlueSoft = 0xFFA7D8F7;
        static const juce::uint32 AccentGlow    = 0x3463B1EA;

        // Text
        static const juce::uint32 TextDark      = 0xFF2C3E50; // Dark text for light backgrounds
        static const juce::uint32 TextLight     = 0xFFF0F4F8; // Light text for dark backgrounds
        static const juce::uint32 TextDim       = 0xFF708090; // Dimmed text

        // Clip Colors (Soft blue-grey, lifted from the editor field)
        static const juce::uint32 ClipGradientTop     = 0xFFD2DDE8;
        static const juce::uint32 ClipGradientBottom  = 0xFFB7C6D5;
        static const juce::uint32 ClipBorder          = 0xFF8FA1B3;
        static const juce::uint32 ClipSelectedTop     = 0xFFD7E2EC;
        static const juce::uint32 ClipSelectedBottom  = 0xFFC4D2E0;
    };

    struct Style
    {
        static constexpr float PanelRadius      = 16.0f; // Increased from 8.0f
        static constexpr float ControlRadius    = 10.0f; // Increased from 6.0f
        static constexpr float KnobRadius       = 999.0f; // Circle
        
        static constexpr float StrokeThin       = 1.0f;
        static constexpr float StrokeThick      = 2.0f;
        
        static constexpr float ShadowAlpha      = 0.13f;
        static constexpr int   ShadowRadius     = 16;
        static constexpr float HoverGlowAmount  = 0.28f;
    };

} // namespace BlueBreeze
} // namespace OpenTune
