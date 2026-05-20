#pragma once

#include <juce_graphics/juce_graphics.h>

namespace OpenTune {
namespace Aurora {

    // Aurora Glass - Dark Neon & Frosted Glass Palette
    struct Colors
    {
        // Backgrounds
        static const juce::uint32 BgDeep        = 0xFF0A1624; // Soft blue-black field
        static const juce::uint32 BgSurface     = 0xFF0D1D2F; // Lifted inner surface
        static const juce::uint32 BgOverlay     = 0x1FFFFFFF; // Light Overlay

        // Aurora Glass v2 semantic surfaces
        static const juce::uint32 GlassSurface   = 0xD40E2237; // Lifted inner glass
        static const juce::uint32 GlassHighlight = 0x229EDFFF; // Fine upper sheen
        static const juce::uint32 GlassEdge      = 0x805CC8FF; // Crisp blue glass edge
        static const juce::uint32 PanelGlow      = 0x521A78D0; // Soft electric blue glow
        static const juce::uint32 ButtonNormal   = 0xC00B1728; // Resting dark glass button
        static const juce::uint32 ButtonHover    = 0xD1112A44; // Hover glass button
        static const juce::uint32 ButtonActive   = 0xE51B5F9E; // Pressed/active glass button
        static const juce::uint32 PianoRollBg    = 0xFF0C1D2F; // Clean deep piano roll field
        static const juce::uint32 PianoLane      = 0xFF87B6D4; // Low contrast pitch lane
        static const juce::uint32 PianoGrid      = 0xFF9BD5FF; // Thin blue grid
        static const juce::uint32 PianoWaveform  = 0xFF8EB8CE; // Luminous blue-grey waveform
        static const juce::uint32 SidebarTrackFade = 0x7A1F7BFF; // Track tint fade
        static const juce::uint32 KnobRim        = 0x9A4FC3FF; // Glass knob rim
        static const juce::uint32 KnobGlassBody  = 0xF0060B14; // Knob body
        static const juce::uint32 KnobGlow       = 0x821688FF; // Knob halo
        
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
        static const juce::uint32 OriginalF0    = 0xFFE0A128; // Deep glass gold
        static const juce::uint32 CorrectedF0   = 0xFF2DFFC4; // Bright cyan-green
        static const juce::uint32 NoteBlock     = 0xFF35C6EF; // Saturated piano-roll note
        static const juce::uint32 NoteBlockEdge = 0xFFB7F4FF; // Glassy note edge
        static const juce::uint32 NoteBlockHot  = 0xFF66E8FF; // Hovered note accent
        
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
