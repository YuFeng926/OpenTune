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
#include <memory>
#include <cstdint>

namespace OpenTune {

class SoundTouchStretcher; // forward

/**
 * ContentRenderService — runtime services facade (Phase 0 thin facade).
 * 
 * 组合 extracted runtime services，不再自己实现基础设施。
 * 保留 facade API 供外部调用。
 * 
 * Phase 0.8: 删除重复实现，改为组合：
 * - PlaybackSourcePublisher (替代 playbackSources_/playbackSourceCache_)
 * - RenderCacheRegistry (替代 renderCaches_)
 * - RenderWorker (替代 worker thread fields)
 * - StretcherPool (替代 stretchers_)
 * - TimeStretchCache (保留)
 */
class ContentRenderService
{
public:
    // ExecutionLease — 使用统一的 RenderExecutionLease
    using ExecutionLease = RenderExecutionLease;

    // Stage2Request — 保留（外部使用）
    struct Stage2Request
    {
        ContentKey contentKey;
        uint64_t pitchRevision{0};
        uint64_t pitchShiftRevision{0};
        uint64_t timeGridRevision{0};
        std::shared_ptr<const EditableContentSnapshot> snapshot;
    };

    ContentRenderService();
    ~ContentRenderService();

    ContentRenderService(const ContentRenderService&) = delete;
    ContentRenderService& operator=(const ContentRenderService&) = delete;

    // ========================================
    // Facade API - 转发到 extracted services
    // ========================================

    // PlaybackSource
    void publishPlaybackSource(ContentKey key, PlaybackReadSource source);
    bool getPlaybackReadSource(ContentKey key, PlaybackReadSource& out) const;
    void removePlaybackSource(ContentKey key);

    // RenderCache
    std::shared_ptr<RenderCache> getOrCreateRenderCache(ContentKey key);
    std::shared_ptr<RenderCache> getRenderCache(ContentKey key) const;
    void removeRenderCache(ContentKey key);

    // RenderWorker
    void attachExecutionLease(ExecutionLease lease);
    void detachExecutionLease(void* owner);
    bool hasActiveLease(void* owner) const noexcept;

    void enqueueRender(RenderJob job);
    bool hasPendingJobs() const;
    void notifyRenderWorker();
    void pauseRenderWorker();
    void resumeRenderWorker();
    void drainRenderWorker();

    // Stretcher
    SoundTouchStretcher* getStretcher(ContentKey key, double sampleRate, int channels);
    void removeStretcher(ContentKey key);

    // TimeStretchCache (直接访问)
    TimeStretchCache& getTimeStretchCache() noexcept { return timeStretchCache_; }
    const TimeStretchCache& getTimeStretchCache() const noexcept { return timeStretchCache_; }

    // Utility
    void clearAll();

    // Accessors for extracted services (供 Phase 0.7 使用)
    PlaybackSourcePublisher& playbackSources() noexcept { return playbackSources_; }
    RenderCacheRegistry& renderCaches() noexcept { return renderCaches_; }
    RenderWorker& renderWorker() noexcept { return renderWorker_; }
    StretcherPool& stretchers() noexcept { return stretchers_; }

private:
    PlaybackSourcePublisher playbackSources_;
    RenderCacheRegistry renderCaches_;
    RenderWorker renderWorker_;
    StretcherPool stretchers_;
    TimeStretchCache timeStretchCache_;
};

} // namespace OpenTune
