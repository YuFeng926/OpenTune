#include "RenderWorker.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include "../Utils/AppLogger.h"
#include <algorithm>
#include <iterator>
#include <set>

namespace OpenTune {

RenderWorker::RenderWorker()
{
    asyncControl_ = std::make_shared<AsyncState::Control>();
    asyncControl_->workerCv = &cv_;
    thread_ = std::thread([this] { loop(); });
}

RenderWorker::~RenderWorker()
{
    stop();
    {
        std::lock_guard<std::mutex> lk(asyncControl_->mutex);
        asyncControl_->workerCv = nullptr;
    }
}

void RenderWorker::stop()
{
    if (!thread_.joinable())
    {
        waitAsyncIdle();
        return;
    }

    if (thread_.get_id() == std::this_thread::get_id())
    {
        AppLogger::error("[RenderWorker] self-join rejected; hard failure");
        std::terminate();
    }

    stopping_.store(true);
    {
        std::lock_guard<std::mutex> lk(asyncControl_->mutex);
        asyncControl_->closed = true;
    }
    std::deque<RenderJob> discarded;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        discarded = std::move(queue_);
    }
    for (const auto& job : discarded)
        if (job.kind == RenderJob::Kind::Stage1Render && job.renderCache != nullptr)
            job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision);
    cv_.notify_all();
    thread_.join();
    waitAsyncIdle();
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
    {
        std::lock_guard<std::mutex> lk(asyncControl_->mutex);
        asyncControl_->closed = false;
    }
    resume();
}

void RenderWorker::detachExecutionLease(void* owner)
{
    std::deque<RenderJob> discardedJobs;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (lease_.owner != owner)
            return;
        lease_ = RenderExecutionLease{};
        discardedJobs = std::move(queue_);
    }
    {
        std::lock_guard<std::mutex> lk(asyncControl_->mutex);
        asyncControl_->closed = true;
    }
    cv_.notify_all();
    for (const auto& job : discardedJobs)
    {
        if (job.kind == RenderJob::Kind::Stage1Render && job.renderCache != nullptr)
            job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision);
    }

    std::unique_lock<std::mutex> lk(mutex_);
    cv_.wait(lk, [this] {
        return inFlight_ == 0 && asyncControl_->count.load(std::memory_order_acquire) == 0;
    });
}

// ============================================================
// 渲染队列
// ============================================================

void RenderWorker::reconcileAndSyncStage1Queue(const RenderJob& templateJob,
                                               const std::function<void()>& reconcile)
{
    std::vector<RenderCache::PendingJob> rejectedJobs;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // 与 worker loop 的 claim 共用同一临界区：reconcile 产生的 revision
        // 不可能被旧 queued job 在新 snapshot 同步前 claim。
        reconcile();
        syncStage1QueueLocked(templateJob, rejectedJobs);
    }
    for (const auto& failure : rejectedJobs)
        templateJob.renderCache->completeChunkRenderFailure(
            failure.startSample, failure.targetRevision);
    cv_.notify_all();
}

void RenderWorker::syncStage1QueueLocked(
    const RenderJob& templateJob,
    std::vector<RenderCache::PendingJob>& rejectedJobs)
{
    jassert(templateJob.kind == RenderJob::Kind::Stage1Render);
    jassert(templateJob.renderCache != nullptr);

    const auto* cache = templateJob.renderCache.get();

    const auto pendingJobs = templateJob.renderCache->getPendingJobs();
    std::set<int64_t> desired;
    for (const auto& pending : pendingJobs)
        desired.insert(pending.startSample);

    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
        [cache, &desired](const RenderJob& queued) {
            return queued.kind == RenderJob::Kind::Stage1Render
                && queued.renderCache.get() == cache
                && desired.count(queued.queuedChunkStartSample) == 0;
        }), queue_.end());

    for (const auto& pending : pendingJobs)
    {
        const auto startSample = pending.startSample;
        const bool alreadyQueued = std::any_of(queue_.begin(), queue_.end(),
            [cache, &pending](const RenderJob& queued) {
                return queued.kind == RenderJob::Kind::Stage1Render
                    && queued.renderCache.get() == cache
                    && queued.queuedChunkStartSample == pending.startSample
                    && queued.startSample == pending.startSample
                    && queued.endSampleExclusive == pending.endSampleExclusive
                    && queued.targetRevision == pending.targetRevision;
            });
        if (alreadyQueued)
        {
            auto queuedIt = std::find_if(queue_.begin(), queue_.end(),
                [cache, startSample](const RenderJob& queued) {
                    return queued.kind == RenderJob::Kind::Stage1Render
                        && queued.renderCache.get() == cache
                        && queued.queuedChunkStartSample == startSample;
                });
            queuedIt->contentSnapshot = templateJob.contentSnapshot;
            queuedIt->audioBuffer = templateJob.audioBuffer;
            queuedIt->audioSampleRate = templateJob.audioSampleRate;
            continue;
        }

        RenderJob queued = templateJob;
        queued.startSample = pending.startSample;
        queued.endSampleExclusive = pending.endSampleExclusive;
        queued.targetRevision = pending.targetRevision;
        queued.queuedChunkStartSample = startSample;
        if (!enqueueLocked(std::move(queued)))
            rejectedJobs.push_back(pending);
    }
}

