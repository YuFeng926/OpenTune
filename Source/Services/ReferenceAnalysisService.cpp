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

void ReferenceAnalysisService::submitAnalysis(uint64_t materializationId,
                                               int64_t renderRevision)
{
    if (materializationId == 0) {
        AppLogger::warn("[ReferenceAnalysisService] submitAnalysis rejected: materializationId is 0");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!analysisFunc_) {
        AppLogger::warn("[ReferenceAnalysisService] submitAnalysis rejected: analysisFunc_ not set");
        return;
    }

    if (activeJob_.has_value() && activeJob_->materializationId == materializationId) {
        AppLogger::debug("[ReferenceAnalysisService] submitAnalysis: materialization "
            + juce::String(materializationId) + " already active, dropping");
        return;
    }

    AnalysisJobKey key;
    key.materializationId = materializationId;
    key.renderRevision = renderRevision;
    pendingJobs_[materializationId] = key;
    cv_.notify_one();
}

void ReferenceAnalysisService::cancelAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pendingJobs_.clear();
    if (activeJob_.has_value()) {
        cancelledActiveJobs_.insert(activeJob_->materializationId);
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

        const uint64_t matId = job.materializationId;
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
            AppLogger::error("[ReferenceAnalysisService] Exception during analysis for matId "
                + juce::String(matId) + ": " + juce::String(e.what()));
            errorReason = e.what();
        } catch (...) {
            AppLogger::error("[ReferenceAnalysisService] Unknown exception during analysis for matId "
                + juce::String(matId));
            errorReason = "Unknown exception during analysis";
        }

        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activeJob_.has_value() && activeJob_->materializationId == matId) {
                activeJob_.reset();
            }

            const auto cancelledIt = cancelledActiveJobs_.find(matId);
            if (cancelledIt != cancelledActiveJobs_.end()) {
                cancelled = true;
                cancelledActiveJobs_.erase(cancelledIt);
            }
        }

        if (cancelled) {
            continue;
        }

        if (success) {
            notifyListenersCompleted(matId, result);
        } else {
            notifyListenersFailed(matId, errorReason);
        }
    }
}

void ReferenceAnalysisService::notifyListenersCompleted(
    uint64_t matId, const ReferenceFeatureSet& result)
{
    auto notify = [this, alive = aliveToken_, matId, result]() {
        if (!alive->load(std::memory_order_acquire)) {
            return;
        }
        listeners_.call([matId, &result](Listener& l) {
            l.analysisCompleted(matId, result);
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
    uint64_t matId, const juce::String& reason)
{
    auto notify = [this, alive = aliveToken_, matId, reason]() {
        if (!alive->load(std::memory_order_acquire)) {
            return;
        }
        listeners_.call([matId, &reason](Listener& l) {
            l.analysisFailed(matId, reason);
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
