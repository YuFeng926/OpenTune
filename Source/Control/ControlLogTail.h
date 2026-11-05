// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace OpenTune {

/** Longest log line app.getLog returns, in Unicode code points; a longer line is cut. */
constexpr std::size_t kControlLogLineMaxCodePoints = 4096;
/** The most of the end of the log file that is read for one request, in bytes. */
constexpr std::size_t kControlLogTailWindowBytes = 1024 * 1024;
/** What replaces the token and the server proof in log lines. */
constexpr std::string_view kControlRedactedText = "<redacted>";

/** The lines of a log file's tail, redacted. */
struct ControlLogTail
{
    /** False when the file could not be opened or read; the other members are then empty. */
    bool readable = false;
    /** At most maxLines lines, oldest first, without line terminators. */
    std::vector<std::string> lines;
    /** The file has earlier lines than the ones returned. */
    bool truncated = false;
};

/**
 * The spellings under which a home directory can appear in a log line: as given, with the other
 * separator, and with backslashes doubled (as in escaped text). Longest first; empty and one-character
 * directories (a root) are left out, because replacing them would mangle every path.
 * Pure; any thread.
 */
std::vector<std::string> controlHomeDirectorySpellings(std::string_view homeDirectory);

/**
 * Redacts one log line: every secret (the session token and the server proof) is replaced by
 * "<redacted>", then every home-directory spelling by "~" (PROTOCOL.md sections 4.5 and 20.2). ASCII
 * letters match without regard to case. Bytes that are not valid UTF-8 become '?', and the line is cut
 * to kControlLogLineMaxCodePoints code points after the replacement, so that a cut can never leave the
 * start of a secret behind. Secrets shorter than 8 bytes are ignored (they would match ordinary text).
 * Pure; any thread.
 */
std::string controlRedactLogLine(std::string_view line, const std::vector<std::string>& secrets,
                                 const std::vector<std::string>& homeDirectorySpellings);

/**
 * Reads the last maxLines lines of a log file and redacts them with controlRedactLogLine. At most
 * kControlLogTailWindowBytes from the end of the file are read, so the cost of a request does not depend
 * on the size of the log; truncated is true when the file has earlier lines. The file may be written
 * to while it is read: a line that is still being written can be returned incomplete.
 * Any thread; blocks for the file read.
 */
ControlLogTail controlReadLogTail(const std::filesystem::path& file, std::size_t maxLines,
                                  const std::vector<std::string>& secrets,
                                  const std::vector<std::string>& homeDirectorySpellings);

} // namespace OpenTune
