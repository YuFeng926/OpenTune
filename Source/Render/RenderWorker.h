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
 * 管理 chunk render 队列、worker thread 生命周期、execution lease。
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
    void beginAsyncJob();
    void completeAsyncJob();

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
    int asyncInFlight_{0};
};

} // namespace OpenTune
