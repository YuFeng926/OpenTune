#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <cmath>

namespace OpenTune {

class PlayheadOverlayComponent : public juce::Component
{
public:
    PlayheadOverlayComponent();
    ~PlayheadOverlayComponent() override;

    void setPlayheadSeconds(double seconds) noexcept;

    void setZoomLevel(double zoom) noexcept {
        const double old = zoomLevel_.load(std::memory_order_relaxed);
        zoomLevel_.store(zoom, std::memory_order_relaxed);
        if (std::abs(old - zoom) > 1.0e-6) {
            lastPlayheadX_.store(-1.0, std::memory_order_relaxed);
            repaint();
        }
    }

    void setScrollOffset(double offset) noexcept {
        const double old = scrollOffset_.load(std::memory_order_relaxed);
        scrollOffset_.store(offset, std::memory_order_relaxed);
        if (std::abs(old - offset) > 1.0e-6) {
            lastPlayheadX_.store(-1.0, std::memory_order_relaxed);
            repaint();
        }
    }

    void setTrackOffsetSeconds(double offset) noexcept {
        const double old = trackOffsetSeconds_.load(std::memory_order_relaxed);
        trackOffsetSeconds_.store(offset, std::memory_order_relaxed);
        if (std::abs(old - offset) > 1.0e-6) {
            lastPlayheadX_.store(-1.0, std::memory_order_relaxed);
            repaint();
        }
    }

    void setAlignmentOffsetSeconds(double offset) noexcept {
        const double old = alignmentOffsetSeconds_.load(std::memory_order_relaxed);
        alignmentOffsetSeconds_.store(offset, std::memory_order_relaxed);
        if (std::abs(old - offset) > 1.0e-6) {
            lastPlayheadX_.store(-1.0, std::memory_order_relaxed);
            repaint();
        }
    }

    void setPianoKeyWidth(int width) noexcept {
        const int old = pianoKeyWidth_.load(std::memory_order_relaxed);
        pianoKeyWidth_.store(width, std::memory_order_relaxed);
        if (old != width) {
            lastPlayheadX_.store(-1.0, std::memory_order_relaxed);
            repaint();
        }
    }

    void setPlaying(bool playing) noexcept {
        const bool old = isPlaying_.load(std::memory_order_relaxed);
        isPlaying_.store(playing, std::memory_order_relaxed);
        if (old != playing) {
            if (!playing) {
                lastPlayheadX_.store(-1.0, std::memory_order_relaxed);
            }
            repaint();
        }
    }

    void setPlayheadColour(juce::Colour colour) {
        playheadColour_ = colour;
    }

private:
    void paint(juce::Graphics& g) override;

    double calculatePlayheadPixelX(double seconds) const;

    std::atomic<double> playheadSeconds_{0.0};
    std::atomic<double> zoomLevel_{1.0};
    std::atomic<double> scrollOffset_{0.0};
    std::atomic<double> trackOffsetSeconds_{0.0};
    std::atomic<double> alignmentOffsetSeconds_{0.0};
    std::atomic<int> pianoKeyWidth_{60};
    std::atomic<bool> isPlaying_{false};

    std::atomic<double> lastPlayheadX_{-1.0};

    juce::Colour playheadColour_{0xFFE74C3C};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayheadOverlayComponent)
};

} // namespace OpenTune
