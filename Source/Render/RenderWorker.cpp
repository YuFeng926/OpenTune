#include "RenderWorker.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include <algorithm>
#include <set>

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

void RenderWorker::syncStage1Queue(const RenderJob& templateJob)
{
    jassert(templateJob.kind == RenderJob::Kind::Stage1Render);
    jassert(templateJob.renderCache != nullptr);

    const auto* cache = templateJob.renderCache.get();

    {
        std::lock_guard<std::mutex> lk(mutex_);
        const auto pendingChunkStarts = templateJob.renderCache->getPendingChunkStarts();
        const std::set<int64_t> desired(pendingChunkStarts.begin(), pendingChunkStarts.end());

        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
            [cache, &desired](const RenderJob& queued) {
                return queued.kind == RenderJob::Kind::Stage1Render
                    && queued.renderCache.get() == cache
                    && desired.count(queued.queuedChunkStartSample) == 0;
            }), queue_.end());

        for (const auto startSample : pendingChunkStarts)
        {
            const bool alreadyQueued = std::any_of(queue_.begin(), queue_.end(),
                [cache, startSample](const RenderJob& queued) {
                    return queued.kind == RenderJob::Kind::Stage1Render
                        && queued.renderCache.get() == cache
                        && queued.queuedChunkStartSample == startSample;
                });
            if (alreadyQueued)
                continue;

            RenderJob queued = templateJob;
            queued.queuedChunkStartSample = startSample;
            enqueueLocked(std::move(queued));
        }
    }
    cv_.notify_all();
}

void RenderWorker::discardStage1Queue(RenderCache* cache)
{
    std::lock_guard<std::mutex> lk(mutex_);
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
        [cache](const RenderJob& queued) {
            return queued.kind == RenderJob::Kind::Stage1Render
                && queued.renderCache.get() == cache;
        }), queue_.end());
    cv_.notify_all();
}

void RenderWorker::discardAllStage1Queue()
{
    std::lock_guard<std::mutex> lk(mutex_);
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
        [](const RenderJob& queued) {
            return queued.kind == RenderJob::Kind::Stage1Render;
        }), queue_.end());
    cv_.notify_all();
}

void RenderWorker::enqueue(RenderJob job)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (job.kind == RenderJob::Kind::Stage1Render)
        {
            jassert(job.renderCache != nullptr && job.queuedChunkStartSample >= 0);
            if (job.renderCache == nullptr || job.queuedChunkStartSample < 0)
                return;

            const auto* cache = job.renderCache.get();
            const bool alreadyQueued = std::any_of(queue_.begin(), queue_.end(),
                [cache, &job](const RenderJob& queued) {
                    return queued.kind == RenderJob::Kind::Stage1Render
                        && queued.renderCache.get() == cache
                        && queued.queuedChunkStartSample == job.queuedChunkStartSample;
                });
            if (alreadyQueued)
                return;
        }

        enqueueLocked(std::move(job));
    }
    cv_.notify_one();
}

void RenderWorker::enqueueLocked(RenderJob job)
{
    if (job.kind != RenderJob::Kind::Stage1Render)
    {
        queue_.push_back(std::move(job));
        return;
    }

    const auto* cache = job.renderCache.get();
    const auto insertIt = std::find_if(queue_.begin(), queue_.end(),
        [cache, &job](const RenderJob& queued) {
            return queued.kind == RenderJob::Kind::Stage1Render
                && queued.renderCache.get() == cache
                && queued.queuedChunkStartSample > job.queuedChunkStartSample;
        });
    queue_.insert(insertIt, std::move(job));
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
                ++inFlight_;

                if (leaseCopy.isValid())
                {
                    if (job.kind == RenderJob::Kind::Stage1Render && job.renderCache != nullptr)
                    {
                        RenderCache::PendingJob pendingJob;
                        if (job.renderCache->claimPendingJob(
                                job.queuedChunkStartSample, pendingJob))
                        {
                            job.startSeconds = pendingJob.startSeconds;
                            job.startSample = pendingJob.startSample;
                            job.endSampleExclusive = pendingJob.endSampleExclusive;
                            job.targetRevision = pendingJob.targetRevision;
                            hasJob = true;
                        }
                    }
                    else if (job.kind == RenderJob::Kind::Stage2Rebuild)
                    {
                        hasJob = true;
                    }
                }
            }
        }

        if (hasJob)
        {
            leaseCopy.renderJobCallback(job);
        }

        {
            std::lock_guard<std::mutex> lk(mutex_);
            --inFlight_;
        }
        cv_.notify_all(); // 唤醒 drain()/detachExecutionLease() 等 inFlight_ 归零的等待者
    }
}

} // namespace OpenTune
