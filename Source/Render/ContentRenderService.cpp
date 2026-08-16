#include "ContentRenderService.h"

#include "RenderChunkPlanner.h"
#include "../Inference/SoundTouchStretcher.h"
#include "../Utils/TimeCoordinate.h"

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

void ContentRenderService::enqueueRender(RenderJob job, const std::vector<Note>& notes)
{
    if (job.kind != RenderJob::Kind::Stage1Render
        || job.renderCache == nullptr
        || job.audioBuffer == nullptr
        || job.endSampleExclusive <= job.startSample)
        return;

    // 即时提取 active-EQ Note 的样本保护范围（只保留 start/end，不复制
    // EqSettings），仅本调用栈内读取，不存储。用现有 TimeCoordinate floor/ceil
    // 表达 Note 覆盖范围，无新取整策略。Stage1 输入已固定 44.1kHz（CRS 播放源
    // canonical），Note 边界统一按 RenderCache::kSampleRate 换算，无第二采样率轴。
    std::vector<RenderChunkPlanner::ProtectedRange> protectedRanges;
    for (const auto& note : notes)
    {
        if (!note.eq.has_value() || !note.eq->active)
            continue;
        const int64_t protectedStart = TimeCoordinate::secondsToSamplesFloor(
            note.startTime, RenderCache::kSampleRate);
        const int64_t protectedEnd = TimeCoordinate::secondsToSamplesCeil(
            note.endTime, RenderCache::kSampleRate);
        if (protectedEnd <= protectedStart)
            continue;
        protectedRanges.push_back({protectedStart, protectedEnd});
    }

    // 完整计划：请求 [0, contentSampleCount) 得到全内容 chunk 几何（升序无重叠）
    const int64_t contentSampleCount = job.audioBuffer->getNumSamples();
    const auto chunkRanges = RenderChunkPlanner::selectChunksIntersectingRange(
        contentSampleCount,
        job.silentGaps,
        protectedRanges,
        0,
        contentSampleCount,
        RenderChunkPlanner::kRenderHopSize);
    if (chunkRanges.empty())
        return;

    std::vector<RenderCache::PlannedChunk> fullPlan;
    fullPlan.reserve(chunkRanges.size());
    for (const auto& range : chunkRanges)
        fullPlan.push_back({range.startSample, range.endSampleExclusive});

    // A new Stage1 batch supersedes every derived Stage2 result once, not per chunk.
    timeStretchCache_.invalidate(job.contentKey);

    // 一次原子 reconcile 完整计划与局部 request；返回本次需投递的 worker job
    // token 数（worker 执行时经 getNextPendingJob 拉取实际 span/revision）。
    const std::size_t jobTokenCount = job.renderCache->reconcileFullPlanAndRequest(
        fullPlan, job.startSample, job.endSampleExclusive);

    for (std::size_t i = 0; i < jobTokenCount; ++i)
    {
        RenderJob subJob = job;
        renderWorker_.enqueue(std::move(subJob));
    }
}

void ContentRenderService::requeueRenderChunk(const RenderJob& job)
{
    jassert(job.kind == RenderJob::Kind::Stage1Render && job.renderCache != nullptr);

    // 只回退状态机：成功回退后仅投递一个 job token，worker 下轮从 PendingJob
    // 重拉 span/revision，owner 回调抓当前 snapshot。不重算几何、不 bump desired。
    const bool requeued = job.renderCache->requeueRunningChunk(job.startSeconds, job.targetRevision);
    if (!requeued)
        return;

    RenderJob subJob = job;
    renderWorker_.enqueue(std::move(subJob));
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
