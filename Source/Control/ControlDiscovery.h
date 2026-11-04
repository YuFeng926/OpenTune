// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlApiOptions.h"
#include "ControlPathPolicy.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace OpenTune {

/** Build identity published in the discovery file and session results (PROTOCOL.md section 5.3). */
struct ControlBuildIdentity
{
    /** OpenTune version string, 1 to 64 characters. */
    std::string appVersion;
    /** Full commit id (40 lowercase hexadecimal characters) of the source the binary was built from. */
    std::string commit;
    /** Version-control tag of that commit, if the build was made from a tag. */
    std::optional<std::string> commitTag;
    /** Distribution label of this binary, for example the tag of a published test build. */
    std::optional<std::string> buildTag;
    /** Where the corresponding source code can be obtained (https URL). */
    std::string sourceUrl;
};

/** Returns an empty string when the identity can be published as the schema requires, otherwise a
    fixed explanation. */
std::string controlValidateBuildIdentity(const ControlBuildIdentity& identity);

/** Content of one discovery file (schemas/discovery-file.schema.json). */
struct ControlDiscoveryRecord
{
    std::int64_t pid = 0;
    std::int64_t processStartTimeMs = 0;
    std::string instanceId;
    int port = 0;
    std::string token;
    std::string serverProof;
    std::optional<std::string> launchNonce;
    ControlBuildIdentity build;
    std::int64_t startedAtMs = 0;
};

/** The discovery file text: every member of the schema, in schema order, compact JSON. Contains the
    token and the server proof, so it must never be logged. */
std::string controlDiscoveryFileContent(const ControlDiscoveryRecord& record);

/** The ready file text {"pid":..,"instanceId":".."} (no secrets). */
std::string controlReadyFileContent(std::int64_t pid, std::string_view instanceId);

/** Platforms whose default discovery directories differ (PROTOCOL.md section 5.1). */
enum class ControlPlatform {
    Windows,
    MacOS,
    OtherPosix
};

ControlPlatform controlCurrentPlatform();

/**
 * Chooses the discovery directory D (PROTOCOL.md section 5.1): --control-api-dir= when given,
 * otherwise <OPENTUNE_DATA_DIR>/control/instances, otherwise the platform default. Returns an empty
 * string and sets error when no directory can be determined. Options must already be valid.
 * Pure function; thread-safe.
 */
std::string controlResolveDiscoveryDirectory(const ControlApiOptions& options, const ControlEnvironmentLookup& env,
                                             ControlPlatform platform, std::string& error);

/** Process id of this process. */
std::int64_t controlCurrentProcessId();
/** Start time of this process as reported by the operating system, in ms since the Unix epoch; 0
    when unknown. */
std::int64_t controlProcessStartTimeMs();
/** Current wall-clock time in ms since the Unix epoch. */
std::int64_t controlNowMs();

/**
 * Writes content to finalPath through a new temporary file in the same directory and one rename that
 * replaces an existing file. The temporary file is created exclusively and private from the start:
 * on POSIX with O_CREAT|O_EXCL|O_NOFOLLOW and mode 0600, on Windows with CREATE_NEW and an explicit
 * DACL, protected from inheritance, that grants access only to the user of the process token and
 * to SYSTEM. The data is flushed before the rename. Temporary names never end in ".json". On
 * failure no temporary file remains and error says why (without the content).
 */
bool controlWritePrivateFile(const std::string& finalPath, std::string_view content, std::string& error);

/**
 * The discovery file of this process. Thread requirements: one owner thread (the server's
 * start/stop thread).
 */
class ControlDiscovery
{
public:
    explicit ControlDiscovery(std::string directory);
    ~ControlDiscovery();

    ControlDiscovery(const ControlDiscovery&) = delete;
    ControlDiscovery& operator=(const ControlDiscovery&) = delete;

    /** Creates missing directories of D (mode 0700 on POSIX) and, on POSIX, checks that D is a
        directory and not a symbolic link, owned by the effective user and not writable by group or
        others (PROTOCOL.md section 5.2 rule 1). */
    bool prepareDirectory(std::string& error);

    /** Writes <D>/<pid>.json. Call only after the listener accepts connections. */
    bool publish(const ControlDiscoveryRecord& record, std::string& error);

    /** Deletes the file written by publish(), never a file of another process. Idempotent. */
    void withdraw();

    const std::string& directory() const { return directory_; }
    /** <D>/<pid>.json of this process. */
    std::string filePath() const;

private:
    std::string directory_;
    bool published_ = false;
};

} // namespace OpenTune
