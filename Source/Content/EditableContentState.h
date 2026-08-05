#pragma once
#include "../DSP/ChromaKeyDetector.h"
#include "../Utils/ContentAnalysisState.h"
#include "../Utils/Note.h"
#include "../Utils/AutomationLane.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include "../DSP/ReferenceFeatures.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <memory>
#include <cstdint>

namespace OpenTune {

// 仅含可编辑字段。不含分析结果、store ID、placement、render queues、worker state、cache ownership。
struct EditableContentState
{
    // Audio data
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double audioSampleRate = 0.0;
    uint64_t audioRevision = 0;

    // Analysis state
    OriginalF0State originalF0State = OriginalF0State::NotRequested;
    DetectedKey detectedKey;

    // Musical content
    std::vector<Note> notes;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;
    PitchShiftSettings pitchShiftSettings;
    ReferenceFeatureSet referenceFeatures;
    AutomationLane volumeEnvelope;
    uint64_t notesRevision{0};
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};
    uint64_t outputGainRevision{0};
    uint64_t contentRevision{0};
};

} // namespace OpenTune
