#pragma once

#include <cmath>

namespace OpenTune {

struct PianoRollCoordinateMapper {
    double pixelsPerSecond = 100.0;
    int scrollOffsetPx = 0;
    int contentStartX = 60;
    float pixelsPerSemitone = 15.0f;
    float verticalScrollOffset = 0.0f;
    float maxMidi = 108.0f;

    int timeToX(double seconds) const noexcept {
        const double contentX = seconds * pixelsPerSecond;
        return static_cast<int>(std::llround(contentX - scrollOffsetPx)) + contentStartX;
    }

    double xToTime(int x) const noexcept {
        return static_cast<double>(x - contentStartX + scrollOffsetPx) / pixelsPerSecond;
    }

    float midiToY(float midi) const noexcept {
        return (maxMidi - midi) * pixelsPerSemitone - verticalScrollOffset;
    }

    float freqToMidi(float freq) const noexcept {
        if (freq <= 0.0f) return 0.0f;
        return 69.0f + 12.0f * std::log2(freq / 440.0f);
    }

    float freqToY(float freq) const noexcept {
        return midiToY(freqToMidi(freq));
    }
};

} // namespace OpenTune
