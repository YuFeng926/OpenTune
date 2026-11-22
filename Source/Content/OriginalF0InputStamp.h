#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdint>

namespace OpenTune {

struct OriginalF0InputStamp
{
    juce::String sourcePersistentId;
    uint64_t sourceSampleGeneration{0};
    double sourceStartSeconds{0.0};
    double sourceEndSeconds{0.0};
    double sourceSampleRate{0.0};
    int64_t numSamples{0};
    int numChannels{0};

    bool operator==(const OriginalF0InputStamp& rhs) const noexcept
    {
        return sourcePersistentId == rhs.sourcePersistentId
            && sourceSampleGeneration == rhs.sourceSampleGeneration
            && std::abs(sourceStartSeconds - rhs.sourceStartSeconds) < 1e-9
            && std::abs(sourceEndSeconds - rhs.sourceEndSeconds) < 1e-9
            && std::abs(sourceSampleRate - rhs.sourceSampleRate) < 1e-9
            && numSamples == rhs.numSamples
            && numChannels == rhs.numChannels;
    }

    bool operator!=(const OriginalF0InputStamp& rhs) const noexcept { return !(*this == rhs); }
};

} // namespace OpenTune
