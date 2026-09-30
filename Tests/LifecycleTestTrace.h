#pragma once

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace OpenTuneTest {

inline void trace(const char* testName, const char* event)
{
    static std::atomic<uint64_t> sequence{0};
    static std::mutex fileMutex;
    const auto root = std::filesystem::path(std::getenv("OPENTUNE_LIFECYCLE_TRACE_DIR")
        ? std::getenv("OPENTUNE_LIFECYCLE_TRACE_DIR")
        : "lifecycle-traces");
    std::error_code error;
    std::filesystem::create_directories(root, error);
    std::ostringstream threadId;
    threadId << std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(fileMutex);
    std::ofstream output(root / (std::string(testName) + ".jsonl"), std::ios::app);
    output << "{\"seq\":" << sequence.fetch_add(1, std::memory_order_relaxed)
           << ",\"thread\":\"" << threadId.str()
           << "\",\"event\":\"" << event << "\"}\n";
}

} // namespace OpenTuneTest
