// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlDiscovery.h"

#include "ControlJson.h"
#include "SecureRandom.h"

#include <chrono>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#include <fstream>
#include <iterator>
#include <sstream>
#endif
#endif

namespace OpenTune {

namespace {

constexpr std::size_t kMaxDiscoveryFileBytes = 8192;

bool isLowercaseHex(std::string_view text)
{
    for (const char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

bool fitsCodePoints(std::string_view text, std::size_t minimum, std::size_t maximum)
{
    if (!isValidUtf8(text))
        return false;
    const std::size_t count = utf8CodePointCount(text);
    return count >= minimum && count <= maximum;
}

JsonValue optionalString(const std::optional<std::string>& value)
{
    return value ? JsonValue::string(*value) : JsonValue();
}

char nativeSeparator()
{
    return controlNativePathStyle() == ControlPathStyle::Windows ? '\\' : '/';
}

// Strips trailing separators from a directory taken from the environment.
std::string trimTrailingSeparators(std::string value, char separator)
{
    while (value.size() > 1 && value.back() == separator)
        value.pop_back();
    return value;
}

bool isUsableEnvironmentDirectory(const std::optional<std::string>& value, ControlPlatform platform)
{
    if (!value || value->empty() || !isValidUtf8(*value))
        return false;
    if (platform == ControlPlatform::Windows)
        return value->size() >= 3 && (*value)[1] == ':' && (*value)[2] == '\\';
    return (*value)[0] == '/';
}

#if defined(_WIN32)
bool toWide(const std::string& utf8, std::wstring& wide)
{
    if (utf8.empty()) {
        wide.clear();
        return true;
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0)
        return false;
    wide.assign(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return true;
}

std::string windowsError(const char* what)
{
    return std::string(what) + " (error " + std::to_string(GetLastError()) + ")";
}

// SID of the user in this process's access token, as an SDDL string such as S-1-5-21-...
bool currentUserSid(std::wstring& sid)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    const BOOL ok = size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!ok)
        return false;
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text))
        return false;
    sid = text;
    LocalFree(text);
    return true;
}
#endif

std::string temporaryPathFor(const std::string& finalPath, std::string& error)
{
    const char separator = nativeSeparator();
    const std::size_t slash = finalPath.find_last_of(separator);
    if (slash == std::string::npos) {
        error = "the target path has no directory";
        return {};
    }
    const auto suffix = SecureRandom::hex(8);
    if (!suffix) {
        error = "the system random number generator failed";
        return {};
    }
    // ".<name>.<random>.tmp": never ends in ".json", so clients never mistake it for a discovery file.
    return finalPath.substr(0, slash + 1) + "." + finalPath.substr(slash + 1) + "." + *suffix + ".tmp";
}

} // namespace

std::string controlValidateBuildIdentity(const ControlBuildIdentity& identity)
{
    if (!fitsCodePoints(identity.appVersion, 1, 64))
        return "appVersion must be 1 to 64 characters";
    if (identity.commit.size() != 40 || !isLowercaseHex(identity.commit))
        return "the build commit must be 40 lowercase hexadecimal characters";
    if (identity.commitTag && !fitsCodePoints(*identity.commitTag, 0, 200))
        return "the build commit tag must be at most 200 characters";
    if (identity.buildTag && !fitsCodePoints(*identity.buildTag, 0, 200))
        return "buildTag must be at most 200 characters";
    if (identity.sourceUrl.compare(0, 8, "https://") != 0 || !isValidUtf8(identity.sourceUrl))
        return "sourceUrl must be an https URL";
    return {};
}

