#include "PlaybackRegion.h"

namespace OpenTune {

bool PlaybackRegion::hasValidPlacement() const noexcept
{
    if (playbackRegion == nullptr)
        return false;

    auto* modification = playbackRegion->getAudioModification();
    if (modification == nullptr || modification->getPersistentID().empty())
        return false;

    return playbackRegion->getDurationInPlaybackTime() > 0.0
        && playbackRegion->getDurationInAudioModificationTime() > 0.0;
}

} // namespace OpenTune
