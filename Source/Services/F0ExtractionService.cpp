#include "F0ExtractionService.h"
#include "../Inference/F0InferenceService.h"
#include "../Utils/AppLogger.h"
#include <juce_events/juce_events.h>
#include <exception>

namespace OpenTune {

F0ExtractionService::F0ExtractionService(int workerCount, size_t maxQueueSize,
                                         std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver)
    : maxQueueSize_(maxQueueSize)
    , runOwnerState_(std::make_shared<F0RunOwnerState>())
    , f0ServiceResolver_(std::move(f0ServiceResolver))
{
    const int count = (workerCount <= 0) ? 1 : workerCount;
    workers_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        workers_.emplace_back([this]() { workerLoop(); });
    }
}

F0ExtractionService::~F0ExtractionService()
{
    shutdown();
}

void F0ExtractionService::shutdown()
{
    {
        // 幂等：单一临界区内置 shutdownStarted_、关闭 owner state、丢弃排队任务、
        // 清空 active 表；与 submit/commit/worker 状态读取线性化，无"检查后关闭"窗口。
        std::lock_guard<std::mutex> lock(entriesMutex_);
        if (shutdownStarted_)
            return;
        shutdownStarted_ = true;

        runOwnerState_->closed.store(true, std::memory_order_release);

        queue_.clear();
        activeEntries_.clear();
    }

    // 唤醒全部 worker：谓词 shutdownStarted_ 为真，立即退出等待。
    queueCv_.notify_all();

    // 锁外：终止属于本 owner 的 pending/active Run（若正在 DML 推理），
    // 使已 dequeue 的 execute 快速返回。
    if (f0ServiceResolver_) {
        if (auto svc = f0ServiceResolver_())
            svc->terminateActiveRun(runOwnerState_);
    }

    // join 所有 worker：Run 已终止 → execute 快速返回 → worker 退出。不悬挂线程。
    for (auto& workerThread : workers_) {
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }
}

F0ExtractionService::SubmitResult F0ExtractionService::submit(F0RequestKey requestKey, ExecuteFn execute, CommitFn commit)
{
    if (!requestKey.contentKey.isValid() || !execute || !commit) {
        return SubmitResult::InvalidTask;
    }

    // 单一临界区：检查 shutdown、查重、容量检查、分配 token、插入 active、enqueue。
    // 队列满直接 QueueFull，不创建 active 条目。与 shutdown 线性化。
    {
        std::lock_guard<std::mutex> lock(entriesMutex_);
        if (shutdownStarted_) {
            return SubmitResult::InvalidTask;
        }

        if (activeEntries_.find(requestKey) != activeEntries_.end()) {
            return SubmitResult::AlreadyInProgress;
        }

        if (queue_.size() >= maxQueueSize_) {
            return SubmitResult::QueueFull;
        }

        const uint64_t token = tokenCounter_++;
        auto entry = std::make_unique<ActiveEntry>();
        entry->token = token;
        activeEntries_[requestKey] = std::move(entry);

        queue_.push_back(Task{ requestKey, token, std::move(execute), std::move(commit) });
    }

    // 解锁后唤醒单个 worker。
    queueCv_.notify_one();
    return SubmitResult::Accepted;
}

bool F0ExtractionService::isActive(F0RequestKey requestKey) const
{
    std::lock_guard<std::mutex> lock(entriesMutex_);
    return activeEntries_.find(requestKey) != activeEntries_.end();
}

void F0ExtractionService::cancel(F0RequestKey requestKey)
{
    std::lock_guard<std::mutex> lock(entriesMutex_);
    activeEntries_.erase(requestKey);
}

void F0ExtractionService::workerLoop()
{
    while (true) {
        // 取任务阶段：临界区内 wait、pop、token 校验，离开作用域自动解锁。
        Task task;
        {
            std::unique_lock<std::mutex> lock(entriesMutex_);
            // 阻塞等待：队列非空或 shutdown。shutdown 唤醒后直接退出。
            queueCv_.wait(lock, [this]() { return shutdownStarted_ || !queue_.empty(); });
            if (shutdownStarted_)
                return;

            // pop 与 active token 校验在同一临界区：deque 状态与 active 表视图一致。
            task = std::move(queue_.front());
            queue_.pop_front();
            auto it = activeEntries_.find(task.requestKey);
            if (it == activeEntries_.end() || it->second->token != task.token) {
                continue; // 作用域退出自动解锁。
            }
        }

        // 锁外 execute。
        Result result;
        try {
            result = task.execute(runOwnerState_);
        } catch (const std::exception& e) {
            result.success = false;
            result.errorMessage = e.what();
        } catch (...) {
            AppLogger::error("[F0ExtractionService] Unknown exception during task execution");
            result.success = false;
            result.errorMessage = "execute_exception";
        }

        if (!result.requestKey.contentKey.isValid()) {
            result.requestKey = task.requestKey;
        }

        // commit 判定阶段：独立临界区检查 shutdown/token 并 erase，离开作用域自动解锁。
        bool shouldCommit = false;
        {
            std::lock_guard<std::mutex> lock(entriesMutex_);
            // 关闭已启动：不再投递 commit（owner 可能已析构；active 表已被 shutdown 清空）。
            if (!shutdownStarted_) {
                auto it2 = activeEntries_.find(task.requestKey);
                if (it2 != activeEntries_.end() && it2->second->token == task.token) {
                    shouldCommit = true;
                    activeEntries_.erase(it2);
                }
            }
        }

        if (!shouldCommit) {
            continue;
        }

        // 锁外投递 commit。
        juce::MessageManager::callAsync([commit = std::move(task.commit), result = std::move(result)]() mutable {
            commit(std::move(result));
        });
    }
}

} // namespace OpenTune
