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
        static const juce::uint32 GlassSurface   = 0xE80E2033; // Deeper neutral glass body
        static const juce::uint32 GlassHighlight = 0x30A8E0FF; // Restrained upper sheen
        static const juce::uint32 GlassEdge      = 0x8088BCE8; // Cooler, thinner default edge
        static const juce::uint32 PanelGlow      = 0x3E1D4D7E; // Subtle ambient cyan-blue aura
        static const juce::uint32 ButtonNormal   = 0xD00E2238; // Resting deep blue-black glass button
        static const juce::uint32 ButtonHover    = 0xD8142D49; // Hover glass button
        static const juce::uint32 ButtonActive   = 0xE51A568E; // Pressed/active restrained blue button

        // Top tray semantics
        static const juce::uint32 TrayTop         = 0xFF1B3350; // Subtle upper lift
        static const juce::uint32 TrayMid         = 0xFF122840; // Stable core tone
        static const juce::uint32 TrayBottom      = 0xFF0A1828; // Deep lower settle
        static const juce::uint32 TraySideGlow    = 0x2860B0E8; // Localized cool air, not full wash
        static const juce::uint32 TrayTopHighlight = 0x60BEE8FF; // Thin cold top edge
        static const juce::uint32 TrayInnerEdge   = 0x3AB0DCFF; // Inner glass lip
        static const juce::uint32 TrayOuterEdge   = 0x6A6098CC; // Outer structural frame

        // Right sidebar shell semantics
        static const juce::uint32 SidebarShellTop      = 0xFF142C45; // Restrained upper lift for tall shell
        static const juce::uint32 SidebarShellMid      = 0xFF0E2238; // Stable deep center mass
        static const juce::uint32 SidebarShellBottom   = 0xFF081523; // Lower settle into dark base
        static const juce::uint32 SidebarTopLip        = 0x4CA9D8FF; // Thin upper lip highlight
        static const juce::uint32 SidebarOuterRim      = 0x4A46719B; // Structural outer frame
        static const juce::uint32 SidebarInnerRim      = 0x2193CCFF; // Inner glass seam
        static const juce::uint32 SidebarEdgeAura      = 0x1A2D7FD0; // Faint edge-bound aura
        static const juce::uint32 SidebarCornerBloom   = 0x142E8BE4; // Corner bloom, not full-panel fog

        static const juce::uint32 PianoRollBg    = 0xFF0C1D2F; // Clean deep piano roll field
        static const juce::uint32 PianoLane      = 0xFF87B6D4; // Low contrast pitch lane
        static const juce::uint32 PianoGrid      = 0xFF9BD5FF; // Thin blue grid
        static const juce::uint32 PianoWaveform  = 0xFF0C3C4A; // Dark reference waveform
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
        static const juce::uint32 CorrectedF0   = 0xFF196FC4; // Piano Roll reference blue
        static const juce::uint32 ShadowTrack   = 0x30196FC4;
        static const juce::uint32 NoteBlock     = 0xFF235AA8; // Deep piano-roll note
        static const juce::uint32 NoteBlockEdge = 0xFF3A69A2; // Deep note edge
        static const juce::uint32 NoteBlockHot  = 0xFF2F6FC4; // Selected note accent
        static const juce::uint32 NoteBlockHover = 0xFF2A63B8;
        
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
