#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune::MouseTrailConfig {

enum class TrailTheme
{
    None = 0,
    Classic,
    Neon,
    Fire,
    Ocean,
    Galaxy,
    CherryBlossom,
    Matrix
};

inline TrailTheme& getCurrentTheme()
{
    static TrailTheme currentTheme = TrailTheme::Classic;
    return currentTheme;
}

inline bool isEnabled()
{
    return getCurrentTheme() != TrailTheme::None;
}

inline void setTheme(TrailTheme theme)
{
    getCurrentTheme() = theme;
}

inline TrailTheme getTheme()
{
    return getCurrentTheme();
}

struct TrailThemeStyle
{
    juce::Colour baseColor;
    juce::Colour accentColor;
    float thickness;
    float fadeSpeed;
    bool useGradient;
    float hueShift;
};

inline TrailThemeStyle getThemeStyle(TrailTheme theme)
{
    switch (theme)
    {
        case TrailTheme::Classic:
            return { juce::Colour(0xFF3B82F6), juce::Colour(0xFF60A5FA), 2.5f, 0.083f, false, 0.0f };
        case TrailTheme::Neon:
            return { juce::Colour(0xFFFF00FF), juce::Colour(0xFF00FFFF), 3.0f, 0.1f, true, 0.5f };
        case TrailTheme::Fire:
            return { juce::Colour(0xFFFF4500), juce::Colour(0xFFFFD700), 3.5f, 0.12f, true, 0.08f };
        case TrailTheme::Ocean:
            return { juce::Colour(0xFF0077BE), juce::Colour(0xFF00CED1), 2.0f, 0.06f, true, 0.55f };
        case TrailTheme::Galaxy:
            return { juce::Colour(0xFF8B5CF6), juce::Colour(0xFFEC4899), 2.8f, 0.09f, true, 0.75f };
        case TrailTheme::CherryBlossom:
            return { juce::Colour(0xFFFFB7C5), juce::Colour(0xFFFF69B4), 2.2f, 0.07f, true, 0.92f };
        case TrailTheme::Matrix:
            return { juce::Colour(0xFF00FF00), juce::Colour(0xFF00FF00), 1.8f, 0.15f, false, 0.33f };
        default:
            return { juce::Colour(0xFF3B82F6), juce::Colour(0xFF60A5FA), 2.5f, 0.083f, false, 0.0f };
    }
}

} // namespace OpenTune::MouseTrailConfig