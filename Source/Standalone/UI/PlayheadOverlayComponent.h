#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune {

struct PlayheadPresentation {
    double x;           // parent-component X coordinate (includes pianoKeyWidth)
    bool visible;
    juce::Colour colour;
};

class PlayheadOverlayComponent : public juce::Component {
public:
    PlayheadOverlayComponent()
    {
        setOpaque(false);
        setInterceptsMouseClicks(false, false);
    }

    void setPresentation(const PlayheadPresentation& presentation) {
        const double oldX = playheadX_;
        playheadX_ = presentation.x;
        visible_ = presentation.visible;
        colour_ = presentation.colour;
        
        repaintPlayheadDirty(oldX, playheadX_);
    }
    
    void setPlayheadColour(juce::Colour colour) { colour_ = colour; }

private:
    void paint(juce::Graphics& g) override {
        if (!visible_) return;
        
        const float xf = static_cast<float>(playheadX_);
        g.setColour(colour_);
        g.drawLine(xf, 0.0f, xf, static_cast<float>(getHeight()), 2.0f);
        
        const float ls = 6.0f;
        juce::Path head;
        head.addTriangle(xf - ls, 0.0f, xf + ls, 0.0f, xf, ls);
        g.fillPath(head);
    }
    
    void repaintPlayheadDirty(double oldX, double newX) {
        const int cx1 = static_cast<int>(std::floor(std::min(oldX, newX))) - 10;
        const int cx2 = static_cast<int>(std::ceil(std::max(oldX, newX))) + 10;
        repaint(cx1, 0, cx2 - cx1, getHeight());
    }
    
    double playheadX_{0.0};
    bool visible_{false};
    juce::Colour colour_{0xFFE74C3C};
};

} // namespace OpenTune
