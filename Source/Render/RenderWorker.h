#pragma once

#include "RenderJob.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace OpenTune {

/**
 * RenderExecutionLease — 渲染执行租约。
 * 
 * 处理器桥接短租约，RenderWorker 不长期持有 callback。
 * Worker 调用 callback 时传入完整的 RenderJob。
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
 * 管理 chunk render 队列、worker thread 生命周期、execution lease。
 * 
 * Phase 0: 提取自 CRS partial-render queue (cpp:765-790)
 *          和 ContentRenderService worker (h:189-199, cpp:93-257)
 * 
 * 修正 CRS 的两个 bug：
 * 1. targetRevision 丢失 - PendingRenderJob 有但 PendingRenderEntry 没有
 * 2. condition_variable wait 持有外层锁 - renderQueueMutex 在 wait 时仍被持有
 */
class RenderWorker
{
public:
    RenderWorker();
    ~RenderWorker();

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    void attachExecutionLease(RenderExecutionLease lease);
    void detachExecutionLease(void* owner);

    void enqueue(RenderJob job);
    bool hasPendingJobs() const;

    void pause();
    void resume();
    void drain();
    void stop();

private:
    void loop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<RenderJob> queue_;

    RenderExecutionLease lease_;

    std::thread thread_;
    std::atomic<bool> stopping_{false};
    bool paused_{false};
    int inFlight_{0};
};

} // namespace OpenTune