std::string controlDiscoveryFileContent(const ControlDiscoveryRecord& record)
{
    auto build = JsonValue::object();
    build.appendMember("commit", JsonValue::string(record.build.commit));
    build.appendMember("tag", optionalString(record.build.commitTag));

    auto majors = JsonValue::array();
    majors.append(JsonValue::integer(1));

    auto json = JsonValue::object();
    json.appendMember("protocolMajors", std::move(majors));
    json.appendMember("pid", JsonValue::integer(record.pid));
    json.appendMember("processStartTimeMs", JsonValue::integer(record.processStartTimeMs));
    json.appendMember("instanceId", JsonValue::string(record.instanceId));
    json.appendMember("port", JsonValue::integer(record.port));
    json.appendMember("token", JsonValue::string(record.token));
    json.appendMember("serverProof", JsonValue::string(record.serverProof));
    json.appendMember("launchNonce", optionalString(record.launchNonce));
    json.appendMember("appVersion", JsonValue::string(record.build.appVersion));
    json.appendMember("build", std::move(build));
    json.appendMember("buildTag", optionalString(record.build.buildTag));
    json.appendMember("sourceUrl", JsonValue::string(record.build.sourceUrl));
    json.appendMember("startedAtMs", JsonValue::integer(record.startedAtMs));
    json.appendMember("flavor", JsonValue::string("standalone"));
    return toJsonString(json);
}

std::string controlReadyFileContent(std::int64_t pid, std::string_view instanceId)
{
    auto json = JsonValue::object();
    json.appendMember("pid", JsonValue::integer(pid));
    json.appendMember("instanceId", JsonValue::string(std::string(instanceId)));
    return toJsonString(json);
}

ControlPlatform controlCurrentPlatform()
{
#if defined(_WIN32)
    return ControlPlatform::Windows;
#elif defined(__APPLE__)
    return ControlPlatform::MacOS;
#else
    return ControlPlatform::OtherPosix;
#endif
}

std::string controlResolveDiscoveryDirectory(const ControlApiOptions& options, const ControlEnvironmentLookup& env,
                                             ControlPlatform platform, std::string& error)
{
    const auto style = platform == ControlPlatform::Windows ? ControlPathStyle::Windows : ControlPathStyle::Posix;
    const char separator = style == ControlPathStyle::Windows ? '\\' : '/';
    if (options.discoveryDirectory)
        return *options.discoveryDirectory;
    if (options.dataDirectory)
        return controlJoinPath(*options.dataDirectory, "control/instances", style);

    const auto lookup = [&env](const char* name) { return env ? env(name) : std::nullopt; };
    switch (platform) {
        case ControlPlatform::Windows: {
            const auto localAppData = lookup("LOCALAPPDATA");
            if (!isUsableEnvironmentDirectory(localAppData, platform)) {
                error = "LOCALAPPDATA is not set to an absolute path";
                return {};
            }
            return controlJoinPath(trimTrailingSeparators(*localAppData, separator), "OpenTune/control/instances",
                                   style);
        }
        case ControlPlatform::MacOS: {
            const auto home = lookup("HOME");
            if (!isUsableEnvironmentDirectory(home, platform)) {
                error = "HOME is not set to an absolute path";
                return {};
            }
            return controlJoinPath(trimTrailingSeparators(*home, separator),
                                   "Library/Application Support/OpenTune/control/instances", style);
        }
        case ControlPlatform::OtherPosix: {
            const auto stateHome = lookup("XDG_STATE_HOME");
            if (isUsableEnvironmentDirectory(stateHome, platform))
                return controlJoinPath(trimTrailingSeparators(*stateHome, separator), "OpenTune/control/instances",
                                       style);
            const auto home = lookup("HOME");
            if (!isUsableEnvironmentDirectory(home, platform)) {
                error = "neither XDG_STATE_HOME nor HOME is set to an absolute path";
                return {};
            }
            return controlJoinPath(trimTrailingSeparators(*home, separator), ".local/state/OpenTune/control/instances",
                                   style);
        }
    }
    error = "unknown platform";
    return {};
}

std::int64_t controlCurrentProcessId()
{
#if defined(_WIN32)
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

std::int64_t controlProcessStartTimeMs()
{
#if defined(_WIN32)
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime))
        return 0;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = creation.dwLowDateTime;
    ticks.HighPart = creation.dwHighDateTime;
    // FILETIME counts 100 ns intervals since 1601-01-01; the Unix epoch is 11644473600 s later.
    constexpr unsigned long long kEpochOffset = 116444736000000000ULL;
    if (ticks.QuadPart < kEpochOffset)
        return 0;
    return static_cast<std::int64_t>((ticks.QuadPart - kEpochOffset) / 10000ULL);
