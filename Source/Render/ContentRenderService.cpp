#include "ContentRenderService.h"
#include "RenderChunkPlanner.h"
#include "../Inference/SoundTouchStretcher.h"

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

void ContentRenderService::enqueueRender(RenderJob job)
{
    if (job.audioBuffer == nullptr || job.endSampleExclusive <= job.startSample) return;

    const int hopSize = 512;
    const double sampleRate = job.audioSampleRate;
    if (sampleRate <= 0.0) return;

    const auto chunks = RenderChunkPlanner::selectChunksIntersectingRange(
        job.audioBuffer->getNumSamples(),
        job.silentGaps,
        job.startSample,
        job.endSampleExclusive,
        hopSize);

    for (const auto& chunk : chunks)
    {
        RenderJob subJob = job;
        subJob.startSample = chunk.startSample;
        subJob.endSampleExclusive = chunk.endSampleExclusive;
        subJob.startSeconds = static_cast<double>(subJob.startSample) / sampleRate;
        subJob.endSeconds = static_cast<double>(subJob.endSampleExclusive) / sampleRate;

        subJob.renderCache->requestRenderPending(
            subJob.startSeconds, subJob.endSeconds,
            subJob.startSample, subJob.endSampleExclusive);

        renderWorker_.enqueue(std::move(subJob));
    }
}

bool ContentRenderService::hasPendingJobs() const
{
    return renderWorker_.hasPendingJobs();
}

void ContentRenderService::beginAsyncRenderJob()
{
    renderWorker_.beginAsyncJob();
}

void ContentRenderService::completeAsyncRenderJob()
{
    renderWorker_.completeAsyncJob();
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
    // renderWorker_ queue �?drain() 控制，不清空
}

} // namespace OpenTune
