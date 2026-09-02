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
        uint64_t pitchRevision{0};
        uint64_t pitchShiftRevision{0};
        uint64_t timeGridRevision{0};
    };

    /** Canonical Stage1 完整物化后，把一次 Stage2 重建放入现有 RenderWorker 队列。 */
    bool enqueueStage2RebuildWhenCanonicalSettled(Stage2Request request);

    ContentRenderService();
    ~ContentRenderService();

    ContentRenderService(const ContentRenderService&) = delete;
    ContentRenderService& operator=(const ContentRenderService&) = delete;

    void publishPlaybackSource(ContentKey key, PlaybackReadSource source);
    bool getPlaybackReadSource(ContentKey key, PlaybackReadSource& out) const;
    void removePlaybackSource(ContentKey key);

    std::shared_ptr<RenderCache> getOrCreateRenderCache(ContentKey key);
    std::shared_ptr<RenderCache> getRenderCache(ContentKey key) const;
    void removeRenderCache(ContentKey key);

    void attachExecutionLease(ExecutionLease lease);
    void detachExecutionLease(void* owner);
    // notes 仅在本调用栈内读取（提取 active-EQ Note 保护范围），不复制、不存储。
    void enqueueRender(RenderJob job, const std::vector<Note>& notes);
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

private:
    PlaybackSourcePublisher playbackSources_;
    RenderCacheRegistry renderCaches_;
    RenderWorker renderWorker_;
    StretcherPool stretchers_;
    TimeStretchCache timeStretchCache_;
};

} // namespace OpenTune
