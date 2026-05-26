#pragma once

/**
 * TimelineViewportState
 *
 * Shared horizontal time/pixel math for PianoRoll and Arrangement.
 * Value object — no mutable state beyond what is explicitly set.
 * Owns zoom, scroll offset, viewport bounds, and content start X.
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>

namespace OpenTune {

struct TimelineViewportState {
    double zoomLevel = 1.0;
    int scrollOffsetPx = 0;
    int viewportWidthPx = 800;
    int viewportHeightPx = 600;
    int contentStartX = 8;

    static constexpr double kPixelsPerSecondBase = 100.0;

    /** Total pixels per timeline second at current zoom. */
    double pixelsPerSecond() const noexcept { return kPixelsPerSecondBase * zoomLevel; }

    /** Content-space X for a timeline time (seconds). */
    int timeToContentX(double seconds) const noexcept {
        return static_cast<int>(std::llround(seconds * pixelsPerSecond()));
    }

    /** Viewport-space X for a timeline time (seconds) — uses current scroll offset. */
    int timeToViewportX(double seconds) const noexcept {
        return timeToViewportX(seconds, scrollOffsetPx);
    }

    /** Viewport-space X with a given scroll offset. */
    int timeToViewportX(double seconds, int projectedScrollOffset) const noexcept {
        const double contentX = static_cast<double>(timeToContentX(seconds));
        return static_cast<int>(std::llround(contentX - projectedScrollOffset)) + contentStartX;
    }

    /** Timeline time for a viewport-space X — uses current scroll offset. */
    double viewportXToTime(int x) const noexcept {
        return (static_cast<double>(x - contentStartX + scrollOffsetPx)) / pixelsPerSecond();
    }

    /** Timeline time for a viewport-space X with a given scroll offset. */
    double viewportXToTime(int x, int scrollOffset) const noexcept {
        return (static_cast<double>(x - contentStartX + scrollOffset)) / pixelsPerSecond();
    }

    /** Visible time range start. */
    double visibleTimeStart() const noexcept {
        return viewportXToTime(0);
    }

    /** Visible time range end. */
    double visibleTimeEnd() const noexcept {
        return viewportXToTime(viewportWidthPx);
    }

    /**
     * Computes the exposed strip rectangle when scrolling horizontally.
     * Returns an empty rect if delta is zero or if the scroll would require
     * a full redraw (e.g. zoom or viewport width changed).
     */
    juce::Rectangle<int> exposedStripForScrollDelta(int oldScrollOffset,
                                                     int newScrollOffset) const noexcept {
        if (oldScrollOffset == newScrollOffset)
            return {};

        const int delta = newScrollOffset - oldScrollOffset;
        if (std::abs(delta) >= viewportWidthPx)
            return {}; // full redraw if scrolled more than viewport width

        if (delta > 0) {
            // Scrolled right — exposed strip on the right
            return { viewportWidthPx - delta, 0, delta, viewportHeightPx };
        } else {
            // Scrolled left — exposed strip on the left
            return { 0, 0, -delta, viewportHeightPx };
        }
    }

    /** Returns true if delta is large enough to warrant a full redraw rather than exposed-strip. */
    bool requiresFullRedrawForDelta(int oldScrollOffset, int newScrollOffset) const noexcept {
        if (oldScrollOffset == newScrollOffset)
            return false;
        const int delta = newScrollOffset - oldScrollOffset;
        return std::abs(delta) >= viewportWidthPx;
    }
};

} // namespace OpenTune
