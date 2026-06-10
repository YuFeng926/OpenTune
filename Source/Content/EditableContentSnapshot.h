#pragma once
#include "AudioModificationContentState.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <cstdint>

namespace OpenTune {

// 不可变快照。render/cache/worker 服务只能消费此对象。不含 render/cache/worker 所有权。
struct EditableContentSnapshot
{
    SourceWindow sourceWindow;
    std::vector<Note> notes;
    std::shared_ptr<PitchCurve> pitchCurve;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;
    PitchShiftSettings pitchShiftSettings;
    OriginalF0State originalF0State{OriginalF0State::NotRequested};
    DetectedKey detectedKey;
    uint64_t notesRevision{0};
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};
    uint64_t contentRevision{0};

    // Standalone domain extended fields
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double audioSampleRate{0.0};
    uint64_t audioRevision{0};
};

} // namespace OpenTune
