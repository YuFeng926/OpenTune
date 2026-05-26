#pragma once

#include <cmath>
#include "AppPreferences.h"

namespace OpenTune::SnapUtils {

namespace detail {

inline double getGridSize(SnapSettings::Mode mode, double bpm) {
    if (bpm <= 0.0) return 0.0;
    switch (mode) {
        case SnapSettings::Mode::Beat:  return (60.0 / bpm) / 4.0;
        case SnapSettings::Mode::Bar:   return (60.0 / bpm) * 4.0;
        case SnapSettings::Mode::Second: return 1.0;
        default: return 0.0;
    }
}

} // namespace detail

/// Snap a time value (seconds) to the nearest grid position.
/// Returns the original value when snap is disabled or mode is Off.
inline double snapTime(double seconds, double bpm, const SnapSettings& settings) {
    if (!settings.isActive() || seconds < 0.0) {
        return seconds;
    }

    double gridSize = detail::getGridSize(settings.mode, bpm);
    if (gridSize <= 0.0) return seconds;
    return std::round(seconds / gridSize) * gridSize;
}

/// Snap a duration delta (seconds) to the grid — ensures minimum increment.
inline double snapDelta(double rawDelta, double bpm, const SnapSettings& settings) {
    if (!settings.isActive() || rawDelta == 0.0) {
        return rawDelta;
    }

    double gridSize = detail::getGridSize(settings.mode, bpm);
    if (gridSize <= 0.0) return rawDelta;
    return std::round(rawDelta / gridSize) * gridSize;
}

} // namespace OpenTune::SnapUtils
