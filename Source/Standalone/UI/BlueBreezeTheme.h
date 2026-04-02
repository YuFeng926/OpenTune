#pragma once

#include <juce_graphics/juce_graphics.h>

namespace OpenTune {
namespace BlueBreeze {

    // Soothe2 Inspired Palette - Soft Precision
    struct Colors
    {
        // Graph Background (Main Work Area)
        static const juce::uint32 GraphBgDeep   = 0xFF7A8F9E; // Darker Blue-Grey
        static const juce::uint32 GraphBgMid    = 0xFF8CA2B0; // Mid Blue-Grey
        static const juce::uint32 GraphBgLight  = 0xFFAABCC7; // Lighter (for gradients)

        // Sidebar / Panels (Pale Fog)
        static const juce::uint32 SidebarBg     = 0xFFC6D4DD; 
        static const juce::uint32 PanelBorder   = 0xFFB0C0CC; 

        // Controls - 钢琴漆旋钮
        static const juce::uint32 KnobBody          = 0xFF1A1A1A; // 纯黑（钢琴漆底色）
        static const juce::uint32 KnobBodyLight     = 0xFF2A2A2A; // 亮部黑色
        static const juce::uint32 KnobHighlight     = 0x80FFFFFF; // 顶部高光（半透明白）
        static const juce::uint32 KnobEdge          = 0xFF404040; // 边缘金属光泽
        static const juce::uint32 KnobIndicator     = 0xFFE0E0E0; // 银白指针
        static const juce::uint32 KnobTrack         = 0x40FFFFFF; // 半透明白色轨道
        static const juce::uint32 KnobShadow        = 0x40000000; // 凸起阴影
        static const juce::uint32 KnobGlow          = 0x30FFFFFF; // 悬停发光

        // Interaction States
        static const juce::uint32 ActiveWhite   = 0xFFFFFFFF; // Pure White (Selected/Active)
        static const juce::uint32 HoverOverlay  = 0x1AFFFFFF; // White overlay for hover
        static const juce::uint32 ShadowColor   = 0xFF5A6A75; // Colored shadow (not pure black)

        // Accent / Nodes (Candy Colors)
        static const juce::uint32 NodeRed       = 0xFFE07A7A; // Soft Red
        static const juce::uint32 NodeYellow    = 0xFFF5D76E; // Soft Yellow
        static const juce::uint32 NodePurple    = 0xFF9B59B6; // Soft Purple
        static const juce::uint32 AccentBlue    = 0xFF60A5FA; // Soft Blue

        // Text
        static const juce::uint32 TextDark      = 0xFF2C3E50; // Dark text for light backgrounds
        static const juce::uint32 TextLight     = 0xFFF0F4F8; // Light text for dark backgrounds
        static const juce::uint32 TextDim       = 0xFF708090; // Dimmed text

        // Clip Colors (Soft Blue-Grey - blends with track background)
        static const juce::uint32 ClipGradientTop     = 0xFFC8D4E0; // Light blue-grey
        static const juce::uint32 ClipGradientBottom  = 0xFFA8B8C8; // Blue-grey with subtle gradient
        static const juce::uint32 ClipBorder          = 0xFF8A9CAD; // Grey-blue border
        static const juce::uint32 ClipSelectedTop     = 0xFFD0D8E0; // Selected state light grey-blue
        static const juce::uint32 ClipSelectedBottom  = 0xFFB8C8D8; // Selected state medium blue-grey
    };

    struct Style
    {
        static constexpr float PanelRadius      = 16.0f; // Increased from 8.0f
        static constexpr float ControlRadius    = 10.0f; // Increased from 6.0f
        static constexpr float KnobRadius       = 999.0f; // Circle
        
        static constexpr float StrokeThin       = 1.0f;
        static constexpr float StrokeThick      = 2.0f;
        
        static constexpr float ShadowAlpha      = 0.25f;
        static constexpr int   ShadowRadius     = 10;
        static constexpr float HoverGlowAmount  = 0.4f;
    };

} // namespace BlueBreeze
} // namespace OpenTune
