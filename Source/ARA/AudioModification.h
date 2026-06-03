#pragma once

#include "AudioSource.h"
#include "../Utils/SourceWindow.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>

namespace OpenTune {

enum class AudioModificationBirthState
{
    Empty,
    WaitingForSource,
    PendingBirth,
    Rendering,
    Ready,
    Failed
};

struct AudioModification
{
    juce::ARAAudioModification* audioModification{nullptr};
    juce::String persistentId;
    juce::String sourcePersistentId;
    SourceWindow contentWindow;
    uint64_t sourceId{0};
    uint64_t materializationId{0};
    uint64_t materializationRevision{0};
    uint64_t contentRevision{0};
    uint64_t birthRevision{0};
    double materializationDurationSeconds{0.0};
    AudioModificationBirthState birthState{AudioModificationBirthState::Empty};

    void updateIdentity(juce::ARAAudioModification* modification);
    void attachSource(const AudioSource& source);
    void resetContent() noexcept;
    bool isRenderable() const noexcept;
};

} // namespace OpenTune
