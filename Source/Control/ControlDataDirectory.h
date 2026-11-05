// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

namespace OpenTune {

/**
 * The directory named by the environment variable OPENTUNE_DATA_DIR. In a build with the control API
 * it takes the place of OpenTune's per-user folder for the preferences, the logs and the default
 * discovery directory (PROTOCOL.md section 6.3), whether or not the API is enabled at run time, so
 * that tests never touch the user's real settings.
 *
 * Returns an empty juce::File when the variable is not set, is empty, or is not an accepted absolute
 * path of this platform (PROTOCOL.md section 20.1, checked with controlCheckAbsolutePath); OpenTune
 * then uses its usual folders, exactly as without the control API. (With such a value the control API
 * itself refuses to start, see parseControlApiOptions.)
 *
 * Used by the data-directory hooks in Source/Utils/AppPreferences.cpp and Source/Utils/AppLogger.cpp.
 * Reads the environment on every call; thread-safe as long as no thread changes the environment.
 */
juce::File controlDataDirectory();

} // namespace OpenTune
