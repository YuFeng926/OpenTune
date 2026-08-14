#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include "TimelineViewportCamera.h"
#include "../../Utils/TuningConfig.h"
#include "../../Utils/PianoRollVisualPreferences.h"

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
    PianoGridStyle gridStyle{PianoGridStyle::PianoLanes};

    // Returns parent-component X for an absolute timeline position.
    int timeToX(double absoluteSeconds) const {
        return contentStartX + static_cast<int>(
            std::llround((absoluteSeconds - visibleStartSeconds) * pixelsPerSecond));
    }

    // Returns absolute timeline seconds for a parent-component X.
    double xToTime(int x) const {
        return visibleStartSeconds + static_cast<double>(x - contentStartX) / pixelsPerSecond;
    }
    
    float midiToY(float midi) const {
        return (maxMidi - midi) * pixelsPerSemitone - verticalScrollOffset;
    }
    
    float yToMidi(float y) const {
        return maxMidi - (y + verticalScrollOffset) / pixelsPerSemitone;
    }
    
    float freqToMidi(float hz) const {
        float midi = static_cast<float>(12.0 * std::log2(hz / TuningConfig::currentTuningHz()) + 69.0);
        return (gridStyle == PianoGridStyle::EqualSpacing) ? midi : midi - 0.5f;
    }
    
    float midiToFreq(float midi) const {
        const float adjusted = (gridStyle == PianoGridStyle::EqualSpacing) ? midi : midi + 0.5f;
        return TuningConfig::currentTuningHz() * std::pow(2.0f, (adjusted - 69.0f) / 12.0f);
    }
    
    float freqToY(float hz) const {
        return midiToY(freqToMidi(hz));
    }
    
    float yToFreq(float y) const {
        return midiToFreq(yToMidi(y));
    }
    

};

} // namespace OpenTune
