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

    // Stage2Request — 调用方填写的身份 + 最小修订号。
    // owner snapshot 由调用方在域层获取后通过 requestStage2Rebuild 的
    // 第二参数显式传入，避免 CRS 反向依赖具体域实现。
    struct Stage2Request
    {
        ContentKey contentKey;
        uint64_t pitchRevision{0};
        uint64_t pitchShiftRevision{0};
        uint64_t timeGridRevision{0};
    };

    /**
     * 触发一次 Stage2（时间拉伸）重建。
     *
     * 该入口由调用方域（非 ARA 路径走 processor，ARA 路径走
     * OpenTuneDocumentController）提供其已抓取的 owner snapshot。
     * CRS 不直接抓 snapshot——这保持了 CRS 的域无关性（domain-neutral）。
     *
     * 当前为同步执行，调用方负责决定是否需要异步/队列化。
     */
    void requestStage2Rebuild(Stage2Request request,
                              std::shared_ptr<const EditableContentSnapshot> ownerSnap);

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
    void beginAsyncRenderJob();
    void completeAsyncRenderJob();
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

    /** 设置目标播放采样率，依次驱动 publisher → registry → TimeStretchCache。 */
    void preparePlaybackSampleRate(double targetSr);

private:
    PlaybackSourcePublisher playbackSources_;
    RenderCacheRegistry renderCaches_;
    RenderWorker renderWorker_;
    StretcherPool stretchers_;
    TimeStretchCache timeStretchCache_;
};

} // namespace OpenTune
