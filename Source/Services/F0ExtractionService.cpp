#include "F0ExtractionService.h"
#include "../Utils/AppLogger.h"
#include <juce_events/juce_events.h>
#include <exception>

namespace OpenTune {

F0ExtractionService::F0ExtractionService(int workerCount, size_t maxQueueSize)
    : queue_(maxQueueSize)
    , maxQueueSize_(maxQueueSize)
{
    const int count = (workerCount <= 0) ? 1 : workerCount;
    workers_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        workers_.emplace_back([this]() { workerLoop(); });
    }
}

F0ExtractionService::~F0ExtractionService()
{
    running_.store(false, std::memory_order_release);
    
    for (auto& workerThread : workers_) {
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }
}

uint64_t F0ExtractionService::makeRequestKey(uint64_t materializationId, int trackId, int placementIndex)
{
    if (materializationId != 0) {
        return materializationId;
    }
    return (static_cast<uint64_t>(static_cast<uint32_t>(trackId)) << 32)
        | static_cast<uint32_t>(placementIndex);
}

F0ExtractionService::SubmitResult F0ExtractionService::submit(uint64_t requestKey, ExecuteFn execute, CommitFn commit)
{
    if (requestKey == 0 || !execute || !commit) {
        return SubmitResult::InvalidTask;
    }

    if (!running_.load(std::memory_order_acquire)) {
        return SubmitResult::InvalidTask;
    }

    const uint64_t token = tokenCounter_.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(entriesMutex_);
        if (activeEntries_.find(requestKey) != activeEntries_.end()) {
            return SubmitResult::AlreadyInProgress;
        }
        auto entry = std::make_unique<ActiveEntry>();
        entry->token.store(token, std::memory_order_relaxed);
        activeEntries_[requestKey] = std::move(entry);
    }

    Task task{ requestKey, token, std::move(execute), std::move(commit) };
    if (!queue_.try_enqueue(std::move(task))) {
        std::lock_guard<std::mutex> lock(entriesMutex_);
        activeEntries_.erase(requestKey);
        return SubmitResult::QueueFull;
    }

    return SubmitResult::Accepted;
}

bool F0ExtractionService::isActive(uint64_t requestKey) const
{
    std::lock_guard<std::mutex> lock(entriesMutex_);
    return activeEntries_.find(requestKey) != activeEntries_.end();
}

void F0ExtractionService::cancel(uint64_t requestKey)
{
    std::lock_guard<std::mutex> lock(entriesMutex_);
    activeEntries_.erase(requestKey);
}

void F0ExtractionService::workerLoop()
{
    while (running_.load(std::memory_order_acquire)) {
        Task task;
        if (!queue_.try_dequeue(task)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        uint64_t expectedToken = 0;
        {
            std::lock_guard<std::mutex> lock(entriesMutex_);
            auto it = activeEntries_.find(task.requestKey);
            if (it == activeEntries_.end() || it->second->token.load(std::memory_order_relaxed) != task.token) {
                continue;
            }
            expectedToken = task.token;
        }

        Result result;
        try {
            result = task.execute();
        } catch (const std::exception& e) {
            result.success = false;
            result.errorMessage = e.what();
        } catch (...) {
            AppLogger::error("[F0ExtractionService] Unknown exception during task execution");
            result.success = false;
            result.errorMessage = "execute_exception";
        }

        if (result.requestKey == 0) {
            result.requestKey = task.requestKey;
        }

        bool shouldCommit = false;
        {
            std::lock_guard<std::mutex> lock(entriesMutex_);
            auto it = activeEntries_.find(task.requestKey);
            if (it != activeEntries_.end() && it->second->token.load(std::memory_order_relaxed) == expectedToken) {
                shouldCommit = true;
                activeEntries_.erase(it);
            }
        }

        if (!shouldCommit) {
            continue;
        }

        juce::MessageManager::callAsync([commit = std::move(task.commit), result = std::move(result)]() mutable {
            commit(std::move(result));
        });
    }
}

} // namespace OpenTune
