#include "ReferenceAnalysisService.h"

#include "../Utils/AppLogger.h"

#include <juce_events/juce_events.h>
#include <exception>

namespace OpenTune {

ReferenceAnalysisService::ReferenceAnalysisService()
{
    state_ = std::make_shared<SharedState>();
    // detached 进程常驻执行器：捕获 shared state 而非 service this。
    // owner 析构后 worker 见 running==false 自行退出；共享 state 由 worker
    // 自身持有，保证其生命周期覆盖最后一次使用。
    std::thread worker([state = state_]() {
        try {
            workerLoop(state);
        } catch (...) {
            AppLogger::error("[ReferenceAnalysisService] workerLoop threw unknown exception");
        }
    });
    worker.detach();
}

ReferenceAnalysisService::~ReferenceAnalysisService()
{
    shutdown();
}

void ReferenceAnalysisService::shutdown()
{
    // 关闭 owner：清空 pending 与 active、唤醒 worker。不 join —— worker 是
    // detached 进程常驻执行器；活跃 GAME 任务允许在后台完成（进程级共享
    // generator，永不 terminateRun）。关闭后不再投递 completion。
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->pendingJobs.clear();
        state_->activeJob.reset();
        state_->running.store(false, std::memory_order_release);
    }
    state_->cv.notify_all();
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

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->running.load(std::memory_order_acquire)) {
        return;
    }
    if (state_->activeJob.has_value() && *state_->activeJob == jobKey) {
        return;
    }
    state_->pendingJobs[key] = jobKey;
    state_->cv.notify_one();
}

void ReferenceAnalysisService::workerLoop(std::shared_ptr<SharedState> state)
{
    while (true) {
        ReferenceJob job;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&state]() {
                return !state->running.load(std::memory_order_acquire) || !state->pendingJobs.empty();
            });

            if (!state->running.load(std::memory_order_acquire)) {
                break;
            }

            auto it = state->pendingJobs.begin();
            job = it->second;
            state->pendingJobs.erase(it);
            state->activeJob = job;
        }

        // 锁外执行 analysis：纯数据函数，不访问 owner；GAME 分析走进程级共享
        // generator（ProcessF0Runtime::generateNotes），owner 销毁后仍安全。
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

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->activeJob.has_value() && state->activeJob->key == job.key) {
                state->activeJob.reset();
            }
            // 关闭后不再投递 completion（调用方 gate 也已关闭）。
            if (!state->running.load(std::memory_order_acquire)) {
                continue;
            }
        }

        const AnalysisJobKey key = job.key;
        auto completion = std::move(job.completion);
        auto notify = [key, result, completion]() mutable {
            completion(key, result);
        };

        NotificationDispatcher dispatcher;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            dispatcher = state->notificationDispatcher;
        }
        if (dispatcher) {
            dispatcher(std::move(notify));
        } else {
            juce::MessageManager::callAsync(std::move(notify));
        }
    }
}

} // namespace OpenTune
