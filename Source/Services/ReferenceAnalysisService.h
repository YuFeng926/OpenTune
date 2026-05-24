#pragma once

#include "../Utils/AppLogger.h"
#include "../MaterializationStore.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <thread>

namespace OpenTune {

/**
 * @brief 后台分析服务，负责 per-materialization 的 derived analysis 生成。
 *
 * 去重：同一 materializationId 的分析任务只保留一个（已在队列中则丢弃新请求）。
 * stale 丢弃：提交前验证 materialization renderRevision 与 job 创建时一致。
 * 此服务不持有 MaterializationStore 引用 —— stale 检查与结果写入由调用方在
 * Listener 回调中完成。
 */
class ReferenceAnalysisService {
public:
    // ==========================================================================
    // Job Key
    // ==========================================================================
    struct AnalysisJobKey {
        uint64_t materializationId{0};
        int64_t  renderRevision{0};           // materialization renderRevision at submit time
        int64_t  analysisRevision{0};          // existing analysis revision, for stale check
    };

    // ==========================================================================
    // 分析函数类型（调用方注入，避免服务耦合具体算法）
    // ==========================================================================
    using AnalysisFunc = std::function<MaterializationStore::DerivedAnalysis(
        const AnalysisJobKey& jobKey)>;
    using NotificationDispatcher = std::function<void(std::function<void()> task)>;

    // ==========================================================================
    // Listener
    // ==========================================================================
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void analysisCompleted(uint64_t materializationId,
                                       const MaterializationStore::DerivedAnalysis& result) = 0;
        virtual void analysisFailed(uint64_t materializationId,
                                    const juce::String& reason) = 0;
    };

    // ==========================================================================
    // API
    // ==========================================================================
    ReferenceAnalysisService();
    ~ReferenceAnalysisService();

    /** 注入分析函数（必须在 submitAnalysis 前调用） */
    void setAnalysisFunc(AnalysisFunc func);
    void setNotificationDispatcher(NotificationDispatcher dispatcher);

    /** 添加 Listener */
    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    /** 提交分析任务（自动去重） */
    void submitAnalysis(uint64_t materializationId, int64_t renderRevision);

    /** 取消指定 materialization 的分析 */
    void cancelAnalysis(uint64_t materializationId);

    /** 取消所有分析 */
    void cancelAll();

    /** 是否有分析正在进行 */
    bool isAnalysisInProgress(uint64_t materializationId) const;

private:
    void workerLoop();
    void notifyListenersCompleted(uint64_t matId, const MaterializationStore::DerivedAnalysis& result);
    void notifyListenersFailed(uint64_t matId, const juce::String& reason);

    AnalysisFunc analysisFunc_;
    NotificationDispatcher notificationDispatcher_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // 待处理队列（dedup by materializationId）
    // 使用 std::map + 最新 key（latest-wins 去重）
    std::map<uint64_t, AnalysisJobKey> pendingJobs_;  // matId -> key

    // 当前正在处理
    std::optional<AnalysisJobKey> activeJob_;
    std::set<uint64_t> cancelledActiveJobs_;

    std::atomic<bool> running_{true};
    std::shared_ptr<std::atomic<bool>> aliveToken_{std::make_shared<std::atomic<bool>>(true)};
    std::thread workerThread_;

    juce::ListenerList<Listener> listeners_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceAnalysisService)
};

} // namespace OpenTune
