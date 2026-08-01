#include "RenderWorker.h"
#include "../Runtime/ProcessRenderRuntime.h"

namespace OpenTune {

RenderWorker::RenderWorker()
{
    thread_ = std::thread([this] { loop(); });
}

RenderWorker::~RenderWorker()
{
    stop();
}

void RenderWorker::stop()
{
    if (!thread_.joinable())
        return;

    stopping_.store(true);
    cv_.notify_all();
    thread_.join();
}

// ============================================================
// 执行租约
// ============================================================

void RenderWorker::attachExecutionLease(RenderExecutionLease lease)
{
    drain();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        lease_ = std::move(lease);
    }
    resume();
}

void RenderWorker::detachExecutionLease(void* owner)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (lease_.owner != owner)
            return;
        // 立即清 lease：worker 之后取出的 job leaseCopy 无效 → 只递减 inFlight_ 不执行回调
        lease_ = RenderExecutionLease{};
        queue_.clear(); // 丢弃排队 job（无 lease 可执行）
    }
    cv_.notify_all();
    // 等待正在执行的 renderJobCallback 完成（其回调访问 owner，owner 仍在析构中存活）
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.wait(lk, [this] { return inFlight_ == 0; });
}

// ============================================================
// 渲染队列
// ============================================================

void RenderWorker::enqueue(RenderJob job)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void RenderWorker::beginAsyncJob()
{
    std::lock_guard<std::mutex> lk(mutex_);
    ++asyncInFlight_;
}

void RenderWorker::completeAsyncJob()
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        jassert(asyncInFlight_ > 0);
        --asyncInFlight_;
    }
    cv_.notify_one();
}

// ============================================================
// 暂停 / 恢复 / 排空
// ============================================================

void RenderWorker::pause()
{
    std::unique_lock<std::mutex> lk(mutex_);
    paused_ = true;
    // 只等待正在执行的 renderJobCallback 完成（inFlight_ 归零），绝不等待
    // 异步 vocoder 推理：卡住的 DML Run 只能由调用方随后 resetVocoder /
    // setVocoderModelWeight 的 SetTerminate + join 终止，onComplete 经
    // shared_ptr 回调归还计数。paused_ 保证等待期间不取新 job，resume() 再放行。
    cv_.wait(lk, [this] { return inFlight_ == 0; });
}

void RenderWorker::resume()
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        paused_ = false;
    }
    cv_.notify_all();
}

void RenderWorker::drain()
{
    // 完整合同：等待队列空、同步与异步渲染全部完成。
    // 导出路径依赖此语义（导出前确保最新完整数据已落盘）。
    // enqueue/worker 循环/completeAsyncJob 在状态变化后 notify，谓词等待无忙等。
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.wait(lk, [this] { return queue_.empty() && inFlight_ == 0 && asyncInFlight_ == 0; });
}

// ============================================================
// 工作循环
// ============================================================

void RenderWorker::loop()
{
    while (!stopping_.load())
    {
        RenderJob job;
        RenderExecutionLease leaseCopy;
        bool hasJob = false;

        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] {
                return stopping_.load() || (!paused_ && asyncInFlight_ == 0 && !queue_.empty());
            });

            if (stopping_.load())
                break;

            if (!paused_ && asyncInFlight_ == 0 && !queue_.empty())
            {
                job = std::move(queue_.front());
                queue_.pop_front();
                leaseCopy = lease_;
                hasJob = true;
                ++inFlight_;
            }
        }

        if (hasJob)
        {
            if (leaseCopy.isValid())
            {
                if (job.kind == RenderJob::Kind::Stage1Render && job.renderCache != nullptr)
                {
                    RenderCache::PendingJob pendingJob;
                    if (job.renderCache->getNextPendingJob(pendingJob))
                    {
                        job.startSeconds = pendingJob.startSeconds;
                        job.startSample = pendingJob.startSample;
                        job.endSampleExclusive = pendingJob.endSampleExclusive;
                        job.targetRevision = pendingJob.targetRevision;
                        leaseCopy.renderJobCallback(job);
                    }
                }
                else if (job.kind == RenderJob::Kind::Stage2Rebuild)
                {
                    leaseCopy.renderJobCallback(job);
                }
            }
            // Lease invalid: job was popped but cannot execute. Decrement to prevent leak.

            {
                std::lock_guard<std::mutex> lk(mutex_);
                --inFlight_;
            }
            cv_.notify_all(); // 唤醒 drain()/detachExecutionLease() 等 inFlight_ 归零的等待者
        }
    }
}

} // namespace OpenTune
