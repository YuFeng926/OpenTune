#include "PlayheadOverlayComponent.h"

namespace OpenTune {

PlayheadOverlayComponent::PlayheadOverlayComponent()
{
    setOpaque(false);
    setInterceptsMouseClicks(false, false);
}

PlayheadOverlayComponent::~PlayheadOverlayComponent() = default;

void PlayheadOverlayComponent::setPlayheadSeconds(double seconds) noexcept
{
    playheadSeconds_.store(seconds, std::memory_order_relaxed);

    if (!isPlaying_.load(std::memory_order_relaxed)) {
        return;
    }

    const double newPlayheadX = calculatePlayheadPixelX(seconds);
    const double lastX = lastPlayheadX_.load(std::memory_order_relaxed);

    if (lastX < 0.0 || std::fabs(newPlayheadX - lastX) > 0.01) {
        constexpr double lineHalfWidth = 10.0;
        const double minX = (lastX >= 0.0) ? std::min(lastX, newPlayheadX) : newPlayheadX;
        const double maxX = (lastX >= 0.0) ? std::max(lastX, newPlayheadX) : newPlayheadX;
        const int dirtyLeft = juce::jlimit(0, getWidth(), static_cast<int>(std::floor(minX - lineHalfWidth)));
        const int dirtyRight = juce::jlimit(0, getWidth(), static_cast<int>(std::ceil(maxX + lineHalfWidth)));

        if (dirtyRight > dirtyLeft) {
            repaint(juce::Rectangle<int>(dirtyLeft, 0, dirtyRight - dirtyLeft, getHeight()));
        } else {
            repaint();
        }

        lastPlayheadX_.store(newPlayheadX, std::memory_order_relaxed);
    }
}

void PlayheadOverlayComponent::paint(juce::Graphics& g)
{
    const double seconds = playheadSeconds_.load(std::memory_order_relaxed);

    const double playheadX = calculatePlayheadPixelX(seconds);
    const int pianoKeyW = pianoKeyWidth_.load(std::memory_order_relaxed);
    
    if (playheadX >= static_cast<double>(pianoKeyW) && playheadX < static_cast<double>(getWidth())) {
        const float playheadXf = static_cast<float>(playheadX);
        
        g.setColour(playheadColour_);
        g.drawLine(playheadXf, 0.0f, playheadXf, static_cast<float>(getHeight()), 2.0f);
        
        const float headSize = 6.0f;
        juce::Path head;
        head.addTriangle(playheadXf - headSize, 0.0f,
                         playheadXf + headSize, 0.0f,
                         playheadXf, headSize);
        g.fillPath(head);
    }
}

double PlayheadOverlayComponent::calculatePlayheadPixelX(double seconds) const
{
    const double zoom = zoomLevel_.load(std::memory_order_relaxed);
    const double scroll = scrollOffset_.load(std::memory_order_relaxed);
    const double trackOffset = trackOffsetSeconds_.load(std::memory_order_relaxed);
    const double alignmentOffset = alignmentOffsetSeconds_.load(std::memory_order_relaxed);
    const int pianoKeyW = pianoKeyWidth_.load(std::memory_order_relaxed);
    
    const double timeSeconds = seconds + alignmentOffset;
    const double adjustedTime = timeSeconds - trackOffset;
    if (adjustedTime < 0.0) return static_cast<double>(pianoKeyW);
    
    const double pixelsPerSecond = 100.0 * zoom;
    const double rawPixelX = adjustedTime * pixelsPerSecond - scroll + static_cast<double>(pianoKeyW);
    return rawPixelX;
}

} // namespace OpenTune
