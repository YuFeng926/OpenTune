#include "EqGraphRenderer.h"

#include <cmath>
#include <algorithm>

namespace OpenTune {

void EqGraphRenderer::draw(juce::Graphics& g, const EqSettings& settings, bool showGrid) const
{
    // Background
    g.fillAll(graphBackgroundColor());
    
    if (showGrid) {
        drawGrid(g);
    }
    
    // Draw curve
    drawCurve(g, settings);
    
    // Draw anchors
    drawAnchors(g, settings);
}

void EqGraphRenderer::drawCurve(juce::Graphics& g, const EqSettings& settings) const
{
    if (!settings.active)
        return;
    
    const auto path = generateCurvePath(settings);
    
    // Draw filled area under curve
    juce::Path filledPath = path;
    filledPath.lineTo(graphBounds_.getRight(), gainToY(0.0));
    filledPath.lineTo(graphBounds_.getX(), gainToY(0.0));
    filledPath.closeSubPath();
    
    g.setColour(curveColor().withAlpha(0.2f));
    g.fillPath(filledPath);
    
    // Draw curve line
    g.setColour(curveColor());
    g.strokePath(path, juce::PathStrokeType(2.0f));
}

void EqGraphRenderer::drawGrid(juce::Graphics& g) const
{
    // Vertical grid lines (frequency)
    const std::vector<double> freqs = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    
    g.setColour(gridColor());
    for (double freq : freqs) {
        const float x = freqToX(freq);
        g.drawVerticalLine(static_cast<int>(x), graphBounds_.getX(), graphBounds_.getBottom());
    }
    
    // Horizontal grid lines (gain)
    const double stepDb = gainRangeDb_ <= 12.0 ? 3.0 : 6.0;
    const int steps = static_cast<int>(std::round(gainRangeDb_ / stepDb));
    
    for (int i = -steps; i <= steps; ++i) {
        const double gainDb = static_cast<double>(i) * stepDb;
        const float y = gainToY(gainDb);
        g.drawHorizontalLine(static_cast<int>(y), graphBounds_.getX(), graphBounds_.getRight());
    }
    
    // Zero line (thicker)
    g.setColour(axisColor());
    const float zeroY = gainToY(0.0);
    g.drawHorizontalLine(static_cast<int>(zeroY), graphBounds_.getX(), graphBounds_.getRight());
}

void EqGraphRenderer::drawAnchors(juce::Graphics& g, const EqSettings& settings, int selectedBand) const
{
    if (!settings.active)
        return;
    
    const float anchorRadius = 6.0f;
    
    for (int i = 0; i < EqSettings::kNumBands; ++i) {
        const auto pos = getAnchorPosition(settings, i);
        
        // Check if within graph bounds
        if (!graphBounds_.contains(pos))
            continue;
        
        // Draw anchor circle
        const bool isSelected = (i == selectedBand);
        g.setColour(isSelected ? anchorHighlightColor() : anchorColor());
        g.fillEllipse(pos.x - anchorRadius, pos.y - anchorRadius, 
                      anchorRadius * 2.0f, anchorRadius * 2.0f);
        
        // Draw band index label
        g.setColour(juce::Colours::black);
        g.setFont(10.0f);
        g.drawText(juce::String(i + 1), pos.x - 4.0f, pos.y - 6.0f, 8.0f, 12.0f,
                   juce::Justification::centred, false);
    }
}

juce::Point<float> EqGraphRenderer::getAnchorPosition(const EqSettings& settings, int bandIndex) const
{
    const auto& band = settings.bands[bandIndex];
    double gainDb;
    
    // Cut filters use cutoff dB at anchor position
    if (bandIndex == EqSettings::kLowCut || bandIndex == EqSettings::kHighCut) {
        gainDb = NoteEqProcessor::filterResponseDb(band, band.frequency);
    } else {
        gainDb = band.gainDb;
    }
    
    return juce::Point<float>(freqToX(band.frequency), gainToY(gainDb));
}

int EqGraphRenderer::findBandAtPosition(juce::Point<float> pos, const EqSettings& settings, float threshold) const
{
    if (!settings.active)
        return -1;
    
    int closestBand = -1;
    float closestDist = threshold;
    
    for (int i = 0; i < EqSettings::kNumBands; ++i) {
        const auto anchorPos = getAnchorPosition(settings, i);
        const float dist = pos.getDistanceFrom(anchorPos);
        
        if (dist < closestDist) {
            closestDist = dist;
            closestBand = i;
        }
    }
    
    return closestBand;
}

juce::Path EqGraphRenderer::generateCurvePath(const EqSettings& settings) const
{
    juce::Path path;
    
    const double minGain = -gainRangeDb_;
    const double maxGain = gainRangeDb_;
    
    for (int i = 0; i <= kCurveSamples; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kCurveSamples);
        const double freq = kMinFrequencyHz * std::pow(kMaxFrequencyHz / kMinFrequencyHz, t);
        
        double gainDb = NoteEqProcessor::totalResponseDb(settings, freq);
        gainDb = std::clamp(gainDb, minGain, maxGain);
        
        const float x = freqToX(freq);
        const float y = gainToY(gainDb);
        
        if (i == 0) {
            path.startNewSubPath(x, y);
        } else {
            path.lineTo(x, y);
        }
    }
    
    return path;
}

} // namespace OpenTune