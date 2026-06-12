#include "ReferenceAnalysisService.h"

#include "../Utils/AppLogger.h"

#include <juce_events/juce_events.h>
#include <exception>

namespace OpenTune {

ReferenceAnalysisService::ReferenceAnalysisService()
{
    workerThread_ = std::thread([this]() { workerLoop(); });
}

ReferenceAnalysisService::~ReferenceAnalysisService()
{
    aliveToken_->store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    cancelAll();
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

void ReferenceAnalysisService::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void ReferenceAnalysisService::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

void ReferenceAnalysisService::submitAnalysis(ContentKey key,
                                               int64_t contentRevision)
{
    if (!key.isValid()) {
        AppLogger::warn("[ReferenceAnalysisService] submitAnalysis rejected: invalid contentKey");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!analysisFunc_) {
        AppLogger::warn("[ReferenceAnalysisService] submitAnalysis rejected: analysisFunc_ not set");
        return;
    }

    if (activeJob_.has_value() && activeJob_->contentKey == key) {
        AppLogger::debug("[ReferenceAnalysisService] submitAnalysis: contentKey "
            + juce::String(static_cast<juce::int64>(key.objectId)) + " already active, dropping");
        return;
    }

    AnalysisJobKey jobKey;
    jobKey.contentKey = key;
    jobKey.contentRevision = contentRevision;
    pendingJobs_[key] = jobKey;
    cv_.notify_one();
}

void ReferenceAnalysisService::cancelAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pendingJobs_.clear();
    if (activeJob_.has_value()) {
        cancelledActiveJobs_.insert(activeJob_->contentKey);
    }
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
        bool success = false;
        juce::String errorReason;

        try {
            if (!analysisFunc) {
                errorReason = "Reference analysis function is not configured";
            } else {
                result = analysisFunc(job);
            }
            success = result.status == ReferenceFeatureStatus::Ready;
            if (!success) {
                errorReason = result.errorMessage.isNotEmpty()
                    ? result.errorMessage
                    : "Reference analysis did not produce Ready features";
            }
        } catch (const std::exception& e) {
            AppLogger::error("[ReferenceAnalysisService] Exception during analysis for contentKey objId="
                + juce::String(static_cast<juce::int64>(key.objectId)) + ": " + juce::String(e.what()));
            errorReason = e.what();
        } catch (...) {
            AppLogger::error("[ReferenceAnalysisService] Unknown exception during analysis for contentKey objId="
                + juce::String(static_cast<juce::int64>(key.objectId)));
            errorReason = "Unknown exception during analysis";
        }

        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activeJob_.has_value() && activeJob_->contentKey == key) {
                activeJob_.reset();
            }

            const auto cancelledIt = cancelledActiveJobs_.find(key);
            if (cancelledIt != cancelledActiveJobs_.end()) {
                cancelled = true;
                cancelledActiveJobs_.erase(cancelledIt);
            }
        }

        if (cancelled) {
            continue;
        }

        if (success) {
            notifyListenersCompleted(key, result);
        } else {
            notifyListenersFailed(key, errorReason);
        }
    }
}

void ReferenceAnalysisService::notifyListenersCompleted(
    ContentKey key, const ReferenceFeatureSet& result)
{
    auto notify = [this, alive = aliveToken_, key, result]() {
        if (!alive->load(std::memory_order_acquire)) {
            return;
        }
        listeners_.call([key, &result](Listener& l) {
            l.analysisCompleted(key, result);
        });
    };

    NotificationDispatcher dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatcher = notificationDispatcher_;
    }
    if (dispatcher) {
        dispatcher(std::move(notify));
        return;
    }

    juce::MessageManager::callAsync(std::move(notify));
}

void ReferenceAnalysisService::notifyListenersFailed(
    ContentKey key, const juce::String& reason)
{
    auto notify = [this, alive = aliveToken_, key, reason]() {
        if (!alive->load(std::memory_order_acquire)) {
            return;
        }
        listeners_.call([key, &reason](Listener& l) {
            l.analysisFailed(key, reason);
        });
    };

    NotificationDispatcher dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatcher = notificationDispatcher_;
    }
    if (dispatcher) {
        dispatcher(std::move(notify));
        return;
    }

    juce::MessageManager::callAsync(std::move(notify));
}

} // namespace OpenTune
