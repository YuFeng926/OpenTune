#pragma once

#include "../Content/ContentKey.h"
#include "../Inference/RenderCache.h"
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
    std::vector<SilentGap> silentGaps;

    double startSeconds{0.0};
    int64_t startSample{0};
    int64_t endSampleExclusive{0};

    uint64_t targetRevision{0};
};

} // namespace OpenTune