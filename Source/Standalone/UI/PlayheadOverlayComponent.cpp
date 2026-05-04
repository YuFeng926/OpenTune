#include "PlayheadOverlayComponent.h"

namespace OpenTune {

PlayheadOverlayComponent::PlayheadOverlayComponent()
{
    setOpaque(false);
    setInterceptsMouseClicks(false, false);
}

PlayheadOverlayComponent::~PlayheadOverlayComponent() = default;

void PlayheadOverlayComponent::paint(juce::Graphics& g)
{
    const double playheadX = calculatePlayheadPixelX(playheadSeconds_);

    if (playheadX >= static_cast<double>(pianoKeyWidth_) && playheadX < static_cast<double>(getWidth())) {
        const float xf = static_cast<float>(playheadX);

        g.setColour(playheadColour_);
        g.drawLine(xf, 0.0f, xf, static_cast<float>(getHeight()), 2.0f);

        const float hs = 6.0f;
        juce::Path head;
        head.addTriangle(xf - hs, 0.0f, xf + hs, 0.0f, xf, hs);
        g.fillPath(head);
    }
}

double PlayheadOverlayComponent::calculatePlayheadPixelX(double seconds) const
{
    const double visibleTime = seconds - timelineStartSeconds_;
    if (visibleTime < 0.0) return static_cast<double>(pianoKeyWidth_);

    return visibleTime * 100.0 * zoomLevel_ - scrollOffset_ + static_cast<double>(pianoKeyWidth_);
}

} // namespace OpenTune
