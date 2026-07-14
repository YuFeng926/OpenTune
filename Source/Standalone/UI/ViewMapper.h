#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdint>
#include "TimelineViewportCamera.h"

namespace OpenTune {

struct ViewMapper {
    double visibleStartSeconds{0.0};  // absolute timeline seconds at left edge of content area
    double pixelsPerSecond{TimelineViewportCamera::kDefaultPixelsPerSecond};
    int contentStartX{0};           // parent-component X where content area starts (e.g. pianoKeyWidth)
    int contentWidth{0};            // pixel width of the content area
    int contentHeight{0};           // pixel height of the content area
    float pixelsPerSemitone{1.0f};
    float verticalScrollOffset{0.0f};
    float maxMidi{127.0f};

    // Returns parent-component X for an absolute timeline position.
    int timeToX(double absoluteSeconds) const {
        const auto worldPixel = static_cast<int64_t>(
            std::llround(absoluteSeconds * pixelsPerSecond));
        const auto surfaceOriginPixel = static_cast<int64_t>(
            std::llround(visibleStartSeconds * pixelsPerSecond));
        return contentStartX + static_cast<int>(worldPixel - surfaceOriginPixel);
    }

    // Returns absolute timeline seconds for a parent-component X.
    double xToTime(int x) const {
        const auto surfaceOriginPixel = static_cast<int64_t>(
            std::llround(visibleStartSeconds * pixelsPerSecond));
        return (surfaceOriginPixel + x - contentStartX) / pixelsPerSecond;
    }
    
    float midiToY(float midi) const {
        return (maxMidi - midi) * pixelsPerSemitone - verticalScrollOffset;
    }
    
    float yToMidi(float y) const {
        return maxMidi - (y + verticalScrollOffset) / pixelsPerSemitone;
    }
    
    float freqToMidi(float hz) const {
        // 440 Hz = MIDI 69 (A4)
        return static_cast<float>(12.0 * std::log2(hz / 440.0) + 69.0) - 0.5f;
    }
    
    float midiToFreq(float midi) const {
        return static_cast<float>(juce::MidiMessage::getMidiNoteInHertz(juce::roundToInt(midi + 0.5f)));
    }
    
    float freqToY(float hz) const {
        return midiToY(freqToMidi(hz));
    }
    
    float yToFreq(float y) const {
        return midiToFreq(yToMidi(y));
    }
    

};

} // namespace OpenTune