void RenderWorker::discardStage1Queue(RenderCache* cache)
{
    std::deque<RenderJob> discarded;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = std::stable_partition(queue_.begin(), queue_.end(),
            [cache](const RenderJob& queued) {
                return !(queued.kind == RenderJob::Kind::Stage1Render
                    && queued.renderCache.get() == cache);
            });
        discarded.insert(discarded.end(), std::make_move_iterator(it),
                         std::make_move_iterator(queue_.end()));
        queue_.erase(it, queue_.end());
    }
    for (const auto& job : discarded)
        if (job.renderCache != nullptr)
            job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision);
    cv_.notify_all();
}

void RenderWorker::discardAllStage1Queue()
{
    std::deque<RenderJob> discarded;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = std::stable_partition(queue_.begin(), queue_.end(),
            [](const RenderJob& queued) {
                return queued.kind != RenderJob::Kind::Stage1Render;
            });
        discarded.insert(discarded.end(), std::make_move_iterator(it),
                         std::make_move_iterator(queue_.end()));
        queue_.erase(it, queue_.end());
    }
    for (const auto& job : discarded)
        if (job.renderCache != nullptr)
            job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision);
    cv_.notify_all();
}

bool RenderWorker::enqueue(RenderJob job)
{
    std::shared_ptr<RenderCache> failureCache;
    int64_t failureStartSample = 0;
    uint64_t failureRevision = 0;
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (job.kind == RenderJob::Kind::Stage1Render)
        {
            jassert(job.renderCache != nullptr && job.queuedChunkStartSample >= 0);
            if (job.renderCache == nullptr || job.queuedChunkStartSample < 0)
            {
                failureCache = job.renderCache;
                failureStartSample = job.startSample;
                failureRevision = job.targetRevision;
            }
            else
            {
                if (!lease_.isValid())
                {
                    failureCache = job.renderCache;
                    failureStartSample = job.startSample;
                    failureRevision = job.targetRevision;
                }
                else
                {
                    const auto* cache = job.renderCache.get();
                    const bool alreadyQueued = std::any_of(queue_.begin(), queue_.end(),
                        [cache, &job](const RenderJob& queued) {
                            return queued.kind == RenderJob::Kind::Stage1Render
                                && queued.renderCache.get() == cache
                                && queued.queuedChunkStartSample == job.queuedChunkStartSample;
                        });
                    if (alreadyQueued)
                        return true;
                }
            }
        }

        if (failureCache == nullptr)
        {
            if (job.kind == RenderJob::Kind::Stage1Render)
            {
                failureCache = job.renderCache;
                failureStartSample = job.startSample;
                failureRevision = job.targetRevision;
            }
            accepted = enqueueLocked(std::move(job));
            if (accepted)
                failureCache.reset();
        }
    }
    if (failureCache != nullptr)
        failureCache->completeChunkRenderFailure(failureStartSample, failureRevision);
    if (!accepted)
        return false;
    cv_.notify_one();
    return true;
}

bool RenderWorker::requeueStage1Chunk(const RenderJob& job)
{
    if (job.kind != RenderJob::Kind::Stage1Render
        || job.renderCache == nullptr
        || job.queuedChunkStartSample < 0)
        return false;

    bool enqueued = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        RenderJob queued = job;
        queued.queuedChunkStartSample = job.startSample;
        enqueued = enqueueLocked(std::move(queued), true);
    }
    if (enqueued)
        cv_.notify_one();
    return enqueued;
}

