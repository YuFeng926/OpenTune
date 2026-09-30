#pragma once

#include "RenderJob.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace OpenTune {

/**
 * RenderExecutionLease — 渲染执行租约。
 * 
 * 处理器桥接短租约，RenderWorker 不长期持有 callback。
 * Worker 调用 callback 时传入完整的 RenderJob。
 * 析构时必须 detach，防止 dangling lambda。
 */
struct RenderExecutionLease
{
    void* owner{nullptr};
    std::function<void(RenderJob&)> renderJobCallback;

    bool isValid() const noexcept
    {
        return owner != nullptr && renderJobCallback != nullptr;
    }
};

/**
 * RenderWorker — 异步渲染队列和工作线程。
 * 
 * 管理带身份的 Stage1 chunk 队列、Stage2 队列、worker thread 生命周期和 execution lease。
 */
class RenderWorker
{
public:
    struct AsyncState
    {
        struct Control
        {
            std::mutex mutex;
            std::atomic<int> count{0};
            bool closed{false};
            std::condition_variable cv;
            std::condition_variable* workerCv{nullptr};
        };

        std::shared_ptr<Control> control;
    };

    RenderWorker();
    ~RenderWorker();

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    void attachExecutionLease(RenderExecutionLease lease);
    /**
     * detachExecutionLease — 终止执行租约（析构路径专用）。
     * 立即清空 lease 与排队 job，并等待正在执行的 renderJobCallback 和
     * owner-free async completion 全部归还。这样 CRS/Domain 析构不会与仍在
     * 使用其 session 的 vocoder completion 并行。
     */
    void detachExecutionLease(void* owner);

    // Reconcile 缓存计划与物理 Stage1 队列的同步必须在同一队列锁临界区内完成：
    // worker loop 也在同一 mutex 下 claim，避免出现「旧 queued job + reconcile
    // 后新 targetRevision」的跨 revision 组合。reconcile 回调在锁内执行。
    void reconcileAndSyncStage1Queue(const RenderJob& templateJob,
                                     const std::function<void()>& reconcile);
    void discardStage1Queue(RenderCache* cache);
    void discardAllStage1Queue();
    void enqueue(RenderJob job);
    std::shared_ptr<AsyncState> beginAsyncJob();
    static void completeAsyncJob(const std::shared_ptr<AsyncState>& state) noexcept;
    static bool isAsyncJobClosed(const std::shared_ptr<AsyncState>& state) noexcept;

    /**
     * pause() 暂停取新同步 job 并等待正在执行的 render callback 完成
     * （inFlight_ == 0），绝不等待异步 vocoder 推理（重置推理后端 /
     * 切换 vocoder 模型前调用）：卡住的推理由调用方 resetVocoder /
     * setVocoderModelWeight 的 SetTerminate + join 终止，onComplete 回调归还计数。
     * drain() 完整合同：等待 queue_ 空且 inFlight_ == 0 && asyncInFlight_ == 0。
     * 导出路径在读取渲染结果前必须调用，保证最新完整数据已落盘。
     * 析构路径不得使用 drain()/pause() 编排：只通过
     * detachExecutionLease() 统一收敛同步和异步 lease。
     */
    void pause();
    void resume();
    void drain();
    void waitAsyncIdle();
    void stop();

private:
    void loop();
    void enqueueLocked(RenderJob job);

    // 物理 Stage1 队列与 cache pending chunk 集合同步。仅可在 mutex_ 持有时调用。
    // 每个 (RenderCache, chunk start) 身份只允许一个排队项。
    void syncStage1QueueLocked(const RenderJob& templateJob);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<RenderJob> queue_;

    RenderExecutionLease lease_;

    std::thread thread_;
    std::atomic<bool> stopping_{false};
    bool paused_{false};
    int inFlight_{0};
    std::shared_ptr<AsyncState::Control> asyncControl_;
};

} // namespace OpenTune
