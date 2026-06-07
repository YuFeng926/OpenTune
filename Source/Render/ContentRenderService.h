#pragma once

#include "../Content/ContentKey.h"
#include "../Content/EditableContentSnapshot.h"
#include "../Inference/RenderCache.h"
#include "../Inference/TimeStretchCache.h"
#include "../Utils/SourceWindow.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/SilentGapDetector.h"
#include "../Utils/PitchShiftSettings.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace OpenTune {

class SoundTouchStretcher; // forward

/**
 * ContentRenderService — 统一渲染/缓存/工作器服务。
 *
 * 拥有：
 *  - RenderCache 查找和生命周期
 *  - TimeStretchCache 查找和生命周期
 *  - 渲染工作线程和挂起队列
 *  - 播放快照发布（lock-free）
 *
 * 不拥有：
 *  - 处理器回调的长期所有权（通过 ExecutionLease 短租约）
 *  - 权威内容（内容属于 DomainContentOwner）
 *  - MaterializationStore 指针
 */
class ContentRenderService
{
public:
    // ============================================================
    // 播放读取源（取代 MaterializationStore::PlaybackReadSource）
    // ============================================================
    struct PlaybackReadSource
    {
        std::shared_ptr<RenderCache> renderCache;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        TimeStretchCache* timeStretchCache{nullptr};
        ContentKey contentKey;
        uint32_t pitchRevision{0};
        uint32_t timeGridRevision{0};
        PitchShiftSettings pitchShiftSettings;
        bool timeGridIsIdentity{true};

        bool hasAudio() const
        {
            return audioBuffer != nullptr && audioBuffer->getNumSamples() > 0;
        }
        bool canRead() const
        {
            return renderCache != nullptr || hasAudio();
        }
    };

    // ============================================================
    // 挂起渲染任务
    // ============================================================
    struct PendingRenderJob
    {
        ContentKey contentKey;
        std::shared_ptr<RenderCache> renderCache;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        std::shared_ptr<PitchCurve> pitchCurve;
        std::vector<SilentGap> silentGaps;
        double startSeconds{0.0};
        double endSeconds{0.0};
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
        uint64_t targetRevision{0};
        uint64_t pitchRevision{0};
        uint64_t timeGridRevision{0};
    };

    // ============================================================
    // 执行租约：处理器桥接短租约，不长期持有 callback
    // ============================================================
    struct ExecutionLease
    {
        std::function<void(PendingRenderJob&)> renderJobCallback;
        void* leaseOwner{nullptr};

        bool isValid() const noexcept { return renderJobCallback && leaseOwner != nullptr; }
    };

    // ============================================================
    // Stage 2 请求：用 (ContentKey, revision) 而非 (materializationId, store*)
    // ============================================================
    struct Stage2Request
    {
        ContentKey contentKey;
        uint64_t pitchRevision{0};
        uint64_t timeGridRevision{0};
        std::shared_ptr<const EditableContentSnapshot> snapshot;
    };

    ContentRenderService();
    ~ContentRenderService();

    ContentRenderService(const ContentRenderService&) = delete;
    ContentRenderService& operator=(const ContentRenderService&) = delete;

    // ============================================================
    // 播放源发布
    // ============================================================
    void publishPlaybackSource(ContentKey key, PlaybackReadSource source);
    bool getPlaybackReadSource(ContentKey key, PlaybackReadSource& out) const;
    void removePlaybackSource(ContentKey key);
    void clearAll();

    // ============================================================
    // RenderCache 管理
    // ============================================================
    std::shared_ptr<RenderCache> getOrCreateRenderCache(ContentKey key);
    std::shared_ptr<RenderCache> getRenderCache(ContentKey key) const;
    void removeRenderCache(ContentKey key);

    // ============================================================
    // TimeStretchCache（Stage 2 缓存）
    // ============================================================
    TimeStretchCache& getTimeStretchCache() noexcept { return timeStretchCache_; }
    const TimeStretchCache& getTimeStretchCache() const noexcept { return timeStretchCache_; }

    // ============================================================
    // 渲染工作器
    // ============================================================
    void attachExecutionLease(ExecutionLease lease);
    void detachExecutionLease(void* leaseOwner);
    bool hasActiveLease(void* leaseOwner) const noexcept { return lease_.leaseOwner == leaseOwner; }

    void enqueueRender(PendingRenderJob job);
    bool pullNextPendingRenderJob(PendingRenderJob& out);
    bool hasPendingJobs() const;

    void notifyRenderWorker();
    void pauseRenderWorker();
    void resumeRenderWorker();
    void drainRenderWorker();

    // ============================================================
    // SoundTouch stretcher（per-content-key）
    // ============================================================
    SoundTouchStretcher* getStretcher(ContentKey key, double sampleRate, int channels);
    void removeStretcher(ContentKey key);

private:
    struct StretcherEntry
    {
        ContentKey key;
        std::unique_ptr<SoundTouchStretcher> stretcher;
    };

    struct PendingRenderEntry
    {
        ContentKey contentKey;
        double startSeconds{0.0};
        double endSeconds{0.0};
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
        uint64_t pitchRevision{0};
        uint64_t timeGridRevision{0};
        std::shared_ptr<PitchCurve> pitchCurve;
    };

    // 播放快照 lock-free 缓存
    mutable std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>> playbackSourceCache_;
    void rebuildPlaybackSourceCache();

    juce::ReadWriteLock lock_;
    std::map<ContentKey, PlaybackReadSource> playbackSources_;
    std::map<ContentKey, std::shared_ptr<RenderCache>> renderCaches_;
    std::vector<StretcherEntry> stretchers_;
    TimeStretchCache timeStretchCache_;

    // 渲染工作器
    std::thread renderWorkerThread_;
    std::mutex renderWorkerMutex_;
    std::condition_variable renderWorkerCv_;
    std::atomic<bool> renderWorkerShouldStop_{false};
    std::atomic<bool> renderPaused_{false};
    std::atomic<int> renderJobsInFlight_{0};

    mutable std::mutex renderQueueMutex_;
    std::deque<PendingRenderEntry> pendingRenderQueue_;

    ExecutionLease lease_;

    void renderWorkerLoop();
    void startRenderWorker();
    void stopRenderWorker();
};

} // namespace OpenTune
