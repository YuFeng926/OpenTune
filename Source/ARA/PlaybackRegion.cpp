#include "PlaybackRegion.h"

namespace OpenTune {

void PlaybackRegion::updateFrom(juce::ARAPlaybackRegion* region)
{
    playbackRegion = region;
    if (region == nullptr)
        return;

    if (auto* modification = region->getAudioModification())
        audioModificationPersistentId = juce::String(modification->getPersistentID());
    else
        audioModificationPersistentId.clear();

    startInPlaybackTime = region->getStartInPlaybackTime();
    startInModificationTime = region->getStartInAudioModificationTime();
    durationInPlaybackTime = region->getDurationInPlaybackTime();
    durationInModificationTime = region->getDurationInAudioModificationTime();
    timestretchEnabled = region->isTimestretchEnabled();
    timestretchReflectingTempo = region->isTimeStretchReflectingTempo();
    contentBasedFadeAtHead = region->hasContentBasedFadeAtHead();
    contentBasedFadeAtTail = region->hasContentBasedFadeAtTail();
    ++placementRevision;
}

double PlaybackRegion::endInPlaybackTime() const noexcept
{
    return startInPlaybackTime + durationInPlaybackTime;
}

bool PlaybackRegion::hasValidPlacement() const noexcept
{
    return playbackRegion != nullptr
        && audioModificationPersistentId.isNotEmpty()
        && durationInPlaybackTime > 0.0
        && durationInModificationTime > 0.0;
}

} // namespace OpenTune
