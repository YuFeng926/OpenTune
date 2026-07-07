#include "FixedPlayheadComponent.h"

namespace OpenTune {

FixedPlayheadComponent::FixedPlayheadComponent()
{
    setOpaque(false);
    setInterceptsMouseClicks(false, false);
}

FixedPlayheadComponent::~FixedPlayheadComponent() = default;

void FixedPlayheadComponent::setAnchorBounds(int anchorX, int height)
{
    if (anchorX_ == anchorX && height_ == height)
        return;
    anchorX_ = anchorX;
    height_ = height;
    repaint();
}

void FixedPlayheadComponent::setColour(juce::Colour colour)
{
    colour_ = colour;
    repaint();
}

void FixedPlayheadComponent::setVisible(bool visible)
{
    juce::Component::setVisible(visible);
}

void FixedPlayheadComponent::paint(juce::Graphics& g)
{
    if (height_ <= 0)
        return;

    // 固定播放头线
    g.setColour(colour_);
    g.drawLine(static_cast<float>(anchorX_), 0.0f,
               static_cast<float>(anchorX_), static_cast<float>(height_),
               2.0f);

    // 播放头三角
    const float triSize = 6.0f;
    const float cx = static_cast<float>(anchorX_);
    juce::Path tri;
    tri.addTriangle(cx - triSize, 0.0f,
                    cx + triSize, 0.0f,
                    cx, triSize);
    g.fillPath(tri);
}

} // namespace OpenTune
