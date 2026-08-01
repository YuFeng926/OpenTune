#include "ReferenceAnalysisService.h"

#include "../Utils/AppLogger.h"

#include <juce_events/juce_events.h>
#include <exception>

namespace OpenTune {

ReferenceAnalysisService::ReferenceAnalysisService()
{
    workerThread_ = std::thread([this]() {
        try {
            workerLoop();
        } catch (const std::exception& e) {
            AppLogger::error("[ReferenceAnalysisService] workerLoop threw: " + juce::String(e.what()));
        } catch (...) {
            AppLogger::error("[ReferenceAnalysisService] workerLoop threw unknown exception");
        }
    });
}

ReferenceAnalysisService::~ReferenceAnalysisService()
{
    shutdown();
}

void ReferenceAnalysisService::shutdown()
{
    aliveToken_->store(false, std::memory_order_release);
    const bool wasRunning = running_.exchange(false, std::memory_order_release);
    if (!wasRunning) {
        if (workerThread_.joinable())
            workerThread_.join();
        return;
    }

    std::function<void()> terminateFnCopy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingJobs_.clear();
        // 锁内仅复制：terminateFn_ 是用户回调，锁外执行（先终止活跃推理再 join，
        // 现语义保持，只是解除锁内调用耦合）。
        if (terminateFn_)
            terminateFnCopy = terminateFn_;
    }
    if (terminateFnCopy)
        terminateFnCopy();

    cv_.notify_all();

    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void ReferenceAnalysisService::setAnalysisFunc(AnalysisFunc func)
{
    std::lock_guard<std::mutex> lock(mutex_);
    analysisFunc_ = std::move(func);
}

void ReferenceAnalysisService::setNotificationDispatcher(NotificationDispatcher dispatcher)
{
    std::lock_guard<std::mutex> lock(mutex_);
    notificationDispatcher_ = std::move(dispatcher);
}

void ReferenceAnalysisService::setTerminateFn(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    terminateFn_ = std::move(fn);
}

void ReferenceAnalysisService::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void ReferenceAnalysisService::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

void ReferenceAnalysisService::submitAnalysis(ContentKey key,
                                               int64_t inputFingerprint,
                                               ReferenceFeatureProducer producer)
{
    if (!key.isValid()) {
        AppLogger::warn("[ReferenceAnalysisService] submitAnalysis rejected: invalid contentKey");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    if (!analysisFunc_) {
        AppLogger::warn("[ReferenceAnalysisService] submitAnalysis rejected: analysisFunc_ not set");
        return;
    }

    AnalysisJobKey jobKey;
    jobKey.contentKey = key;
    jobKey.inputFingerprint = inputFingerprint;
    jobKey.producer = producer;

    if (activeJob_.has_value() && *activeJob_ == jobKey) {
        return;
    }

    pendingJobs_[key] = jobKey;
    cv_.notify_one();
}

void ReferenceAnalysisService::workerLoop()
{
    while (running_.load(std::memory_order_acquire)) {
        AnalysisJobKey job;
        AnalysisFunc analysisFunc;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return !running_.load(std::memory_order_acquire) || !pendingJobs_.empty();
            });

            if (!running_.load(std::memory_order_acquire)) {
                break;
            }

            auto it = pendingJobs_.begin();
            job = it->second;
            pendingJobs_.erase(it);
            activeJob_ = job;
            analysisFunc = analysisFunc_;
        }

        const ContentKey key = job.contentKey;
        ReferenceFeatureSet result;

        try {
            if (analysisFunc) {
                result = analysisFunc(job);
            } else {
                result.status = ReferenceFeatureStatus::Failed;
                result.errorMessage = "Reference analysis function is not configured";
            }
        } catch (const std::exception& e) {
            AppLogger::error("[ReferenceAnalysisService] Exception during analysis for contentKey objId="
                + juce::String(static_cast<juce::int64>(key.objectId)) + ": " + juce::String(e.what()));
            result.status = ReferenceFeatureStatus::Failed;
            result.errorMessage = e.what();
        } catch (...) {
            AppLogger::error("[ReferenceAnalysisService] Unknown exception during analysis for contentKey objId="
                + juce::String(static_cast<juce::int64>(key.objectId)));
            result.status = ReferenceFeatureStatus::Failed;
            result.errorMessage = "Unknown exception during analysis";
        }

        result.producer = job.producer;
        result.inputFingerprint = job.inputFingerprint;
        if (result.status == ReferenceFeatureStatus::NotRequested
            || result.status == ReferenceFeatureStatus::Extracting) {
            result.status = ReferenceFeatureStatus::Failed;
            if (result.errorMessage.isEmpty()) {
                result.errorMessage = "Reference analysis did not produce Ready features";
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activeJob_.has_value() && activeJob_->contentKey == key) {
                activeJob_.reset();
            }
        }

        auto notify = [this, aliveToken = aliveToken_, key, result]() {
            if (!aliveToken->load(std::memory_order_acquire))
                return;
            listeners_.call([key, &result](Listener& l) {
                l.analysisFinished(key, result);
            });
        };

        NotificationDispatcher dispatcher;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatcher = notificationDispatcher_;
        }
        if (dispatcher) {
            dispatcher(std::move(notify));
        } else {
            juce::MessageManager::callAsync(std::move(notify));
        }
    }
}

} // namespace OpenTune
