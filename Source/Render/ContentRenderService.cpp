#include "ContentRenderService.h"

namespace OpenTune {

ContentRenderService::ContentRenderService() = default;

ContentRenderService::~ContentRenderService() = default;

// ========================================
// PlaybackSource facade
// ========================================

void ContentRenderService::publishPlaybackSource(ContentKey key, PlaybackReadSource source)
{
    playbackSources_.publish(key, source);
}

bool ContentRenderService::getPlaybackReadSource(ContentKey key, PlaybackReadSource& out) const
{
    return playbackSources_.get(key, out);
}

void ContentRenderService::removePlaybackSource(ContentKey key)
{
    playbackSources_.remove(key);
}

// ========================================
// RenderCache facade
// ========================================

std::shared_ptr<RenderCache> ContentRenderService::getOrCreateRenderCache(ContentKey key)
{
    return renderCaches_.getOrCreate(key);
}

std::shared_ptr<RenderCache> ContentRenderService::getRenderCache(ContentKey key) const
{
    return renderCaches_.get(key);
}

void ContentRenderService::removeRenderCache(ContentKey key)
{
    renderCaches_.remove(key);
}

// ========================================
// RenderWorker facade
// ========================================

void ContentRenderService::attachExecutionLease(ExecutionLease lease)
{
    renderWorker_.attachExecutionLease(lease);
}

void ContentRenderService::detachExecutionLease(void* owner)
{
    renderWorker_.detachExecutionLease(owner);
}

bool ContentRenderService::hasActiveLease(void* /*owner*/) const noexcept
{
    // RenderWorker 不公开 lease 状态；Phase 0.8 保留兼容性
    return false;
}

void ContentRenderService::enqueueRender(RenderJob job)
{
    renderWorker_.enqueue(std::move(job));
}

bool ContentRenderService::hasPendingJobs() const
{
    return renderWorker_.hasPendingJobs();
}

void ContentRenderService::notifyRenderWorker()
{
    // 不再需要手动通知：enqueue 和 worker loop 的 predicate wait 自动处理唤醒。
    // Phase 0.7 时调用方会移除对 notifyRenderWorker 的调用。
}

void ContentRenderService::pauseRenderWorker()
{
    renderWorker_.pause();
}

void ContentRenderService::resumeRenderWorker()
{
    renderWorker_.resume();
}

void ContentRenderService::drainRenderWorker()
{
    renderWorker_.drain();
}

// ========================================
// Stretcher facade
// ========================================

SoundTouchStretcher* ContentRenderService::getStretcher(ContentKey key, double sampleRate, int channels)
{
    return stretchers_.getOrCreate(key, sampleRate, channels);
}

void ContentRenderService::removeStretcher(ContentKey key)
{
    stretchers_.remove(key);
}

// ========================================
// Utility
// ========================================

void ContentRenderService::clearAll()
{
    playbackSources_.clear();
    renderCaches_.clear();
    stretchers_.clear();
    timeStretchCache_.clear();
    // renderWorker_ queue 由 drain() 控制，不清空
}

} // namespace OpenTune
