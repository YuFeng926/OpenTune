#include "RenderWorker.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include <chrono>
#include <thread>

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
    drain();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (lease_.owner != owner)
            return;
        lease_ = RenderExecutionLease{};
    }
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
    // Wait for in-flight jobs to complete
    while (inFlight_ > 0 || asyncInFlight_ > 0)
    {
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lk.lock();
    }
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
    while (true)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        if (queue_.empty() && inFlight_ == 0 && asyncInFlight_ == 0)
            break;
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
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

            std::lock_guard<std::mutex> lk(mutex_);
            --inFlight_;
        }
    }
}

} // namespace OpenTune
