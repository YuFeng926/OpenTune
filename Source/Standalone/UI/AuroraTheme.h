#pragma once

#include <juce_graphics/juce_graphics.h>

namespace OpenTune {
namespace Aurora {

    // Aurora Glass - Dark Neon & Frosted Glass Palette
    struct Colors
    {
        // Backgrounds
        static const juce::uint32 BgDeep        = 0xFF151B22; // Deep Blue Black
        static const juce::uint32 BgSurface     = 0xFF1A2332; // Card Surface (#1A2332)
        static const juce::uint32 BgOverlay     = 0x1FFFFFFF; // Light Overlay
        
        // Borders
        static const juce::uint32 BorderLight   = 0x33FFFFFF; // Subtle Border
        static const juce::uint32 BorderGlow    = 0x66FFFFFF; // Highlight Edge
        
        // Accents (Neon Rainbow)
        static const juce::uint32 Cyan          = 0xFF3B82F6; // Blue (#3B82F6)
        static const juce::uint32 Violet        = 0xFF8B5CF6; // Violet
        static const juce::uint32 ElectricBlue  = 0xFF0070FF; // Electric Blue
        static const juce::uint32 Magenta       = 0xFFEC4899; // Pink
        static const juce::uint32 NeonGreen     = 0xFF22C55E; // Green (#22C55E)
        static const juce::uint32 NeonOrange    = 0xFFF97316; // Orange (#F97316)
        static const juce::uint32 NeonRed       = 0xFFEF4444; // Red (#EF4444)
        static const juce::uint32 NeonYellow    = 0xFFEAB308; // Yellow
        
        // Text
        static const juce::uint32 TextPrimary   = 0xFFFFFFFF; // Pure White
        static const juce::uint32 TextSecondary = 0x99FFFFFF; // 60% White
        static const juce::uint32 TextDim       = 0x66FFFFFF; // 40% White
        
        // Status
        static const juce::uint32 Success       = 0xFF22C55E; // Neon Green
        static const juce::uint32 Warning       = 0xFFEAB308; // Neon Yellow
        static const juce::uint32 Error         = 0xFFEF4444; // Neon Red
        
        // Controls
        static const juce::uint32 KnobBody      = 0xFF1A2332; // Dark Body
        static const juce::uint32 KnobIndicator = 0xFF3B82F6; // Blue Indicator
    };

    struct Style
    {
        static constexpr float PanelRadius      = 16.0f; // Unified with BlueBreeze
        static constexpr float ControlRadius    = 10.0f; // Unified with BlueBreeze
        static constexpr float FieldRadius      = 10.0f; // Unified with BlueBreeze
        
        static constexpr float StrokeThin       = 1.0f;
        static constexpr float StrokeThick      = 2.0f;
        
        static constexpr float ShadowAlpha      = 0.25f; // Unified with BlueBreeze
        static constexpr int   ShadowRadius     = 10;    // Unified with BlueBreeze
        static constexpr int   ShadowOffsetX    = 0;     // Unified with BlueBreeze
        static constexpr int   ShadowOffsetY    = 4;     // Unified with BlueBreeze
        
        static constexpr float GlowAmount       = 0.6f; // For neon effects
    };

} // namespace Aurora
} // namespace OpenTune
