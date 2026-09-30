#pragma once

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <initializer_list>
#include <utility>
#include <mutex>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace OpenTuneTest {

inline void trace(const char* testName,
                  const char* event,
                  std::initializer_list<std::pair<const char*, std::string>> fields = {})
{
    static std::atomic<uint64_t> sequence{0};
    static std::atomic<bool> firstEvent{true};
    static std::mutex fileMutex;
    const auto root = std::filesystem::path(std::getenv("OPENTUNE_LIFECYCLE_TRACE_DIR")
        ? std::getenv("OPENTUNE_LIFECYCLE_TRACE_DIR")
        : "lifecycle-traces");
    std::error_code error;
    std::filesystem::create_directories(root, error);
    const auto pid =
#if defined(_WIN32)
        static_cast<unsigned long>(GetCurrentProcessId());
#else
        static_cast<unsigned long>(getpid());
#endif
    std::ostringstream threadId;
    threadId << std::this_thread::get_id();
    const auto path = root / (std::string(testName) + "-" + std::to_string(pid) + ".jsonl");
    std::lock_guard<std::mutex> lock(fileMutex);
    const auto first = firstEvent.exchange(false, std::memory_order_acq_rel);
    std::ofstream output(path, first ? std::ios::trunc : std::ios::app);
    std::ostringstream line;
    line << "{\"seq\":" << sequence.fetch_add(1, std::memory_order_relaxed)
           << ",\"pid\":" << pid
           << ",\"thread\":\"" << threadId.str()
           << "\",\"event\":\"" << event << "\"";
    for (const auto& [name, value] : fields)
        line << ",\"" << name << "\":" << value;
    line << "}\n";
    output << line.str();
}

inline std::string jsonString(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const auto character : value)
    {
        if (character == '\\' || character == '"')
            escaped.push_back('\\');
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

inline std::string jsonNumber(long long value)
{
    return std::to_string(value);
}

} // namespace OpenTuneTest
