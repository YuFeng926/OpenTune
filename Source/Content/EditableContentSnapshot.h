#pragma once
#include "../Utils/SourceWindow.h"
#include "../Utils/Note.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include "../Utils/MaterializationState.h"
#include "../Utils/SilentGapDetector.h"
#include "../DSP/ChromaKeyDetector.h"
#include "../DSP/ReferenceFeatures.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

struct EditableContentSnapshot
{
    SourceWindow sourceWindow;

    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double audioSampleRate{0.0};
    uint64_t audioRevision{0};

    std::vector<Note> notes;
    std::vector<CorrectedSegment> correctedSegments;
    std::shared_ptr<PitchCurve> pitchCurve;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;
    PitchShiftSettings pitchShiftSettings;

    OriginalF0State originalF0State{OriginalF0State::NotRequested};
    DetectedKey detectedKey;
    std::vector<SilentGap> silentGaps;
    ReferenceFeatureSet referenceFeatures;

    uint64_t notesRevision{0};
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};
    uint64_t contentRevision{0};
};

} // namespace OpenTune
