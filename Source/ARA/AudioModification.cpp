#include "AudioModification.h"

namespace OpenTune {

void AudioModification::updateIdentity(juce::ARAAudioModification* modification)
{
    audioModification = modification;
    persistentId = modification != nullptr
        ? juce::String(modification->getPersistentID())
        : juce::String();
}

void AudioModification::attachSource(const AudioSource& source)
{
    sourcePersistentId = source.getIdentity().persistentId;
    const auto& shape = source.getShape();
    contentWindow = SourceWindow{sourceId, 0.0, shape.durationSeconds()};
}

void AudioModification::resetContent() noexcept
{
    materializationId = 0;
    materializationRevision = 0;
    materializationDurationSeconds = 0.0;
    contentWindow = {};
    ++contentRevision;
    birthState = AudioModificationBirthState::Empty;
}

bool AudioModification::isRenderable() const noexcept
{
    return materializationId != 0
        && materializationDurationSeconds > 0.0
        && birthState == AudioModificationBirthState::Ready;
}

} // namespace OpenTune
