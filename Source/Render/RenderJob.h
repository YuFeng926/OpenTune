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
    enum class Kind : uint8_t {
        Stage1Render,
        Stage2Rebuild
    };

    Kind kind{Kind::Stage1Render};
    ContentKey contentKey;

    std::shared_ptr<RenderCache> renderCache;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double audioSampleRate{0.0};
    std::vector<SilentGap> silentGaps;

    double startSeconds{0.0};
    int64_t startSample{0};
    int64_t endSampleExclusive{0};

    uint64_t targetRevision{0};

    uint64_t contentRevision{0};      // 来自 EditableContentSnapshot，用于 reconcile 去重
    uint64_t pitchRevision{0};
    uint64_t pitchShiftRevision{0};
    uint64_t timeGridRevision{0};
};

} // namespace OpenTune
