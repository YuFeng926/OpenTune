#pragma once

#include <algorithm>

namespace OpenTune {

inline float placementFadeGain(double timeInPlacement,
                               double placementDurationSeconds,
                               double fadeInSeconds,
                               double fadeOutSeconds) noexcept
{
    timeInPlacement = std::clamp(timeInPlacement, 0.0, placementDurationSeconds);
    double gain = 1.0;
    if (fadeInSeconds > 0.0 && timeInPlacement < fadeInSeconds)
        gain *= timeInPlacement / fadeInSeconds;
    if (fadeOutSeconds > 0.0
        && timeInPlacement >= placementDurationSeconds - fadeOutSeconds)
        gain *= (placementDurationSeconds - timeInPlacement) / fadeOutSeconds;
    return static_cast<float>(gain);
}

} // namespace OpenTune