#elif defined(__APPLE__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(getpid())};
    struct kinfo_proc info
    {};
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
        return 0;
    const auto& started = info.kp_proc.p_starttime;
    return static_cast<std::int64_t>(started.tv_sec) * 1000 + static_cast<std::int64_t>(started.tv_usec) / 1000;
#elif defined(__linux__)
    // Field 22 of /proc/self/stat is the start time in clock ticks after boot; /proc/stat has the
    // boot time ("btime") in seconds since the epoch.
    std::ifstream statFile("/proc/self/stat");
    std::string stat((std::istreambuf_iterator<char>(statFile)), std::istreambuf_iterator<char>());
    const std::size_t close = stat.rfind(')');
    if (close == std::string::npos)
        return 0;
    std::istringstream fields(stat.substr(close + 2));
    std::string field;
    unsigned long long startTicks = 0;
    for (int index = 3; index <= 22 && fields >> field; ++index)
        if (index == 22)
            startTicks = std::stoull(field);
    std::ifstream bootFile("/proc/stat");
    std::string line;
    long long bootSeconds = -1;
    while (std::getline(bootFile, line))
        if (line.compare(0, 6, "btime ") == 0)
            bootSeconds = std::stoll(line.substr(6));
    const long ticksPerSecond = sysconf(_SC_CLK_TCK);
    if (bootSeconds < 0 || ticksPerSecond <= 0)
        return 0;
    return static_cast<std::int64_t>(bootSeconds) * 1000
           + static_cast<std::int64_t>(startTicks * 1000ULL / static_cast<unsigned long long>(ticksPerSecond));
#else
    return 0;
#endif
}

