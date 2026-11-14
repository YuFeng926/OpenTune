// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

namespace OpenTune {

/**
 * The directory named by OPENTUNE_DATA_DIR. In Standalone it takes the place of OpenTune's per-user
 * folder for the preferences, logs and the default discovery directory.
 *
 * Returns an empty juce::File when the variable is not set, is empty, or is not an accepted absolute
 * path of this platform (checked with controlCheckAbsolutePath); OpenTune
 * then uses its usual folders. When the Control API is enabled, parseControlApiOptions also reports
 * an invalid non-empty value as a startup error.
 *
 * Used by Standalone's Source/Utils/AppPreferences.cpp and Source/Utils/AppLogger.cpp.
 * Reads the environment on every call; thread-safe as long as no thread changes the environment.
 */
juce::File controlDataDirectory();

} // namespace OpenTune
