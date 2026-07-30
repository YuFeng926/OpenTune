#include "ContentRenderService.h"

#include "RenderChunkPlanner.h"
#include "../Inference/SoundTouchStretcher.h"

namespace OpenTune {

ContentRenderService::ContentRenderService() = default;
ContentRenderService::~ContentRenderService() = default;

bool ContentRenderService::enqueueStage2RebuildWhenCanonicalSettled(Stage2Request request)
{
    if (!request.contentKey.isValid())
        return false;

    auto renderCache = getRenderCache(request.contentKey);
    if (renderCache == nullptr || !renderCache->isCanonicalSettled())
        return false;

    RenderJob job;
    job.kind = RenderJob::Kind::Stage2Rebuild;
    job.contentKey = request.contentKey;
    job.pitchRevision = request.pitchRevision;
    job.pitchShiftRevision = request.pitchShiftRevision;
    job.timeGridRevision = request.timeGridRevision;
    renderWorker_.enqueue(std::move(job));
    return true;
}

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

void ContentRenderService::attachExecutionLease(ExecutionLease lease)
{
    renderWorker_.attachExecutionLease(std::move(lease));
}

void ContentRenderService::detachExecutionLease(void* owner)
{
    renderWorker_.detachExecutionLease(owner);
}

void ContentRenderService::enqueueRender(RenderJob job)
{
    if (job.kind != RenderJob::Kind::Stage1Render
        || job.renderCache == nullptr
        || job.audioBuffer == nullptr
        || job.endSampleExclusive <= job.startSample)
        return;

    const double sampleRate = job.audioSampleRate;
    if (sampleRate <= 0.0)
        return;

    constexpr int kHopSize = 512;
    const auto chunks = RenderChunkPlanner::selectChunksIntersectingRange(
        job.audioBuffer->getNumSamples(),
        job.silentGaps,
        job.startSample,
        job.endSampleExclusive,
        kHopSize);
    if (chunks.empty())
        return;

    // A new Stage1 batch supersedes every derived Stage2 result once, not per chunk.
    timeStretchCache_.invalidate(job.contentKey);

    for (const auto& chunk : chunks)
    {
        RenderJob subJob = job;
        subJob.renderCache->requestRenderPending(chunk.startSample, chunk.endSampleExclusive);
        renderWorker_.enqueue(std::move(subJob));
    }
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

SoundTouchStretcher* ContentRenderService::getStretcher(ContentKey key, double sampleRate, int channels)
{
    return stretchers_.getOrCreate(key, sampleRate, channels);
}

void ContentRenderService::removeStretcher(ContentKey key)
{
    stretchers_.remove(key);
}

void ContentRenderService::clearAll()
{
    playbackSources_.clear();
    renderCaches_.clear();
    stretchers_.clear();
    timeStretchCache_.clear();
}

void ContentRenderService::preparePlaybackSampleRate(double targetSr)
{
    if (targetSr <= 0.0)
        return;

    playbackSources_.setPlaybackSampleRate(targetSr);
    renderCaches_.preparePlaybackSampleRate(targetSr);
    timeStretchCache_.prepareForPlaybackSampleRate(targetSr);
}

} // namespace OpenTune
