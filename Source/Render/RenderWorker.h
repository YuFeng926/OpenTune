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
    /**
     * detachExecutionLease — 终止执行租约（析构路径专用）。
     * 立即清空 lease 与排队 job，并等待正在执行的 renderJobCallback 完成。
     * 不等待 asyncInFlight_：vocoder 推理不可取消，其完成不依赖本 worker 存活
     * （onComplete 经 shared_ptr 持有 ContentRenderService，必然回调）。
     */
    void detachExecutionLease(void* owner);

    void enqueue(RenderJob job);
    void beginAsyncJob();
    void completeAsyncJob();

    /**
     * pause() 暂停取新同步 job 并等待正在执行的 render callback 完成
     * （inFlight_ == 0），绝不等待异步 vocoder 推理（重置推理后端 /
     * 切换 vocoder 模型前调用）：卡住的推理由调用方 resetVocoder /
     * setVocoderModelWeight 的 SetTerminate + join 终止，onComplete 回调归还计数。
     * drain() 完整合同：等待 queue_ 空且 inFlight_ == 0 && asyncInFlight_ == 0。
     * 导出路径在读取渲染结果前必须调用，保证最新完整数据已落盘。
     * 析构路径不得使用 drain()/pause() 编排：vocoder 推理不可取消，
     * 其完成不依赖本 worker 存活，析构只调 detachExecutionLease。
     */
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
