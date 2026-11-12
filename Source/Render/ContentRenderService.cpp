#include "ContentRenderService.h"

#include "RenderChunkPlanner.h"
#include "Stage2TimeStretchRebuilder.h"
#include "../Inference/SoundTouchStretcher.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include "../Utils/TimeCoordinate.h"

namespace OpenTune {

ContentRenderService::ContentRenderService() = default;
ContentRenderService::~ContentRenderService()
{
    if (backendListenerRegistered_)
        ProcessRenderRuntime::getInstance().unregisterBackendChangeListener(this);
}

bool ContentRenderService::enqueueStage2RebuildWhenCanonicalSettled(
    ContentKey contentKey,
    std::shared_ptr<const EditableContentSnapshot> contentSnapshot,
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
    double audioSampleRate)
{
    if (!contentKey.isValid()
        || contentSnapshot == nullptr
        || contentSnapshot->timeGrid == nullptr
        || contentSnapshot->timeGrid->empty()
        || audioBuffer == nullptr
        || audioSampleRate <= 0.0)
        return false;

    auto renderCache = getRenderCache(contentKey);
    if (renderCache == nullptr || !renderCache->isCanonicalSettled())
        return false;

    RenderJob job;
    job.kind = RenderJob::Kind::Stage2Rebuild;
    job.contentKey = contentKey;
    job.contentSnapshot = std::move(contentSnapshot);
    job.contentRevision = job.contentSnapshot->contentRevision;
    job.audioBuffer = std::move(audioBuffer);
    job.audioSampleRate = audioSampleRate;
    return renderWorker_.enqueue(std::move(job));
}

void ContentRenderService::publishPlaybackSource(ContentKey key, PlaybackReadSource source)
{
    if (!key.isValid()
        || source.contentSnapshot == nullptr
        || source.contentSnapshot->timeGrid == nullptr
        || source.contentSnapshot->timeGrid->empty())
        return;
    source.renderCache->setCanonicalSource(
        source.canonicalBuffer, source.firstCanonicalSample, source.nativeSource);
    playbackSources_.publish(key, source);
    ProcessRenderRuntime::getInstance().renderQueueCapacityAvailable();
}

bool ContentRenderService::republishPlaybackSource(
    ContentKey key,
    std::shared_ptr<const EditableContentSnapshot> contentSnapshot)
{
    if (!contentSnapshot)
        return false;

    PlaybackReadSource current;
    if (!getPlaybackReadSource(key, current)
        || !current.canonicalBuffer
        || current.canonicalSampleRate <= 0.0)
        return false;

    current.contentSnapshot = std::move(contentSnapshot);
    publishPlaybackSource(key, std::move(current));
    return true;
}

bool ContentRenderService::getPlaybackReadSource(ContentKey key, PlaybackReadSource& out) const
{
    return playbackSources_.get(key, out);
}

ContentRenderService::DeferredSourceStatus ContentRenderService::restoreDeferredRenderJob(
    RenderJob& job, uint64_t expectedAudioRevision,
    int expectedAudioSamples, int expectedAudioChannels) const
{
    if (!isCurrentRenderCache(job.contentKey, job.renderCache))
        return DeferredSourceStatus::Stale;

    RenderCache::RunningChunkSnapshot running;
    if (!job.renderCache->getRunningChunk(job.startSample, running)
        || running.startSample != job.startSample
        || running.endSampleExclusive != job.endSampleExclusive
        || running.runningRevision != job.targetRevision)
        return DeferredSourceStatus::Stale;

    PlaybackReadSource source;
    if (!getPlaybackReadSource(job.contentKey, source))
        return DeferredSourceStatus::SourceUnavailable;
    if (source.renderCache != job.renderCache)
        return DeferredSourceStatus::Stale;
    if (source.contentSnapshot == nullptr)
        return DeferredSourceStatus::SourceUnavailable;
    if (running.lastRequestedContentRevision != source.contentSnapshot->contentRevision)
        return DeferredSourceStatus::PlanNotSynchronized;
    if (source.canonicalBuffer == nullptr
        || source.contentSnapshot->audioRevision != expectedAudioRevision
        || source.canonicalSampleRate != job.audioSampleRate
        || source.canonicalBuffer->getNumSamples() != expectedAudioSamples
        || source.canonicalBuffer->getNumChannels() != expectedAudioChannels
        || job.startSample < 0
        || job.endSampleExclusive > source.canonicalBuffer->getNumSamples()
        || job.endSampleExclusive <= job.startSample)
        return DeferredSourceStatus::Stale;

    job.contentSnapshot = std::move(source.contentSnapshot);
    job.audioBuffer = std::move(source.canonicalBuffer);
    job.audioSampleRate = source.canonicalSampleRate;
    job.contentRevision = job.contentSnapshot->contentRevision;
    return DeferredSourceStatus::Ready;
}

bool ContentRenderService::isCurrentRenderCache(
    ContentKey key, const std::shared_ptr<RenderCache>& cache) const
{
    return cache != nullptr && renderCaches_.get(key) == cache;
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
    auto cache = renderCaches_.get(key);
    if (cache != nullptr) {
        ProcessRenderRuntime::getInstance().pruneDeferredJobsForCache(cache);
        renderWorker_.discardStage1Queue(cache.get());
    }
    renderCaches_.remove(key);
    if (cache != nullptr)
        ProcessRenderRuntime::getInstance().pruneDeferredJobsForCache(cache);
}

void ContentRenderService::attachExecutionLease(ExecutionLease lease)
{
    renderWorker_.attachExecutionLease(std::move(lease));
    if (auto service = weak_from_this().lock())
    {
        ProcessRenderRuntime::getInstance().registerBackendChangeListener(service);
        backendListenerRegistered_ = true;
    }
}

void ContentRenderService::detachExecutionLease(void* owner)
{
    if (backendListenerRegistered_)
    {
        ProcessRenderRuntime::getInstance().unregisterBackendChangeListener(this);
        backendListenerRegistered_ = false;
    }
    renderWorker_.detachExecutionLease(owner);
}

void ContentRenderService::notifyBackendChanged()
{
    renderWorker_.notifyBackendChanged();
}

void ContentRenderService::enqueueRender(RenderJob job)
{
    if (job.kind != RenderJob::Kind::Stage1Render
        || job.renderCache == nullptr
        || job.contentSnapshot == nullptr
        || job.audioBuffer == nullptr
        || job.endSampleExclusive <= job.startSample)
        return;

    job.contentRevision = job.contentSnapshot->contentRevision;

    // 即时提取 active-EQ Note 的样本保护范围（只保留 start/end，不复制
    // EqSettings），仅本调用栈内读取，不存储。用现有 TimeCoordinate floor/ceil
    // 表达 Note 覆盖范围，无新取整策略。Stage1 输入已固定 44.1kHz（CRS 播放源
    // canonical），Note 边界统一按 RenderCache::kSampleRate 换算，无第二采样率轴。
    std::vector<RenderChunkPlanner::ProtectedRange> protectedRanges;
    for (const auto& note : job.contentSnapshot->notes)
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

    // 完整计划：请求 [0, contentSampleCount) 得到全内容 chunk 几何（升序无重叠）。
    // silentGaps 与 notes 同源，直接用 snapshot 真相。
    const int64_t contentSampleCount = job.audioBuffer->getNumSamples();
    const auto chunkRanges = RenderChunkPlanner::selectChunksIntersectingRange(
        contentSampleCount,
        job.contentSnapshot->silentGaps,
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
    // Only invalidate when state actually changed — a no-op reconcile must not destroy
    // existing TimeStretchCache entries.
    // reconcile 与 queue sync 在 RenderWorker 同一临界区内完成：worker 不可能
    // 在两者之间 claim 旧 queued job 并拿到 reconcile 后的新 targetRevision。
    renderWorker_.reconcileAndSyncStage1Queue(job, [&]() {
        const auto reconcileResult = job.renderCache->reconcileFullPlanAndRequest(
            fullPlan,
            job.startSample,
            job.endSampleExclusive,
            job.contentSnapshot->contentRevision);

        if (reconcileResult.stateChanged)
            timeStretchCache_.invalidate(job.contentKey);
    });
    ProcessRenderRuntime::getInstance().pruneStaleDeferredJobsForCache(job.renderCache);
    ProcessRenderRuntime::getInstance().renderQueueCapacityAvailable();
}

RenderWorker::RequeueResult ContentRenderService::requeueRenderChunk(const RenderJob& job)
{
    jassert(job.kind == RenderJob::Kind::Stage1Render && job.renderCache != nullptr);

    // Cache state transition and physical insertion share RenderWorker's
    // queue mutex with owner detach.  A detached lease therefore rejects the
    // requeue before it can turn Running into an orphaned Pending chunk.
    return renderWorker_.requeueStage1Chunk(job);
}

std::vector<RenderWorker::RequeueResult> ContentRenderService::requeueRenderChunks(
    const std::vector<RenderJob>& jobs)
{
    return renderWorker_.requeueStage1Chunks(jobs);
}

void ContentRenderService::setDeferredRetryCapacityCallback(std::function<void()> callback)
{
    renderWorker_.setStage1CapacityCallback(std::move(callback));
}

std::shared_ptr<RenderWorker::AsyncState> ContentRenderService::beginAsyncRenderJob()
{
    return renderWorker_.beginAsyncJob();
}

void ContentRenderService::completeAsyncRenderJob(const std::shared_ptr<RenderWorker::AsyncState>& state) noexcept
{
    RenderWorker::completeAsyncJob(state);
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

bool ContentRenderService::drainRenderWorkerForExport()
{
    auto& runtime = ProcessRenderRuntime::getInstance();
    for (;;)
    {
        renderWorker_.drain();
        if (!runtime.hasDeferredRetriesForService(this))
            return true;
        if (!runtime.flushDeferredRetriesForService(this)
            && runtime.hasDeferredRetriesForService(this))
            return false;
    }
}

void ContentRenderService::waitAsyncRenderJobs()
{
    renderWorker_.waitAsyncIdle();
}

std::size_t ContentRenderService::renderQueueDepth() const noexcept
{
    return renderWorker_.queueDepth();
}

int ContentRenderService::renderInFlight() const noexcept
{
    return renderWorker_.inFlight();
}

int ContentRenderService::renderAsyncInFlight() const noexcept
{
    return renderWorker_.asyncInFlight();
}

std::shared_ptr<SoundTouchStretcher> ContentRenderService::getStretcher(
    ContentKey key, double sampleRate, int channels)
{
    return stretchers_.getOrCreate(key, sampleRate, channels);
}

void ContentRenderService::removeStretcher(ContentKey key)
{
    stretchers_.remove(key);
}

void ContentRenderService::clearAll()
{
    ProcessRenderRuntime::getInstance().pruneDeferredJobsForService(this);
    playbackSources_.clear();
    renderWorker_.discardAllStage1Queue();
    renderCaches_.clear();
    ProcessRenderRuntime::getInstance().pruneDeferredJobsForService(this);
    stretchers_.clear();
    timeStretchCache_.clear();
}

void ContentRenderService::preparePlaybackSampleRate(double targetSr)
{
    if (targetSr <= 0.0)
        return;

    if (!renderCaches_.preparePlaybackSampleRate(targetSr))
        return;
    playbackSources_.setPlaybackSampleRate(targetSr);
    timeStretchCache_.prepareForPlaybackSampleRate(targetSr);
}

} // namespace OpenTune
