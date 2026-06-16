#include "ContentRenderService.h"
#include "RenderChunkPlanner.h"
#include "../Inference/SoundTouchStretcher.h"

namespace OpenTune {

namespace {

std::vector<SilentGap> makeRangeLocalSilentGaps(const std::vector<SilentGap>& gaps,
                                                int64_t rangeStartSample,
                                                int64_t rangeEndSampleExclusive)
{
    std::vector<SilentGap> localGaps;
    localGaps.reserve(gaps.size());
    for (const auto& gap : gaps)
    {
        const int64_t clippedStart = juce::jmax(gap.startSample, rangeStartSample);
        const int64_t clippedEnd = juce::jmin(gap.endSampleExclusive, rangeEndSampleExclusive);
        if (clippedEnd <= clippedStart)
            continue;

        SilentGap localGap = gap;
        localGap.startSample = clippedStart - rangeStartSample;
        localGap.endSampleExclusive = clippedEnd - rangeStartSample;
        localGaps.push_back(localGap);
    }
    return localGaps;
}

} // namespace

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
    const int64_t rangeSampleCount = job.endSampleExclusive - job.startSample;
    if (rangeSampleCount <= 0) return;

    const int hopSize = 512;
    auto localGaps = makeRangeLocalSilentGaps(job.silentGaps, job.startSample, job.endSampleExclusive);
    auto boundaries = RenderChunkPlanner::buildChunkBoundariesFromSilentGaps(
        rangeSampleCount,
        localGaps,
        hopSize);

    const double sampleRate = job.audioSampleRate;

    if (boundaries.size() > 2)
    {
        for (size_t i = 0; i + 1 < boundaries.size(); ++i)
        {
            RenderJob subJob = job;
            subJob.startSample = job.startSample + boundaries[i];
            subJob.endSampleExclusive = job.startSample + boundaries[i + 1];
            subJob.startSeconds = static_cast<double>(subJob.startSample) / sampleRate;
            subJob.endSeconds = static_cast<double>(subJob.endSampleExclusive) / sampleRate;

            subJob.renderCache->requestRenderPending(
                subJob.startSeconds, subJob.endSeconds,
                subJob.startSample, subJob.endSampleExclusive);

            renderWorker_.enqueue(std::move(subJob));
        }
    }
    else
    {
        job.renderCache->requestRenderPending(
            job.startSeconds, job.endSeconds,
            job.startSample, job.endSampleExclusive);

        renderWorker_.enqueue(std::move(job));
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
