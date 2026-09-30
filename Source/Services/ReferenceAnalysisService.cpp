#include "ReferenceAnalysisService.h"

#include "../Utils/AppLogger.h"

#include <juce_events/juce_events.h>
#include <exception>

namespace OpenTune {

void ReferenceAnalysisService::SharedState::DispatcherFailureMailbox::enqueue(
    std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    triggerAsyncUpdate();
}

void ReferenceAnalysisService::SharedState::DispatcherFailureMailbox::cancelAndClear()
{
    cancelPendingUpdate();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

void ReferenceAnalysisService::SharedState::DispatcherFailureMailbox::handleAsyncUpdate()
{
    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks.swap(queue_);
    }
    for (auto& task : tasks)
    {
        try
        {
            task();
        }
        catch (const std::exception& e)
        {
            AppLogger::error("[ReferenceAnalysisService] mailbox completion threw: "
                + juce::String(e.what()));
        }
        catch (...)
        {
            AppLogger::error("[ReferenceAnalysisService] mailbox completion threw: unknown exception");
        }
    }
}

ReferenceAnalysisService::ReferenceAnalysisService()
{
    state_ = std::make_shared<SharedState>();
    state_->dispatcherFailureMailbox = std::make_shared<SharedState::DispatcherFailureMailbox>();
    // worker 捕获 shared state 而非 service this，由 shutdown() 负责 join。
    worker_ = std::thread([state = state_]() { workerLoop(state); });
}

ReferenceAnalysisService::~ReferenceAnalysisService()
{
    shutdown();
}

void ReferenceAnalysisService::shutdown()
{
    std::lock_guard<std::mutex> shutdownLock(shutdownMutex_);
    if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id()) {
        AppLogger::error("[ReferenceAnalysisService] shutdown called from worker thread");
        jassertfalse;
        std::terminate();
    }
    std::map<ContentKey, ReferenceJob> discardedJobs;
    std::optional<ReferenceJob> discardedActiveJob;
    // 关闭 owner：清空 pending 与 active，唤醒并 join worker；活跃 GAME 任务
    // 允许在 shutdown 期间完成（进程级共享 generator，永不 terminateRun）。
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        discardedJobs = std::move(state_->pendingJobs);
        discardedActiveJob = std::move(state_->activeJob);
        state_->running.store(false, std::memory_order_release);
    }
    state_->cv.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }
    state_->dispatcherFailureMailbox->cancelAndClear();
}

void ReferenceAnalysisService::setNotificationDispatcher(NotificationDispatcher dispatcher)
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->notificationDispatcher = std::move(dispatcher);
}

void ReferenceAnalysisService::submitAnalysis(ContentKey key,
                                               int64_t inputFingerprint,
                                               ReferenceFeatureProducer producer,
                                               AnalysisFunc analysis,
                                               CompletionFunc completion)
{
    if (!key.isValid()) {
        AppLogger::warn("[ReferenceAnalysisService] submitAnalysis rejected: invalid contentKey");
        return;
    }
    if (!analysis || !completion) {
        AppLogger::warn("[ReferenceAnalysisService] submitAnalysis rejected: analysis/completion missing");
        return;
    }

    ReferenceJob jobKey;
    jobKey.key.contentKey = key;
    jobKey.key.inputFingerprint = inputFingerprint;
    jobKey.key.producer = producer;
    jobKey.analysis = std::move(analysis);
    jobKey.completion = std::move(completion);

    bool notifyWorker = false;
    ReferenceJob discardedJob;
    std::map<ContentKey, ReferenceJob>::node_type discardedNode;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->running.load(std::memory_order_acquire)) {
            return;
        }
        if (state_->activeJob.has_value() && *state_->activeJob == jobKey) {
            return;
        }

        discardedNode = state_->pendingJobs.extract(key);
        if (!discardedNode.empty()) {
            discardedJob = std::move(discardedNode.mapped());
        }
        state_->pendingJobs.emplace(key, std::move(jobKey));
        notifyWorker = true;
    }
    if (notifyWorker) {
        state_->cv.notify_one();
    }
}

void ReferenceAnalysisService::submitAsyncAnalysis(ContentKey key,
                                                   int64_t inputFingerprint,
                                                   ReferenceFeatureProducer producer,
                                                   ReferenceAnalysisService::AsyncAnalysisFunc analysis,
                                                   CompletionFunc completion)
{
    if (!key.isValid() || !analysis || !completion)
        return;

    ReferenceJob job;
    job.key = {key, inputFingerprint, producer};
    job.asyncAnalysis = std::move(analysis);
    job.completion = std::move(completion);

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->running.load(std::memory_order_acquire))
            return;
        if (state_->activeJob.has_value() && *state_->activeJob == job)
            return;
        state_->pendingJobs.insert_or_assign(key, std::move(job));
    }
    state_->cv.notify_one();
}

