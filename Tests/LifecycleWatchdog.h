#pragma once

#include "LifecycleTestTrace.h"

#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace OpenTuneTest {

inline constexpr unsigned long lifecycleWatchdogApprovalTimeoutMs = 3000;
inline constexpr unsigned long lifecycleQueueApprovalLimit = 100;

#if defined(_WIN32)

namespace detail {

inline std::wstring wideArg(const char* value)
{
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    const UINT codePage = length != 0 ? CP_UTF8 : CP_ACP;
    const int required = MultiByteToWideChar(codePage, 0, value, -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(codePage, 0, value, -1, result.data(), required);
    result.resize(result.size() - 1);
    return result;
}

inline std::wstring executablePath(const char* argv0)
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return wideArg(argv0);
        if (length < buffer.size() - 1)
            return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}

inline void appendQuoted(std::wstring& commandLine, const std::wstring& argument)
{
    commandLine.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            commandLine.append(backslashes * 2 + 1, L'\\');
            commandLine.push_back(L'"');
            backslashes = 0;
        } else {
            commandLine.append(backslashes, L'\\');
            commandLine.push_back(character);
            backslashes = 0;
        }
    }
    commandLine.append(backslashes * 2, L'\\');
    commandLine.push_back(L'"');
}

inline std::wstring commandLine(int argc, char** argv, const std::wstring& executable)
{
    std::wstring result;
    appendQuoted(result, executable);
    for (int index = 1; index < argc; ++index) {
        result.push_back(L' ');
        appendQuoted(result, wideArg(argv[index]));
    }
    return result;
}

inline void watchdogFailure(const char* testName)
{
    trace(testName, "watchdog_failure");
}

} // namespace detail

template <typename ChildMain>
int run(int argc, char** argv, const char* testName, ChildMain childMain)
{
    const auto* childMarker = _wgetenv(L"OPENTUNE_LIFECYCLE_CHILD");
    if (childMarker != nullptr && std::wstring(childMarker) == L"1")
        return childMain(argc, argv);

    if (!SetEnvironmentVariableW(L"OPENTUNE_LIFECYCLE_CHILD", L"1")) {
        detail::watchdogFailure(testName);
        return 1;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    std::wstring command = detail::commandLine(argc, argv, detail::executablePath(argv[0]));
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
        &startupInfo, &processInfo)) {
        detail::watchdogFailure(testName);
        return 1;
    }

    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, lifecycleWatchdogApprovalTimeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        detail::watchdogFailure(testName);
        if (!TerminateProcess(processInfo.hProcess, 1))
        {
            detail::watchdogFailure(testName);
            WaitForSingleObject(processInfo.hProcess, 0);
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
            return 1;
        }
        WaitForSingleObject(processInfo.hProcess, INFINITE);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return 1;
    }

    if (waitResult == WAIT_FAILED)
    {
        detail::watchdogFailure(testName);
        TerminateProcess(processInfo.hProcess, 1);
        WaitForSingleObject(processInfo.hProcess, INFINITE);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return 1;
    }

    DWORD exitCode = 1;
    if (!GetExitCodeProcess(processInfo.hProcess, &exitCode))
    {
        detail::watchdogFailure(testName);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return 1;
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return static_cast<int>(exitCode);
}

#else

// Other platforms deliberately execute the scenario directly; no watchdog is provided.
template <typename ChildMain>
int run(int argc, char** argv, const char*, ChildMain childMain)
{
    return childMain(argc, argv);
}

#endif

} // namespace OpenTuneTest
