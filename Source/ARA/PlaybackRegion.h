#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace OpenTune {

struct PlaybackRegion
{
    juce::ARAPlaybackRegion* playbackRegion{nullptr};
    juce::String audioModificationPersistentId;
    double startInPlaybackTime{0.0};
    double startInModificationTime{0.0};
    double durationInPlaybackTime{0.0};
    double durationInModificationTime{0.0};
    bool timestretchEnabled{false};
    bool timestretchReflectingTempo{false};
    bool contentBasedFadeAtHead{false};
    bool contentBasedFadeAtTail{false};
    uint64_t placementRevision{0};

    void updateFrom(juce::ARAPlaybackRegion* region);
    double endInPlaybackTime() const noexcept;
    bool hasValidPlacement() const noexcept;
};

} // namespace OpenTune