bool RenderWorker::enqueueLocked(RenderJob job, bool requeueRunningChunk)
{
    if (!lease_.isValid() || queue_.size() >= RenderWorker::kMaxQueueDepth)
        return false;
    if (requeueRunningChunk
        && !job.renderCache->requeueRunningChunk(job.startSample, job.targetRevision))
        return false;
    if (job.kind != RenderJob::Kind::Stage1Render)
    {
        queue_.push_back(std::move(job));
        return true;
    }

    const auto* cache = job.renderCache.get();
    const auto insertIt = std::find_if(queue_.begin(), queue_.end(),
        [cache, &job](const RenderJob& queued) {
            return queued.kind == RenderJob::Kind::Stage1Render
                && queued.renderCache.get() == cache
                && queued.queuedChunkStartSample > job.queuedChunkStartSample;
        });
    queue_.insert(insertIt, std::move(job));
    return true;
}

std::shared_ptr<RenderWorker::AsyncState> RenderWorker::beginAsyncJob()
{
    auto state = std::make_shared<AsyncState>();
    state->control = asyncControl_;
    std::lock_guard<std::mutex> lk(state->control->mutex);
    if (state->control->closed)
        return {};
    state->control->count.fetch_add(1, std::memory_order_acq_rel);
    return state;
}

void RenderWorker::completeAsyncJob(const std::shared_ptr<AsyncState>& state) noexcept
{
    if (state == nullptr)
        return;
    if (state->control == nullptr)
        return;
    std::condition_variable* workerCv = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->control->mutex);
        auto count = state->control->count.load(std::memory_order_acquire);
        if (count <= 0)
            return;
        state->control->count.store(count - 1, std::memory_order_release);
        workerCv = state->control->workerCv;
    }
    if (workerCv != nullptr)
        workerCv->notify_all();
    state->control->cv.notify_all();
}

bool RenderWorker::isAsyncJobClosed(const std::shared_ptr<AsyncState>& state) noexcept
{
    if (state == nullptr || state->control == nullptr)
        return true;
    std::lock_guard<std::mutex> lk(state->control->mutex);
    return state->control->closed;
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
    cv_.wait(lk, [this] {
        return queue_.empty() && inFlight_ == 0
            && asyncControl_->count.load(std::memory_order_acquire) == 0;
    });
}

void RenderWorker::waitAsyncIdle()
{
    std::unique_lock<std::mutex> lk(asyncControl_->mutex);
    asyncControl_->cv.wait(lk, [this] {
        return asyncControl_->count.load(std::memory_order_acquire) == 0;
    });
}

std::size_t RenderWorker::queueDepth() const noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
}

int RenderWorker::inFlight() const noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    return inFlight_;
}

int RenderWorker::asyncInFlight() const noexcept
{
    return asyncControl_->count.load(std::memory_order_acquire);
}

bool RenderWorker::isPaused() const noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    return paused_;
}

bool RenderWorker::isJoinable() const noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    return thread_.joinable();
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
                return stopping_.load()
                    || (!paused_
                        && asyncControl_->count.load(std::memory_order_acquire) == 0
                        && !queue_.empty());
            });

            if (stopping_.load())
                break;

            if (!paused_
                && asyncControl_->count.load(std::memory_order_acquire) == 0
                && !queue_.empty())
            {
                job = std::move(queue_.front());
                queue_.pop_front();
                leaseCopy = lease_;
                ++inFlight_;

                if (leaseCopy.isValid())
                {
                    if (job.kind == RenderJob::Kind::Stage1Render && job.renderCache != nullptr)
                    {
                        RenderCache::PendingJob expectedJob;
                        expectedJob.startSample = job.startSample;
                        expectedJob.endSampleExclusive = job.endSampleExclusive;
                        expectedJob.targetRevision = job.targetRevision;
                        if (job.renderCache->claimPendingJob(expectedJob))
                        {
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

        struct InFlightGuard final
        {
            RenderWorker& worker;
            ~InFlightGuard() noexcept
            {
                {
                    std::lock_guard<std::mutex> lk(worker.mutex_);
                    --worker.inFlight_;
                }
                worker.cv_.notify_all();
            }
        } guard{*this};

        if (hasJob)
        {
            try
            {
                leaseCopy.renderJobCallback(job);
            }
            catch (const std::exception& e)
            {
                AppLogger::error("[RenderWorker] render callback threw: " + juce::String(e.what()));
                if (job.kind == RenderJob::Kind::Stage1Render && job.renderCache != nullptr)
                    job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision);
            }
            catch (...)
            {
                AppLogger::error("[RenderWorker] render callback threw unknown exception");
                if (job.kind == RenderJob::Kind::Stage1Render && job.renderCache != nullptr)
                    job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision);
            }
        }
    }
}

} // namespace OpenTune
