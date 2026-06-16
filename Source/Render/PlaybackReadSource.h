#pragma once

#include "../Content/ContentKey.h"
#include "../Inference/RenderCache.h"
#include "../Inference/TimeStretchCache.h"
#include "../Utils/PitchShiftSettings.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <cstdint>

namespace OpenTune {

/**
 * PlaybackReadSource is the immutable read view consumed by render paths.
 *
 * Standalone and regular VST3 capture publish their owner audio here. ARA2
 * publishes a CRS-derived buffer rebuilt from AudioSource sample access. RenderCache
 * and TimeStretchCache are derived overlays keyed by the same ContentKey; they
 * never become persisted source truth.
 */
struct PlaybackReadSource
{
    ContentKey contentKey;

    std::shared_ptr<RenderCache> renderCache;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double audioSampleRate{0.0};
    TimeStretchCache* timeStretchCache{nullptr};

    uint64_t renderRevision{0};
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};

    PitchShiftSettings pitchShiftSettings;
    bool timeGridIsIdentity{true};

    bool hasAudio() const noexcept
    {
        return audioBuffer != nullptr && audioBuffer->getNumSamples() > 0;
    }

    bool canRead() const noexcept
    {
        return hasAudio();
    }
};

} // namespace OpenTune