std::int64_t controlNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool controlWritePrivateFile(const std::string& finalPath, std::string_view content, std::string& error)
{
    const std::string temporaryPath = temporaryPathFor(finalPath, error);
    if (temporaryPath.empty())
        return false;
#if defined(_WIN32)
    std::wstring wideFinal;
    std::wstring wideTemporary;
    if (!toWide(finalPath, wideFinal) || !toWide(temporaryPath, wideTemporary)) {
        error = "the path is not valid UTF-8";
        return false;
    }
    std::wstring userSid;
    if (!currentUserSid(userSid)) {
        error = windowsError("cannot read the user of the process token");
        return false;
    }
    // Protected DACL ("P": no inherited entries): full access for the process user and for SYSTEM only.
    const std::wstring sddl = L"D:P(A;;FA;;;" + userSid + L")(A;;FA;;;SY)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        error = windowsError("cannot build the security descriptor");
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    // The descriptor is applied when the file is created, so the file never exists with other rights.
    const HANDLE file =
        CreateFileW(wideTemporary.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE) {
        error = windowsError("cannot create the temporary file");
        return false;
    }
    bool ok = true;
    std::size_t written = 0;
    while (ok && written < content.size()) {
        DWORD chunk = 0;
        const auto request = static_cast<DWORD>(content.size() - written);
        ok = WriteFile(file, content.data() + written, request, &chunk, nullptr) != 0 && chunk > 0;
        written += chunk;
    }
    if (ok)
        ok = FlushFileBuffers(file) != 0;
    if (!ok)
        error = windowsError("cannot write the temporary file");
    CloseHandle(file);
    if (ok
        && !MoveFileExW(wideTemporary.c_str(), wideFinal.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = windowsError("cannot rename the temporary file");
        ok = false;
    }
    if (!ok)
        DeleteFileW(wideTemporary.c_str());
    return ok;
#else
    const int file = ::open(temporaryPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0) {
        error = "cannot create the temporary file (errno " + std::to_string(errno) + ")";
        return false;
    }
    bool ok = ::fchmod(file, 0600) == 0;
    std::size_t written = 0;
    while (ok && written < content.size()) {
        const ssize_t chunk = ::write(file, content.data() + written, content.size() - written);
        if (chunk < 0 && errno == EINTR)
            continue;
        ok = chunk > 0;
        if (ok)
            written += static_cast<std::size_t>(chunk);
    }
    if (ok)
        ok = ::fsync(file) == 0;
    if (!ok)
        error = "cannot write the temporary file (errno " + std::to_string(errno) + ")";
    ::close(file);
    if (ok && ::rename(temporaryPath.c_str(), finalPath.c_str()) != 0) {
        error = "cannot rename the temporary file (errno " + std::to_string(errno) + ")";
        ok = false;
    }
    if (!ok)
        ::unlink(temporaryPath.c_str());
    return ok;
#endif
}

ControlDiscovery::ControlDiscovery(std::string directory) : directory_(std::move(directory)) {}

ControlDiscovery::~ControlDiscovery()
{
    withdraw();
}

bool ControlDiscovery::prepareDirectory(std::string& error)
{
    const char separator = nativeSeparator();
    if (directory_.empty()) {
        error = "the discovery directory is empty";
        return false;
    }
    // Create every missing component; existing components are left as they are.
#if defined(_WIN32)
    const std::size_t firstComponent = 3; // after "X:\"
#else
    const std::size_t firstComponent = 1; // after "/"
#endif
    for (std::size_t pos = directory_.find(separator, firstComponent);; pos = directory_.find(separator, pos + 1)) {
        const std::string prefix = pos == std::string::npos ? directory_ : directory_.substr(0, pos);
#if defined(_WIN32)
        std::wstring wide;
        if (!toWide(prefix, wide)) {
            error = "the discovery directory is not valid UTF-8";
            return false;
        }
        const DWORD existing = GetFileAttributesW(wide.c_str());
        const bool isDirectory = existing != INVALID_FILE_ATTRIBUTES && (existing & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!isDirectory && !CreateDirectoryW(wide.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            error = windowsError("cannot create the discovery directory");
            return false;
        }
#else
        if (::mkdir(prefix.c_str(), 0700) != 0 && errno != EEXIST) {
            error = "cannot create the discovery directory (errno " + std::to_string(errno) + ")";
            return false;
        }
#endif
        if (pos == std::string::npos)
            break;
    }
#if defined(_WIN32)
    std::wstring wideDirectory;
    toWide(directory_, wideDirectory);
    const DWORD attributes = GetFileAttributesW(wideDirectory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        error = "the discovery directory is not a directory";
        return false;
    }
#else
    struct stat info
    {};
    if (::lstat(directory_.c_str(), &info) != 0) {
        error = "cannot inspect the discovery directory (errno " + std::to_string(errno) + ")";
        return false;
    }
    if (S_ISLNK(info.st_mode) || !S_ISDIR(info.st_mode)) {
        error = "the discovery directory is a symbolic link or not a directory";
        return false;
    }
    if (info.st_uid != ::geteuid()) {
        error = "the discovery directory is not owned by the effective user";
        return false;
    }
    if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        error = "the discovery directory is writable by group or others";
        return false;
    }
#endif
    return true;
}

bool ControlDiscovery::publish(const ControlDiscoveryRecord& record, std::string& error)
{
    const std::string content = controlDiscoveryFileContent(record);
    if (content.size() > kMaxDiscoveryFileBytes) {
        error = "the discovery file would exceed 8192 bytes";
        return false;
    }
    if (!controlWritePrivateFile(filePath(), content, error))
        return false;
    published_ = true;
    return true;
}

void ControlDiscovery::withdraw()
{
    if (!published_)
        return;
    published_ = false;
    const std::string path = filePath();
#if defined(_WIN32)
    std::wstring wide;
    if (toWide(path, wide))
        DeleteFileW(wide.c_str());
#else
    ::unlink(path.c_str());
#endif
}

std::string ControlDiscovery::filePath() const
{
    return controlJoinPath(directory_, std::to_string(controlCurrentProcessId()) + ".json", controlNativePathStyle());
}

} // namespace OpenTune
