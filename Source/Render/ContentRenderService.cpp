#include "ContentRenderService.h"
#include "../Inference/SoundTouchStretcher.h"
#include <algorithm>

namespace OpenTune {

ContentRenderService::ContentRenderService()
{
    startRenderWorker();
}

ContentRenderService::~ContentRenderService()
{
    stopRenderWorker();
}

// ============================================================
// 播放源发布
// ============================================================

void ContentRenderService::publishPlaybackSource(ContentKey key, PlaybackReadSource source)
{
    juce::ScopedWriteLock wl(lock_);
    source.contentKey = key;
    playbackSources_[key] = std::move(source);
    rebuildPlaybackSourceCache();
}

bool ContentRenderService::getPlaybackReadSource(ContentKey key, PlaybackReadSource& out) const
{
    auto cache = std::atomic_load(&playbackSourceCache_);
    if (!cache) return false;
    auto it = cache->find(key);
    if (it == cache->end()) return false;
    out = it->second;
    return true;
}

void ContentRenderService::removePlaybackSource(ContentKey key)
{
    juce::ScopedWriteLock wl(lock_);
    playbackSources_.erase(key);
    rebuildPlaybackSourceCache();
}

void ContentRenderService::clearAll()
{
    juce::ScopedWriteLock wl(lock_);
    playbackSources_.clear();
    renderCaches_.clear();
    rebuildPlaybackSourceCache();
}

void ContentRenderService::rebuildPlaybackSourceCache()
{
    auto cache = std::make_shared<std::map<ContentKey, PlaybackReadSource>>(playbackSources_);
    std::atomic_store(&playbackSourceCache_,
                      std::static_pointer_cast<const std::map<ContentKey, PlaybackReadSource>>(cache));
}

// ============================================================
// RenderCache 管理
// ============================================================

std::shared_ptr<RenderCache> ContentRenderService::getOrCreateRenderCache(ContentKey key)
{
    juce::ScopedWriteLock wl(lock_);
    auto it = renderCaches_.find(key);
    if (it != renderCaches_.end())
        return it->second;
    auto cache = std::make_shared<RenderCache>();
    renderCaches_[key] = cache;
    return cache;
}

std::shared_ptr<RenderCache> ContentRenderService::getRenderCache(ContentKey key) const
{
    juce::ScopedReadLock rl(lock_);
    auto it = renderCaches_.find(key);
    return (it != renderCaches_.end()) ? it->second : nullptr;
}

void ContentRenderService::removeRenderCache(ContentKey key)
{
    juce::ScopedWriteLock wl(lock_);
    renderCaches_.erase(key);
}

// ============================================================
// 执行租约
// ============================================================

void ContentRenderService::attachExecutionLease(ExecutionLease lease)
{
    drainRenderWorker();
    lease_ = std::move(lease);
    resumeRenderWorker();
}

void ContentRenderService::detachExecutionLease(void* leaseOwner)
{
    if (lease_.leaseOwner != leaseOwner) return;
    drainRenderWorker();
    lease_ = ExecutionLease{};
}

// ============================================================
// 渲染队列
// ============================================================

void ContentRenderService::enqueueRender(PendingRenderJob job)
{
    PendingRenderEntry entry;
    entry.contentKey = job.contentKey;
    entry.startSeconds = job.startSeconds;
    entry.endSeconds = job.endSeconds;
    entry.startSample = job.startSample;
    entry.endSampleExclusive = job.endSampleExclusive;
    entry.pitchRevision = job.pitchRevision;
    entry.timeGridRevision = job.timeGridRevision;
    entry.pitchCurve = job.pitchCurve;

    {
        std::lock_guard<std::mutex> lk(renderQueueMutex_);
        pendingRenderQueue_.push_back(entry);
    }
    notifyRenderWorker();
}

bool ContentRenderService::pullNextPendingRenderJob(PendingRenderJob& out)
{
    PendingRenderEntry entry;
    {
        std::lock_guard<std::mutex> lk(renderQueueMutex_);
        if (pendingRenderQueue_.empty())
            return false;
        entry = pendingRenderQueue_.front();
        pendingRenderQueue_.pop_front();
    }

    out.contentKey = entry.contentKey;
    out.startSeconds = entry.startSeconds;
    out.endSeconds = entry.endSeconds;
    out.startSample = entry.startSample;
    out.endSampleExclusive = entry.endSampleExclusive;
    out.pitchRevision = entry.pitchRevision;
    out.timeGridRevision = entry.timeGridRevision;
    out.pitchCurve = entry.pitchCurve;

    // 填充关联的 cache/audio
    PlaybackReadSource src;
    if (getPlaybackReadSource(entry.contentKey, src))
    {
        out.renderCache = src.renderCache;
        out.audioBuffer = src.audioBuffer;
    }
    return true;
}

bool ContentRenderService::hasPendingJobs() const
{
    std::lock_guard<std::mutex> lk(renderQueueMutex_);
    return !pendingRenderQueue_.empty();
}

// ============================================================
// 渲染工作器
// ============================================================

void ContentRenderService::notifyRenderWorker()
{
    renderWorkerCv_.notify_one();
}

void ContentRenderService::pauseRenderWorker()
{
    renderPaused_.store(true, std::memory_order_release);
    while (renderJobsInFlight_.load(std::memory_order_acquire) > 0)
        std::this_thread::yield();
}

void ContentRenderService::resumeRenderWorker()
{
    renderPaused_.store(false, std::memory_order_release);
    renderWorkerCv_.notify_one();
}

void ContentRenderService::drainRenderWorker()
{
    pauseRenderWorker();
    std::lock_guard<std::mutex> lk(renderQueueMutex_);
    pendingRenderQueue_.clear();
}

void ContentRenderService::startRenderWorker()
{
    renderWorkerShouldStop_.store(false);
    renderPaused_.store(false);
    renderWorkerThread_ = std::thread([this] { renderWorkerLoop(); });
}

void ContentRenderService::stopRenderWorker()
{
    renderWorkerShouldStop_.store(true);
    notifyRenderWorker();
    if (renderWorkerThread_.joinable())
        renderWorkerThread_.join();
}

void ContentRenderService::renderWorkerLoop()
{
    while (!renderWorkerShouldStop_.load(std::memory_order_acquire))
    {
        PendingRenderEntry entry;
        {
            std::lock_guard<std::mutex> lk(renderQueueMutex_);
            if (pendingRenderQueue_.empty())
            {
                std::unique_lock<std::mutex> wlk(renderWorkerMutex_);
                renderWorkerCv_.wait_for(wlk, std::chrono::milliseconds(100));
                continue;
            }
            entry = pendingRenderQueue_.front();
            pendingRenderQueue_.pop_front();
        }

        if (renderPaused_.load(std::memory_order_acquire))
            continue;

        renderJobsInFlight_.fetch_add(1, std::memory_order_release);

        if (lease_.isValid())
        {
            PendingRenderJob job;
            job.contentKey = entry.contentKey;
            job.startSeconds = entry.startSeconds;
            job.endSeconds = entry.endSeconds;
            job.startSample = entry.startSample;
            job.endSampleExclusive = entry.endSampleExclusive;
            job.pitchRevision = entry.pitchRevision;
            job.timeGridRevision = entry.timeGridRevision;
            job.pitchCurve = entry.pitchCurve;

            // Fill render cache and audio buffer from playback source
            PlaybackReadSource src;
            if (getPlaybackReadSource(entry.contentKey, src))
            {
                job.renderCache = src.renderCache;
                job.audioBuffer = src.audioBuffer;
            }

            lease_.renderJobCallback(job);
        }

        renderJobsInFlight_.fetch_sub(1, std::memory_order_release);
    }
}

// ============================================================
// Stretcher 管理
// ============================================================

SoundTouchStretcher* ContentRenderService::getStretcher(ContentKey key, double sampleRate, int channels)
{
    juce::ScopedWriteLock wl(lock_);
    for (auto& e : stretchers_)
        if (e.key == key)
            return e.stretcher.get();
    stretchers_.push_back({key, std::make_unique<SoundTouchStretcher>(sampleRate, channels)});
    return stretchers_.back().stretcher.get();
}

void ContentRenderService::removeStretcher(ContentKey key)
{
    juce::ScopedWriteLock wl(lock_);
    stretchers_.erase(
        std::remove_if(stretchers_.begin(), stretchers_.end(),
                       [&](const auto& e) { return e.key == key; }),
        stretchers_.end());
}

} // namespace OpenTune
