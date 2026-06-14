#pragma once

#include "../Content/ContentKey.h"
#include "../Inference/RenderCache.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include "../Utils/SilentGapDetector.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

struct RenderJob
{
    ContentKey contentKey;

    std::shared_ptr<RenderCache> renderCache;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double audioSampleRate{0.0};
    std::shared_ptr<PitchCurve> pitchCurve;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;
    PitchShiftSettings pitchShiftSettings;
    std::vector<SilentGap> silentGaps;

    double startSeconds{0.0};
    double endSeconds{0.0};
    int64_t startSample{0};
    int64_t endSampleExclusive{0};

    uint64_t targetRevision{0};
    uint64_t renderRevision{0};
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};
    uint64_t contentRevision{0};
};

} // namespace OpenTune