#pragma once

#include "../Content/ContentKey.h"
#include "../Content/EditableContentSnapshot.h"
#include "PlaybackReadSource.h"
#include "RenderJob.h"
#include "PlaybackSourcePublisher.h"
#include "RenderCacheRegistry.h"
#include "RenderWorker.h"
#include "StretcherPool.h"
#include "../Inference/RenderCache.h"
#include "../Inference/TimeStretchCache.h"
#include <juce_core/juce_core.h>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace OpenTune {

class SoundTouchStretcher;

/** Domain-neutral owner of derived render artifacts. */
class ContentRenderService
{
public:
    using ExecutionLease = RenderExecutionLease;

    /** Canonical Stage1 完整物化后，把一次 Stage2 重建放入现有 RenderWorker 队列。 */
    bool enqueueStage2RebuildWhenCanonicalSettled(
        ContentKey contentKey,
        std::shared_ptr<const EditableContentSnapshot> contentSnapshot,
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
        double audioSampleRate);

    ContentRenderService();
    ~ContentRenderService();

    ContentRenderService(const ContentRenderService&) = delete;
    ContentRenderService& operator=(const ContentRenderService&) = delete;

    void publishPlaybackSource(ContentKey key, PlaybackReadSource source);
    bool republishPlaybackSource(
        ContentKey key,
        std::shared_ptr<const EditableContentSnapshot> contentSnapshot);
    bool getPlaybackReadSource(ContentKey key, PlaybackReadSource& out) const;
    enum class DeferredSourceStatus : uint8_t {
        Ready,
        SourceUnavailable,
        PlanNotSynchronized,
        Stale
    };
    DeferredSourceStatus restoreDeferredRenderJob(RenderJob& job,
                                                   uint64_t expectedAudioRevision,
                                                   int expectedAudioSamples,
                                                   int expectedAudioChannels) const;
    bool isCurrentRenderCache(ContentKey key, const std::shared_ptr<RenderCache>& cache) const;
    void removePlaybackSource(ContentKey key);

    std::shared_ptr<RenderCache> getOrCreateRenderCache(ContentKey key);
    std::shared_ptr<RenderCache> getRenderCache(ContentKey key) const;
    void removeRenderCache(ContentKey key);

    void attachExecutionLease(ExecutionLease lease);
    void detachExecutionLease(void* owner);
    // notes/silentGaps 从 job.contentSnapshot 读取，不复制、不额外传参。
    void enqueueRender(RenderJob job);
    // stale-generation 回退：只回退 RenderCache 状态机（Running→Pending）并投递
    // 一个带 chunk 身份的队列项，不重算几何/快照、不 bump desired。
    RenderWorker::RequeueResult requeueRenderChunk(const RenderJob& job);
    std::vector<RenderWorker::RequeueResult> requeueRenderChunks(const std::vector<RenderJob>& jobs);
    void setDeferredRetryCapacityCallback(std::function<void()> callback);
    std::shared_ptr<RenderWorker::AsyncState> beginAsyncRenderJob();
    static void completeAsyncRenderJob(const std::shared_ptr<RenderWorker::AsyncState>& state) noexcept;
    void pauseRenderWorker();
    void resumeRenderWorker();
    void drainRenderWorker();
    bool drainRenderWorkerForExport();
    void waitAsyncRenderJobs();

    std::size_t renderQueueDepth() const noexcept;
    int renderInFlight() const noexcept;
    int renderAsyncInFlight() const noexcept;

    std::shared_ptr<SoundTouchStretcher> getStretcher(
        ContentKey key, double sampleRate, int channels);
    void removeStretcher(ContentKey key);

    TimeStretchCache& getTimeStretchCache() noexcept { return timeStretchCache_; }
    const TimeStretchCache& getTimeStretchCache() const noexcept { return timeStretchCache_; }

    /// Tracks revisions where Stage 1 baked the TimeGrid into conditioning so Stage 2
    /// publishes canonical PCM verbatim instead of stretching; keyed by (key, timeGridRevision).
    void markStage1BakedTimeGrid(ContentKey key, uint64_t timeGridRevision);
    bool isStage1BakedTimeGrid(ContentKey key, uint64_t timeGridRevision) const;
    /// True when this content has ever been rendered with a baked TimeGrid, at any
    /// revision. Used to decide that a grid edit must re-run Stage 1 even when the
    /// new grid is identity (the settled PCM still carries the old mapping).
    bool hasStage1BakedTimeGrid(ContentKey key) const;
    void clearStage1BakedTimeGrid(ContentKey key);

    void clearAll();
    void preparePlaybackSampleRate(double targetSr);

private:
    PlaybackSourcePublisher playbackSources_;
    RenderCacheRegistry renderCaches_;
    StretcherPool stretchers_;
    TimeStretchCache timeStretchCache_;
    RenderWorker renderWorker_;

    mutable std::mutex bakedTimeGridMutex_;
    std::map<ContentKey, uint64_t> bakedTimeGridRevisions_;
};

} // namespace OpenTune
