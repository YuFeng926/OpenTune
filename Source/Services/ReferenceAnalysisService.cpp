#include "ReferenceAnalysisService.h"
#include "../Utils/AppLogger.h"
#include <juce_events/juce_events.h>
#include <exception>

namespace OpenTune {

// ============================================================================
// Construction / Destruction
// ============================================================================

ReferenceAnalysisService::ReferenceAnalysisService()
{
    workerThread_ = std::thread([this]() { workerLoop(); });
}

ReferenceAnalysisService::~ReferenceAnalysisService()
{
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

// ============================================================================
// Configuration
// ============================================================================

void ReferenceAnalysisService::setAnalysisFunc(AnalysisFunc func)
{
    analysisFunc_ = std::move(func);
}

// ============================================================================
// Listener Management
// ============================================================================

void ReferenceAnalysisService::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void ReferenceAnalysisService::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

// ============================================================================
// Submit / Cancel / Query
// ============================================================================

void ReferenceAnalysisService::submitAnalysis(uint64_t materializationId,
                                               int64_t renderRevision)
{
    if (materializationId == 0) {
        AppLogger::warn("[ReferenceAnalysisService] submitAnalysis rejected: materializationId is 0");
        return;
    }

    if (!analysisFunc_) {
        AppLogger::warn("[ReferenceAnalysisService] submitAnalysis rejected: analysisFunc_ not set");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 如果同一 matId 正在活跃处理中，丢弃
    if (activeJob_.has_value() && activeJob_->materializationId == materializationId) {
        AppLogger::debug("[ReferenceAnalysisService] submitAnalysis: materialization "
            + juce::String(materializationId) + " already active, dropping");
        return;
    }

    // 同一 matId 已在 pendingJobs_ 中：覆盖（latest-wins 去重）
    AnalysisJobKey key;
    key.materializationId = materializationId;
    key.renderRevision = renderRevision;
    key.analysisRevision = 0; // 由调用方在 Listener 中处理

    pendingJobs_[materializationId] = key;
    cv_.notify_one();
}

void ReferenceAnalysisService::cancelAnalysis(uint64_t materializationId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pendingJobs_.erase(materializationId);
}

void ReferenceAnalysisService::cancelAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pendingJobs_.clear();
}

bool ReferenceAnalysisService::isAnalysisInProgress(uint64_t materializationId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeJob_.has_value() && activeJob_->materializationId == materializationId) {
        return true;
    }
    return pendingJobs_.find(materializationId) != pendingJobs_.end();
}

// ============================================================================
// Worker Loop
// ============================================================================

void ReferenceAnalysisService::workerLoop()
{
    while (running_.load(std::memory_order_acquire)) {
        AnalysisJobKey job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return !running_.load(std::memory_order_acquire) || !pendingJobs_.empty();
            });

            if (!running_.load(std::memory_order_acquire)) {
                break;
            }

            // 从 pendingJobs_ 取第一个任务
            auto it = pendingJobs_.begin();
            job = it->second;
            pendingJobs_.erase(it);

            // 标记为 active
            activeJob_ = job;
        }

        const uint64_t matId = job.materializationId;

        // 执行分析（同步，在 worker 线程）
        MaterializationStore::DerivedAnalysis result;
        bool success = false;
        juce::String errorReason;

        try {
            result = analysisFunc_(matId);
            success = true;
        } catch (const std::exception& e) {
            AppLogger::error("[ReferenceAnalysisService] Exception during analysis for matId "
                + juce::String(matId) + ": " + juce::String(e.what()));
            errorReason = e.what();
        } catch (...) {
            AppLogger::error("[ReferenceAnalysisService] Unknown exception during analysis for matId "
                + juce::String(matId));
            errorReason = "Unknown exception during analysis";
        }

        // 清除 active job
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activeJob_.has_value() && activeJob_->materializationId == matId) {
                activeJob_.reset();
            }
        }

        // 通知 Listener（在消息线程上执行）
        // stale 检查与存储写入由调用方在 Listener 回调中完成
        if (success) {
            notifyListenersCompleted(matId, result);
        } else {
            notifyListenersFailed(matId, errorReason);
        }
    }
}

// ============================================================================
// Listener Notification Helpers
// ============================================================================

void ReferenceAnalysisService::notifyListenersCompleted(
    uint64_t matId, const MaterializationStore::DerivedAnalysis& result)
{
    juce::MessageManager::callAsync([this, matId, result]() {
        listeners_.call([matId, &result](Listener& l) {
            l.analysisCompleted(matId, result);
        });
    });
}

void ReferenceAnalysisService::notifyListenersFailed(
    uint64_t matId, const juce::String& reason)
{
    juce::MessageManager::callAsync([this, matId, reason]() {
        listeners_.call([matId, &reason](Listener& l) {
            l.analysisFailed(matId, reason);
        });
    });
}

} // namespace OpenTune
