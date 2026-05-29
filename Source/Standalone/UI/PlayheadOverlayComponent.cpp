#include "PlayheadOverlayComponent.h"

namespace OpenTune {

// The playhead draws a 2 px line + a triangle of half-size 6 px.
// Allow generous margin so the dirty rect encloses both gracefully.
static constexpr int kPlayheadDirtyRectHalfWidth = 10;

PlayheadOverlayComponent::PlayheadOverlayComponent()
{
    setOpaque(false);
    setInterceptsMouseClicks(false, false);
}

PlayheadOverlayComponent::~PlayheadOverlayComponent() = default;

// ============================================================================
// Paint — draws exactly the same visual as before
// ============================================================================

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

// ============================================================================
// Dirty-rect helpers
// ============================================================================

juce::Rectangle<int> PlayheadOverlayComponent::playheadDirtyRect(double pixelX) const
{
    const int cx = static_cast<int>(pixelX);
    const int h = getHeight();
    if (h <= 0) return {};

    return juce::Rectangle<int>(cx - kPlayheadDirtyRectHalfWidth, 0,
                                2 * kPlayheadDirtyRectHalfWidth, h);
}

void PlayheadOverlayComponent::repaintPlayheadDirty(double oldPixelX, double newPixelX)
{
    const auto oldRect = playheadDirtyRect(oldPixelX);
    const auto newRect = playheadDirtyRect(newPixelX);

    const auto bounds = getLocalBounds();
    if (bounds.isEmpty()) return;

    // Repaint union of old and new narrow rects, clipped to component bounds
    const auto dirty = oldRect.getUnion(newRect).getIntersection(bounds);
    if (!dirty.isEmpty())
        repaint(dirty);
}

// ============================================================================
// Setters — each computes old/new pixel positions and repaints only the union
// ============================================================================

void PlayheadOverlayComponent::setPlayheadSeconds(double seconds)
{
    if (playheadSeconds_ == seconds)
        return;

    const double oldX = calculatePlayheadPixelX(playheadSeconds_);
    playheadSeconds_ = seconds;
    const double newX = calculatePlayheadPixelX(playheadSeconds_);
    repaintPlayheadDirty(oldX, newX);
}

void PlayheadOverlayComponent::setZoomLevel(double zoom)
{
    if (zoomLevel_ == zoom)
        return;

    const double oldX = calculatePlayheadPixelX(playheadSeconds_);
    zoomLevel_ = zoom;
    const double newX = calculatePlayheadPixelX(playheadSeconds_);
    repaintPlayheadDirty(oldX, newX);
}

void PlayheadOverlayComponent::setScrollOffset(double offset)
{
    if (scrollOffset_ == offset)
        return;

    const double oldX = calculatePlayheadPixelX(playheadSeconds_);
    scrollOffset_ = offset;
    const double newX = calculatePlayheadPixelX(playheadSeconds_);
    repaintPlayheadDirty(oldX, newX);
}

void PlayheadOverlayComponent::setTimelineStartSeconds(double seconds)
{
    if (timelineStartSeconds_ == seconds)
        return;

    const double oldX = calculatePlayheadPixelX(playheadSeconds_);
    timelineStartSeconds_ = seconds;
    const double newX = calculatePlayheadPixelX(playheadSeconds_);
    repaintPlayheadDirty(oldX, newX);
}

void PlayheadOverlayComponent::setPianoKeyWidth(int width)
{
    if (pianoKeyWidth_ == width)
        return;

    const double oldX = calculatePlayheadPixelX(playheadSeconds_);
    pianoKeyWidth_ = width;
    const double newX = calculatePlayheadPixelX(playheadSeconds_);
    repaintPlayheadDirty(oldX, newX);
}

void PlayheadOverlayComponent::setPinnedViewportX(double x)
{
    if (usePinnedViewportX_ && pinnedViewportX_ == x)
        return;

    const double oldX = calculatePlayheadPixelX(playheadSeconds_);
    pinnedViewportX_ = x;
    usePinnedViewportX_ = true;
    const double newX = calculatePlayheadPixelX(playheadSeconds_);
    repaintPlayheadDirty(oldX, newX);
}

void PlayheadOverlayComponent::clearPinnedViewportX()
{
    if (!usePinnedViewportX_)
        return;

    const double oldX = calculatePlayheadPixelX(playheadSeconds_);
    usePinnedViewportX_ = false;
    const double newX = calculatePlayheadPixelX(playheadSeconds_);
    repaintPlayheadDirty(oldX, newX);
}

void PlayheadOverlayComponent::setPlaying(bool playing)
{
    if (isPlaying_ == playing)
        return;

    isPlaying_ = playing;
    // Playing state change doesn't move the playhead, but triggers a visual
    // refresh (e.g. colour pulse in future). Repaint only the line strip.
    const double x = calculatePlayheadPixelX(playheadSeconds_);
    const auto dirty = playheadDirtyRect(x).getIntersection(getLocalBounds());
    if (!dirty.isEmpty())
        repaint(dirty);
}

// ============================================================================
// Coordinate math — unchanged
// ============================================================================

double PlayheadOverlayComponent::calculatePlayheadPixelX(double seconds) const
{
    if (usePinnedViewportX_) {
        return pinnedViewportX_;
    }

    const double visibleTime = seconds - timelineStartSeconds_;
    if (visibleTime < 0.0) return static_cast<double>(pianoKeyWidth_);

    return visibleTime * 100.0 * zoomLevel_ - scrollOffset_ + static_cast<double>(pianoKeyWidth_);
}

} // namespace OpenTune
