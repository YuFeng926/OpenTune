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

        // Aurora Glass v3 semantic surfaces
        static const juce::uint32 GlassSurface   = 0xE10B1827; // Deeper neutral glass body
        static const juce::uint32 GlassHighlight = 0x1A7CC5F4; // Restrained upper sheen
        static const juce::uint32 GlassEdge      = 0x666DA8D8; // Cooler, thinner default edge
        static const juce::uint32 PanelGlow      = 0x2A133964; // Subtle ambient cyan-blue aura
        static const juce::uint32 ButtonNormal   = 0xE10A1420; // Resting dark capsule
        static const juce::uint32 ButtonHover    = 0xE4101B29; // Hover dark capsule
        static const juce::uint32 ButtonActive   = 0xE8112437; // Active state keeps dark body

        // Top tray semantics
        static const juce::uint32 TrayTop         = 0xFF14263A; // Subtle upper lift
        static const juce::uint32 TrayMid         = 0xFF0D1A29; // Stable core tone
        static const juce::uint32 TrayBottom      = 0xFF08121D; // Deep lower settle
        static const juce::uint32 TraySideGlow    = 0x18367AC4; // Localized cool air, not full wash
        static const juce::uint32 TrayTopHighlight = 0x3E9FD6FF; // Thin cold top edge
        static const juce::uint32 TrayInnerEdge   = 0x2299CFFF; // Inner glass lip
        static const juce::uint32 TrayOuterEdge   = 0x4F4F83B2; // Outer structural frame

        // Button chrome semantics
        static const juce::uint32 ButtonFaceTop   = 0xFF182A3C; // Compressed top light
        static const juce::uint32 ButtonFaceMid   = 0xFF0D1826; // Dense centre mass
        static const juce::uint32 ButtonFaceBottom = 0xFF08111A; // Deep lower glass
        static const juce::uint32 ButtonSheen     = 0x2496D6FF; // Tighter top ridge sheen
        static const juce::uint32 ButtonInnerLight = 0x1687C3FF; // Narrower inner refraction
        static const juce::uint32 ButtonEdge      = 0x7989B2DC; // Resting cold edge
        static const juce::uint32 ButtonActiveEdge = 0xD07BCFFF; // Active cyan hotspot
        static const juce::uint32 ButtonCoreShadow = 0x6E01060D; // Centre cavity shade
        static const juce::uint32 ButtonRestGlow  = 0x10164A84; // Minimal idle halo
        static const juce::uint32 ButtonHoverGlow = 0x182965A6; // Hover edge aura
        static const juce::uint32 ButtonActiveGlow = 0x1F357DD6; // Active edge aura
        static const juce::uint32 ButtonActiveTint = 0x2E2C74BE; // Active internal tint, not full fill
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
        static const juce::uint32 OriginalF0    = 0xFFD24A3A; // Piano Roll reference red
        static const juce::uint32 CorrectedF0   = 0xFF2EC7F8; // Piano Roll reference cyan
        static const juce::uint32 NoteBlock     = 0xFF72D8F7; // Pale piano-roll note
        static const juce::uint32 NoteBlockEdge = 0xFFB6F0FF; // Glassy note edge
        static const juce::uint32 NoteBlockHot  = 0xFF9CEAFF; // Hovered note accent
        
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
