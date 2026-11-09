// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace OpenTune {

/** Path syntax the rules apply to. Both are selectable on every platform so that tests can cover
    the Windows rules on POSIX hosts and the other way round. */
enum class ControlPathStyle {
    Windows,
    Posix
};

/** The path style of the running platform. */
ControlPathStyle controlNativePathStyle();

/**
 * Checks the raw text of an absolute path given to the control API (PROTOCOL.md section 20.1),
 * before any file-system access. The path is never normalised: it is accepted exactly as given or
 * rejected.
 *
 * Returns an empty string when the path is acceptable, otherwise the reason code of the first rule
 * it breaks, in the order of the specification: path_invalid_character, path_device_namespace,
 * path_unc, path_drive_relative, path_not_absolute, path_alternate_stream, path_not_normalized or
 * path_reserved_name (Windows); path_invalid_character, path_not_absolute or path_not_normalized
 * (POSIX). Text that is not valid UTF-8 counts as path_invalid_character.
 *
 * Pure function; thread-safe.
 */
std::string controlCheckAbsolutePath(std::string_view utf8Path, ControlPathStyle style);

/** Joins an absolute directory and relative segments with the separator of the style. */
std::string controlJoinPath(std::string_view directory, std::string_view relative, ControlPathStyle style);

} // namespace OpenTune
