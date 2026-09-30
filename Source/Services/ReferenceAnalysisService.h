#pragma once

#include "../Content/ContentKey.h"
#include "../DSP/ReferenceFeatures.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace OpenTune {

class ReferenceAnalysisService {
public:
    struct AnalysisJobKey {
        ContentKey contentKey;
        int64_t  inputFingerprint{0};
        ReferenceFeatureProducer producer{ReferenceFeatureProducer::Unknown};

        // pendingJobs_ stores the latest job per ContentKey. Equality identifies
        // the exact active job so duplicate submissions do not restart analysis.
        bool operator==(const AnalysisJobKey& rhs) const noexcept
        {
            return contentKey == rhs.contentKey
                && inputFingerprint == rhs.inputFingerprint
                && producer == rhs.producer;
        }
    };

    using AnalysisFunc = std::function<ReferenceFeatureSet(
        const AnalysisJobKey& jobKey)>;
    using AsyncAnalysisFunc = std::function<void(
        const AnalysisJobKey& jobKey,
        std::function<void(ReferenceFeatureSet)> completion)>;
    using CompletionFunc = std::function<void(
        const AnalysisJobKey& jobKey, const ReferenceFeatureSet& result)>;
    using NotificationDispatcher = std::function<void(std::function<void()> task)>;

    // 每个 job 携带 analysis 与 completion 函数：不存在 service 级分析函数、
    // Listener 或 raw-this 通知路径。analysis 在 worker 上执行（纯数据，不访问
    // owner）；completion 由 worker 经 dispatcher 投递到消息线程，调用方在
    // completion gate 关闭后直接丢弃。
    struct ReferenceJob {
        AnalysisJobKey key;
        AnalysisFunc analysis;
        AsyncAnalysisFunc asyncAnalysis;
        CompletionFunc completion;

        bool operator==(const ReferenceJob& rhs) const noexcept
        {
            return key == rhs.key;
        }
    };

    ReferenceAnalysisService();
    ~ReferenceAnalysisService();

    /// 提交分析 job：活跃 job 完全一致时不重启分析，同一 ContentKey 只保留
    /// 最新 pending job。analysis 在 worker 上执行；completion 经 dispatcher
    /// 投递到消息线程。
    void submitAnalysis(ContentKey key, int64_t inputFingerprint,
                        ReferenceFeatureProducer producer,
                        AnalysisFunc analysis, CompletionFunc completion);

    void submitAsyncAnalysis(ContentKey key, int64_t inputFingerprint,
                             ReferenceFeatureProducer producer,
                             AsyncAnalysisFunc analysis,
                             CompletionFunc completion);

    void setNotificationDispatcher(NotificationDispatcher dispatcher);

    /// 关闭 owner：停止接受提交，清空 pending 与 active，唤醒并 join worker。
    /// 异步 GAME 任务不在本 worker 上执行；其进程级 owner 继续运行，关闭开始
    /// 后经 owner-free gate 丢弃 completion。
    void shutdown();

private:
    // worker 线程执行状态全部放入 shared state：worker 捕获 state（shared_ptr）
    // 而非 service this；owner 销毁后 state 由 worker 自身持有直到其退出，因此
    // worker 永不触碰已析构的 service。
    struct SharedState {
        class DispatcherFailureMailbox final : public juce::AsyncUpdater {
        public:
            void enqueue(std::function<void()> task);
            void cancelAndClear();
            void handleAsyncUpdate() override;

        private:
            std::mutex mutex_;
            std::deque<std::function<void()>> queue_;
        };

        std::mutex mutex;
        std::condition_variable cv;

        std::map<ContentKey, ReferenceJob> pendingJobs;
        std::optional<ReferenceJob> activeJob;

        std::atomic<bool> running{true};
        NotificationDispatcher notificationDispatcher;
        std::shared_ptr<DispatcherFailureMailbox> dispatcherFailureMailbox;
    };

    std::shared_ptr<SharedState> state_;
    std::mutex shutdownMutex_;
    std::thread worker_;

    static void workerLoop(std::shared_ptr<SharedState> state);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceAnalysisService)
};

} // namespace OpenTune
