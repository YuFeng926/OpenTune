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
 * Domain-neutral runtime rendering services.
 *
 * ContentRenderService stores derived artifacts only: playback read views,
 * render caches, render jobs, stretchers, and time-stretch cache. Editable
 * content remains owned by the domain model object addressed by ContentKey.
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

    void enqueueRender(RenderJob job);
    bool hasPendingJobs() const;
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

    // Accessors for focused runtime services.
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
