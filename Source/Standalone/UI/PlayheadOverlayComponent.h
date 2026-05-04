#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune {

class PlayheadOverlayComponent : public juce::Component
{
public:
    PlayheadOverlayComponent();
    ~PlayheadOverlayComponent() override;

    void setPlayheadSeconds(double seconds) {
        playheadSeconds_ = seconds;
        repaint();
    }

    void setZoomLevel(double zoom) {
        zoomLevel_ = zoom;
        repaint();
    }

    void setScrollOffset(double offset) {
        scrollOffset_ = offset;
        repaint();
    }

    void setTimelineStartSeconds(double seconds) {
        timelineStartSeconds_ = seconds;
        repaint();
    }

    void setPianoKeyWidth(int width) {
        pianoKeyWidth_ = width;
        repaint();
    }

    void setPlaying(bool playing) {
        isPlaying_ = playing;
        repaint();
    }

    void setPlayheadColour(juce::Colour colour) { playheadColour_ = colour; }

private:
    void paint(juce::Graphics& g) override;
    double calculatePlayheadPixelX(double seconds) const;

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
