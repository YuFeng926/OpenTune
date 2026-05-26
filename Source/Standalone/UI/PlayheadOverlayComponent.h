#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune {

/** @brief Cheap translucent overlay that draws only the playhead line + triangle.

    Every setter computes the union of old and new narrow dirty rectangles
    and only repaints that union, never the full overlay area. Setters that
    do not actually change the effective value skip work entirely.
*/
class PlayheadOverlayComponent : public juce::Component
{
public:
    PlayheadOverlayComponent();
    ~PlayheadOverlayComponent() override;

    void setPlayheadSeconds(double seconds);
    void setZoomLevel(double zoom);
    void setScrollOffset(double offset);
    void setTimelineStartSeconds(double seconds);
    void setPianoKeyWidth(int width);
    void setPlaying(bool playing);

    void setPlayheadColour(juce::Colour colour) { playheadColour_ = colour; }

private:
    void paint(juce::Graphics& g) override;
    double calculatePlayheadPixelX(double seconds) const;

    /** Returns the dirty rectangle for a playhead at the given pixel X,
        clipped to the current component bounds.
        Width encloses: 2-pixel stroked line + triangle (±6 px) + margin.
    */
    juce::Rectangle<int> playheadDirtyRect(double pixelX) const;

    /** Repaints only the union of old and new playhead pixel positions. */
    void repaintPlayheadDirty(double oldPixelX, double newPixelX);

    double playheadSeconds_{0.0};
    double zoomLevel_{1.0};
    double scrollOffset_{0.0};
    double timelineStartSeconds_{0.0};
    int pianoKeyWidth_{60};
    bool isPlaying_{false};

    juce::Colour playheadColour_{0xFFE74C3C};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayheadOverlayComponent)
};

} // namespace OpenTune