void ReferenceAnalysisService::workerLoop(std::shared_ptr<SharedState> state)
{
    while (true) {
        ReferenceJob job;
        std::map<ContentKey, ReferenceJob>::node_type pendingNode;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&state]() {
                return !state->running.load(std::memory_order_acquire)
                    || (state->activeJob == std::nullopt && !state->pendingJobs.empty());
            });

            if (!state->running.load(std::memory_order_acquire)) {
                break;
            }

            auto it = state->pendingJobs.begin();
            pendingNode = state->pendingJobs.extract(it);
            job = std::move(pendingNode.mapped());
            state->activeJob = job;
        }

        if (job.asyncAnalysis) {
            auto asyncAnalysis = std::move(job.asyncAnalysis);
            auto completion = std::move(job.completion);
            const auto key = job.key;
            auto completionClaim = std::make_shared<std::atomic<bool>>(false);
            auto completeAsync = [state, key, completionClaim,
                                  completion = std::move(completion)]
                                 (ReferenceFeatureSet result) mutable {
                if (completionClaim->exchange(true, std::memory_order_acq_rel))
                    return;
                NotificationDispatcher dispatcher;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->activeJob.has_value() && state->activeJob->key == key)
                        state->activeJob.reset();
                    if (!state->running.load(std::memory_order_acquire))
                        return;
                    dispatcher = state->notificationDispatcher;
                }
                auto notify = [key, result, completion = std::move(completion)]() mutable {
                    completion(key, result);
                };
                if (dispatcher)
                    dispatcher(std::move(notify));
                else
                {
                    const bool posted = juce::MessageManager::callAsync(notify);
                    if (!posted)
                    {
                        AppLogger::error("[ReferenceAnalysisService] async completion dispatcher rejected; mailbox fallback");
                        state->dispatcherFailureMailbox->enqueue(std::move(notify));
                    }
                }
                state->cv.notify_one();
            };
            try
            {
                asyncAnalysis(key, completeAsync);
            }
            catch (const std::exception& e)
            {
                AppLogger::error("[ReferenceAnalysisService] async analysis submission failed: "
                    + juce::String(e.what()));
                ReferenceFeatureSet failed;
                failed.status = ReferenceFeatureStatus::Failed;
                failed.errorMessage = "async_analysis_submission_exception";
                completeAsync(std::move(failed));
            }
            catch (...)
            {
                AppLogger::error("[ReferenceAnalysisService] async analysis submission failed: unknown exception");
                ReferenceFeatureSet failed;
                failed.status = ReferenceFeatureStatus::Failed;
                failed.errorMessage = "async_analysis_submission_exception";
                completeAsync(std::move(failed));
            }
            continue;
        }

        // 锁外执行同步 analysis：纯数据函数，不访问 owner。
        ReferenceFeatureSet result;
        try {
            if (job.analysis) {
                result = job.analysis(job.key);
            } else {
                result.status = ReferenceFeatureStatus::Failed;
                result.errorMessage = "Reference analysis function is missing";
            }
        } catch (...) {
            AppLogger::error("[ReferenceAnalysisService] Unknown exception during analysis for contentKey objId="
                + juce::String(static_cast<juce::int64>(job.key.contentKey.objectId)));
            result.status = ReferenceFeatureStatus::Failed;
            result.errorMessage = "Unknown exception during analysis";
        }

        result.producer = job.key.producer;
        result.inputFingerprint = job.key.inputFingerprint;
        if (result.status == ReferenceFeatureStatus::NotRequested
            || result.status == ReferenceFeatureStatus::Extracting) {
            result.status = ReferenceFeatureStatus::Failed;
            if (result.errorMessage.isEmpty()) {
                result.errorMessage = "Reference analysis did not produce Ready features";
            }
        }

        NotificationDispatcher dispatcher;
        std::optional<ReferenceJob> releasedActiveJob;
        bool shouldNotify = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->activeJob.has_value() && state->activeJob->key == job.key) {
                releasedActiveJob = std::move(state->activeJob);
            }
            // 关闭后不再投递 completion（调用方 gate 也已关闭）。
            if (!state->running.load(std::memory_order_acquire)) {
                continue;
            }
            dispatcher = state->notificationDispatcher;
            shouldNotify = true;
        }

        const AnalysisJobKey key = job.key;
        auto completion = std::move(job.completion);
        auto notify = [key, result, completion]() mutable {
            completion(key, result);
        };

        if (shouldNotify) {
            if (dispatcher) {
                dispatcher(std::move(notify));
            } else {
                const bool posted = juce::MessageManager::callAsync(notify);
                if (!posted)
                {
                    AppLogger::error("[ReferenceAnalysisService] completion dispatcher rejected; mailbox fallback");
                    state->dispatcherFailureMailbox->enqueue(std::move(notify));
                }
            }
        }
    }
}

} // namespace OpenTune
