#include "F0ExtractionService.h"
#include "../Inference/F0InferenceService.h"
#include "../Utils/AppLogger.h"
#include <juce_events/juce_events.h>
#include <exception>

namespace OpenTune {

F0ExtractionService::F0ExtractionService(int workerCount, size_t maxQueueSize,
                                         std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver)
    : state_(std::make_shared<SharedState>())
    , f0ServiceResolver_(std::move(f0ServiceResolver))
{
    state_->maxQueueSize_ = maxQueueSize;
    state_->runOwnerState_ = std::make_shared<F0RunOwnerState>();

    const int count = (workerCount <= 0) ? 1 : workerCount;
    for (int i = 0; i < count; ++i) {
        // detached 进程常驻执行器：捕获 shared state 而非 service this。
        // owner 析构后 worker 见 shutdownStarted_ 自行退出；共享 state 由
        // worker 自身持有，保证其生命周期覆盖最后一次使用。
        std::thread worker([state = state_]() { workerLoop(state); });
        worker.detach();
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
        std::lock_guard<std::mutex> lock(state_->entriesMutex_);
        if (state_->shutdownStarted_)
            return;
        state_->shutdownStarted_ = true;

        state_->runOwnerState_->closed.store(true, std::memory_order_release);

        state_->queue_.clear();
        state_->activeEntries_.clear();
    }

    // 唤醒全部 worker：谓词 shutdownStarted_ 为真，立即退出等待。
    state_->queueCv_.notify_all();

    // 锁外：终止属于本 owner 的 pending/active Run（若正在 DML 推理），
    // 使已 dequeue 的 execute 快速返回。不 join worker —— worker 见
    // shutdownStarted_ 自行退出；推理归属进程寿命的 F0InferenceService。
    if (f0ServiceResolver_) {
        if (auto svc = f0ServiceResolver_())
            svc->terminateActiveRun(state_->runOwnerState_);
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
        std::lock_guard<std::mutex> lock(state_->entriesMutex_);
        if (state_->shutdownStarted_) {
            return SubmitResult::InvalidTask;
        }

        if (state_->activeEntries_.find(requestKey) != state_->activeEntries_.end()) {
            return SubmitResult::AlreadyInProgress;
        }

        if (state_->queue_.size() >= state_->maxQueueSize_) {
            return SubmitResult::QueueFull;
        }

        const uint64_t token = state_->tokenCounter_++;
        auto entry = std::make_unique<ActiveEntry>();
        entry->token = token;
        state_->activeEntries_[requestKey] = std::move(entry);

        state_->queue_.push_back(Task{ requestKey, token, std::move(execute), std::move(commit) });
    }

    // 解锁后唤醒单个 worker。
    state_->queueCv_.notify_one();
    return SubmitResult::Accepted;
}

bool F0ExtractionService::isActive(F0RequestKey requestKey) const
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex_);
    return state_->activeEntries_.find(requestKey) != state_->activeEntries_.end();
}

void F0ExtractionService::cancel(F0RequestKey requestKey)
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex_);
    state_->activeEntries_.erase(requestKey);
}

void F0ExtractionService::workerLoop(std::shared_ptr<SharedState> state)
{
    while (true) {
        // 取任务阶段：临界区内 wait、pop、token 校验，离开作用域自动解锁。
        Task task;
        {
            std::unique_lock<std::mutex> lock(state->entriesMutex_);
            // 阻塞等待：队列非空或 shutdown。shutdown 唤醒后直接退出。
            state->queueCv_.wait(lock, [&state]() { return state->shutdownStarted_ || !state->queue_.empty(); });
            if (state->shutdownStarted_)
                return;

            // pop 与 active token 校验在同一临界区：deque 状态与 active 表视图一致。
            task = std::move(state->queue_.front());
            state->queue_.pop_front();
            auto it = state->activeEntries_.find(task.requestKey);
            if (it == state->activeEntries_.end() || it->second->token != task.token) {
                continue; // 作用域退出自动解锁。
            }
        }

        // 锁外 execute：execute 只访问进程寿命的 F0InferenceService 与提交时
        // 捕获的纯数据快照，owner 销毁后仍安全。
        Result result;
        try {
            result = task.execute(state->runOwnerState_);
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
            std::lock_guard<std::mutex> lock(state->entriesMutex_);
            // 关闭已启动：不再投递 commit（owner 可能已析构；active 表已被 shutdown 清空）。
            if (!state->shutdownStarted_) {
                auto it2 = state->activeEntries_.find(task.requestKey);
                if (it2 != state->activeEntries_.end() && it2->second->token == task.token) {
                    shouldCommit = true;
                    state->activeEntries_.erase(it2);
                }
            }
        }

        if (!shouldCommit) {
            continue;
        }

        // 锁外投递 commit：经消息线程执行，调用方以 completion gate 决定是否访问 owner。
        juce::MessageManager::callAsync([commit = std::move(task.commit), result = std::move(result)]() mutable {
            commit(std::move(result));
        });
    }
}

} // namespace OpenTune
