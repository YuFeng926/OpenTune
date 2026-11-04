// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace OpenTune {

/**
 * The identity of an existing file or directory (PROTOCOL.md section 20.3 rule 1): on Windows the volume
 * serial number and the file id of a handle opened on the path, on POSIX systems st_dev and st_ino. Two
 * existing files are the same file exactly when their identities are equal (rule 2), whatever the strings
 * that name them (letter case, short names, links, junctions, mount points, hard links).
 */
struct ControlFileId
{
    std::uint64_t volume = 0;
    std::uint64_t idHigh = 0;
    std::uint64_t idLow = 0;

    bool operator==(const ControlFileId& other) const noexcept
    {
        return volume == other.volume && idHigh == other.idHigh && idLow == other.idLow;
    }
    bool operator!=(const ControlFileId& other) const noexcept { return !(*this == other); }
};

/** How the identity of a path was determined. */
enum class ControlFileIdStatus {
    /** Something exists at the path; its identity is set. */
    Found,
    /** Nothing exists at the path, or a directory on the way does not exist. */
    Missing,
    /** The volume that holds the path is not available (a removed drive or card, a disconnected network
        share, a drive letter that is not assigned). Nothing on that volume can be reached, so a file there
        is not the same file as one on an available volume. */
    Unavailable,
    /** The identity cannot be determined for another reason (for example a directory that cannot be
        opened); section 20.3 rule 4. */
    Failed
};

/**
 * Sorts an operating system error code of a failed attempt to open or stat a path: Missing for "no such
 * file or directory", Unavailable for "the volume is not there" (Windows ERROR_NOT_READY, ERROR_BAD_NETPATH,
 * ERROR_BAD_NET_NAME, ERROR_INVALID_DRIVE and similar; POSIX ENODEV, ENXIO, ESTALE and similar), Failed for
 * everything else. The code is GetLastError() on Windows and errno elsewhere.
 *
 * Any thread.
 */
ControlFileIdStatus controlClassifyOsError(unsigned long code) noexcept;

/**
 * The identity of the file or directory at an absolute path (UTF-8, already checked with
 * controlCheckAbsolutePath), following symbolic links. Opens the path only to read its identity.
 *
 * Any thread.
 */
ControlFileIdStatus controlFileIdentity(const std::string& utf8Path, ControlFileId& out);

/**
 * Whether two absolute paths name the same file (PROTOCOL.md section 20.3 rules 2 and 3): two existing
 * files by their identities; two paths at which nothing exists when their parent directories exist with
 * equal identities and their last segments are equal as the file system compares names (on Windows without
 * regard to case); an existing file and a path without a file never. nullopt when an identity that the
 * decision needs cannot be determined (rule 4): the caller refuses with IO_ERROR(read_failed).
 *
 * Any thread.
 */
std::optional<bool> controlSameFile(const std::string& utf8PathA, const std::string& utf8PathB);

/**
 * Compares one target path with many other paths, reading the identity of the target (and of its parent
 * directory) at most once. The decisions are those of controlSameFile.
 *
 * Any thread; an object is used by one thread.
 */
class ControlSameFileProbe
{
public:
    /** The identity of the target is read here, once. */
    explicit ControlSameFileProbe(std::string utf8Target);

    /**
     * Whether `utf8Other` names the same file as the target. nullopt when an identity that the decision
     * needs cannot be determined (rule 4), and always when the identity of the target cannot. A path whose
     * volume is not available (ControlFileIdStatus::Unavailable) is such a case too: the same share can be
     * reachable through another name, so it is never taken as a different file.
     */
    std::optional<bool> sameAs(const std::string& utf8Other);

private:
    std::string target_;
    ControlFileId targetId_;
    ControlFileIdStatus targetStatus_;
    bool parentRead_ = false;
    ControlFileId parentId_;
    ControlFileIdStatus parentStatus_ = ControlFileIdStatus::Failed;
};

/** How a final rename ended. */
enum class ControlRenameResult {
    Done,
    /** The rename is done and the target is in place, but the source path (a second hard link to the
        target's data, POSIX only) could not be removed and is still there; the caller deletes it again. */
    DoneSourceRemains,
    /** A rename that never replaces found the target; the target is untouched. */
    TargetExists,
    Failed
};

/**
 * Moves a file to a path in the same directory with one rename (PROTOCOL.md section 20.4). With replace,
 * a rename that replaces an existing target (Windows MoveFileExW with MOVEFILE_REPLACE_EXISTING, POSIX
 * rename). Without it, a rename that fails atomically when the target exists, never a test followed by a
 * rename (Windows MoveFileExW without MOVEFILE_REPLACE_EXISTING; POSIX link to the target, then unlink of
 * the source, which the specification allows on every POSIX system); on a file system that offers no such
 * rename it fails, and the source stays where it is.
 *
 * Any thread.
 */
ControlRenameResult controlRenameFile(const std::string& utf8From, const std::string& utf8To, bool replace);

/**
 * The result of a rename that never replaces, after its link to the target has been made, from the return
 * value and errno of the unlink of the source: Done when the source is gone (or was already gone),
 * DoneSourceRemains otherwise. The target is in place in both cases.
 *
 * Any thread.
 */
ControlRenameResult controlRenameAfterLink(int unlinkReturn, int unlinkErrno) noexcept;

} // namespace OpenTune
