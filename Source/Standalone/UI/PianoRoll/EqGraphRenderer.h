/**
 * EQ Graph Renderer — curve rendering engine for per-note EQ
 * 
 * Migrated from Qt EQGraphWidget visual math:
 * - Log frequency → X coordinate (log scale)
 * - Gain dB → Y coordinate (linear scale)
 * - Curve sampling using NoteEqProcessor visual math functions
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>
#include <vector>

#include "../Utils/NoteEqSettings.h"
#include "../DSP/NoteEqProcessor.h"

namespace OpenTune {

class EqGraphRenderer {
public:
    // Frequency range (from Qt source)
    static constexpr double kMinFrequencyHz = 20.0;
    static constexpr double kMaxFrequencyHz = 20000.0;
    
    // Gain range (from Qt source)
    static constexpr double kDefaultGainRangeDb = 12.0;
    static constexpr double kMinGainDb = -24.0;
    static constexpr double kMaxGainDb = 24.0;
    
    // Curve sampling resolution
    static constexpr int kCurveSamples = 256;
    
    // Colors (from Qt source)
    static juce::Colour graphBackgroundColor() { return juce::Colour(22, 22, 22); }
    static juce::Colour gridColor() { return juce::Colour(50, 50, 50); }
    static juce::Colour axisColor() { return juce::Colour(80, 80, 80); }
    static juce::Colour curveColor() { return juce::Colour(0, 200, 100); }
    static juce::Colour anchorColor() { return juce::Colour(255, 255, 255); }
    static juce::Colour anchorHighlightColor() { return juce::Colour(0, 150, 255); }
    
    EqGraphRenderer() = default;
    
    // Set the graph bounds (in pixels)
    void setGraphBounds(juce::Rectangle<float> bounds) {
        graphBounds_ = bounds;
    }
    
    // Set the gain range (in dB)
    void setGainRangeDb(double rangeDb) {
        gainRangeDb_ = std::clamp(rangeDb, 6.0, 24.0);
    }
    
    // Coordinate mapping (log frequency → X, gain dB → Y)
    float freqToX(double frequencyHz) const {
        const double norm = std::log(frequencyHz / kMinFrequencyHz) 
                          / std::log(kMaxFrequencyHz / kMinFrequencyHz);
        return static_cast<float>(graphBounds_.getX() + std::clamp(norm, 0.0, 1.0) * graphBounds_.getWidth());
    }
    
    double xToFreq(float x) const {
        const double norm = std::clamp(
            static_cast<double>(x - graphBounds_.getX()) / std::max(1.0, static_cast<double>(graphBounds_.getWidth())),
            0.0, 1.0);
        return kMinFrequencyHz * std::pow(kMaxFrequencyHz / kMinFrequencyHz, norm);
    }
    
    float gainToY(double gainDb) const {
        const double minGain = -gainRangeDb_;
        const double maxGain = gainRangeDb_;
        const double norm = (gainDb - minGain) / (maxGain - minGain);
        return static_cast<float>(graphBounds_.getBottom() - std::clamp(norm, 0.0, 1.0) * graphBounds_.getHeight());
    }
    
    double yToGain(float y) const {
        const double minGain = -gainRangeDb_;
        const double maxGain = gainRangeDb_;
        const double norm = std::clamp(
            static_cast<double>(graphBounds_.getBottom() - y) / std::max(1.0, static_cast<double>(graphBounds_.getHeight())),
            0.0, 1.0);
        return minGain + norm * (maxGain - minGain);
    }
    
    // Draw the EQ graph background, grid, and curve
    void draw(juce::Graphics& g, const EqSettings& settings, bool showGrid = true) const;
    
    // Draw only the curve (for preview mode)
    void drawCurve(juce::Graphics& g, const EqSettings& settings) const;
    
    // Draw grid lines and frequency labels
    void drawGrid(juce::Graphics& g) const;
    
    // Draw anchor points for each band
    void drawAnchors(juce::Graphics& g, const EqSettings& settings, int selectedBand = -1) const;
    
    // Get anchor position for a band
    juce::Point<float> getAnchorPosition(const EqSettings& settings, int bandIndex) const;
    
    // Find which band anchor is at a given position (returns -1 if none)
    int findBandAtPosition(juce::Point<float> pos, const EqSettings& settings, float threshold = 10.0f) const;
    
    // Generate curve path for the given settings
    juce::Path generateCurvePath(const EqSettings& settings) const;

private:
    juce::Rectangle<float> graphBounds_;
    double gainRangeDb_ = kDefaultGainRangeDb;
};

} // namespace OpenTune