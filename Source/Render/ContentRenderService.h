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
#include <memory>

namespace OpenTune {

class SoundTouchStretcher;

/** Domain-neutral owner of derived render artifacts. */
class ContentRenderService
{
public:
    using ExecutionLease = RenderExecutionLease;

    struct Stage2Request
    {
        ContentKey contentKey;
        // 重建输入随请求固定：worker 只消费这里的 snapshot/audio，不回查 owner。
        std::shared_ptr<const EditableContentSnapshot> contentSnapshot;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        double audioSampleRate{0.0};
    };

    /** Canonical Stage1 完整物化后，把一次 Stage2 重建放入现有 RenderWorker 队列。 */
    bool enqueueStage2RebuildWhenCanonicalSettled(Stage2Request request);

    ContentRenderService();
    ~ContentRenderService();

    ContentRenderService(const ContentRenderService&) = delete;
    ContentRenderService& operator=(const ContentRenderService&) = delete;

    void publishPlaybackSource(ContentKey key, PlaybackReadSource source);
    bool republishPlaybackSource(
        ContentKey key,
        std::shared_ptr<const EditableContentSnapshot> contentSnapshot);
    bool getPlaybackReadSource(ContentKey key, PlaybackReadSource& out) const;
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
    void requeueRenderChunk(const RenderJob& job);
    void beginAsyncRenderJob();
    void completeAsyncRenderJob();
    void pauseRenderWorker();
    void resumeRenderWorker();
    void drainRenderWorker();

    SoundTouchStretcher* getStretcher(ContentKey key, double sampleRate, int channels);
    void removeStretcher(ContentKey key);

    TimeStretchCache& getTimeStretchCache() noexcept { return timeStretchCache_; }
    const TimeStretchCache& getTimeStretchCache() const noexcept { return timeStretchCache_; }

    void clearAll();
    void preparePlaybackSampleRate(double targetSr);
    double getPlaybackSampleRate() const;

private:
    PlaybackSourcePublisher playbackSources_;
    RenderCacheRegistry renderCaches_;
    RenderWorker renderWorker_;
    StretcherPool stretchers_;
    TimeStretchCache timeStretchCache_;
};

} // namespace OpenTune
