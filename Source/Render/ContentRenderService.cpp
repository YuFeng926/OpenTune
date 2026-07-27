#include "ContentRenderService.h"
#include "RenderChunkPlanner.h"
#include "Stage2TimeStretchRebuilder.h"
#include "../Inference/SoundTouchStretcher.h"

namespace OpenTune {

ContentRenderService::ContentRenderService() = default;

ContentRenderService::~ContentRenderService() = default;

void ContentRenderService::requestStage2Rebuild(Stage2Request request,
                                                  std::shared_ptr<const EditableContentSnapshot> ownerSnap)
{
    // Stage2 rebuild runs synchronously on the caller's thread.  Callers that need
    // off-thread / queued execution (e.g. the processor's Stage2Worker) must wrap this
    // entry in their own threading primitive.
    Stage2TimeStretchRebuilder::Request rebuildRequest;
    rebuildRequest.contentKey = request.contentKey;
    rebuildRequest.pitchRevision = request.pitchRevision;
    rebuildRequest.pitchShiftRevision = request.pitchShiftRevision;
    rebuildRequest.timeGridRevision = request.timeGridRevision;

    Stage2TimeStretchRebuilder::rebuild(*this, rebuildRequest, std::move(ownerSnap));
}

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
    renderWorker_.attachExecutionLease(std::move(lease));
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

void ContentRenderService::preparePlaybackSampleRate(double targetSr)
{
    if (targetSr <= 0.0) return;

    // 1. Playback sources — dry audio 准备（使用 publisher 自有 resampler）
    playbackSources_.setPlaybackSampleRate(targetSr);

    // 2. RenderCaches — chunk 准备（使用各 cache 自有 resampler）
    renderCaches_.preparePlaybackSampleRate(targetSr);

    // 3. TimeStretchCache — 整数切片准备（使用自有 resampler）
    timeStretchCache_.prepareForPlaybackSampleRate(targetSr);
}

} // namespace OpenTune
