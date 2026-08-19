#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <map>
#include <set>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "Utils/TimeCoordinate.h"
#include "../DSP/ResamplingManager.h"

struct RenderCacheTestAccessor;

namespace OpenTune {

class RenderCache {
public:
    static constexpr size_t kDefaultGlobalCacheLimitBytes = static_cast<size_t>(256) * 1024 * 1024;
    static constexpr double kSampleRate = TimeCoordinate::kRenderSampleRate;

    struct Chunk {
        double startSeconds{0.0};
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
        std::shared_ptr<const std::vector<float>> audio;

        enum class Status : uint8_t {
            Idle,    // 无待处理渲染需求
            Pending, // 有待渲染需求，等 Worker 拉取
            Running, // 正在渲染中
            Blank,   // 空白区域（无有效F0），无需渲染
            Failed   // 显式失败：后端推理错误，需重试才能再次运行
        };
        Status status{Status::Idle};

        uint64_t desiredRevision{0};    // 目标版本（用户最新编辑产生）
        uint64_t runningRevision{0};   // 当前正在运行的 revision（在 Pending→Running 时记录）
        uint64_t publishedRevision{0};  // 已成功发布的版本
        uint64_t lastRequestedContentRevision{0}; // 上次 reconcile 时的内容版本，用于去重
    };

    // 调度状态管理 API
    struct PlannedChunk {
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
    };
    // 以完整计划原子重建 chunks_/pendingChunks_，返回本次需投递的 worker job token 数。
    // contentRevision 来自 EditableContentSnapshot：同几何且同内容版本时跳过 desiredRevision 递增。
    struct ReconcileResult {
        std::size_t workerTokenCount{0};
        bool stateChanged{false};
    };
    ReconcileResult reconcileFullPlanAndRequest(const std::vector<PlannedChunk>& fullPlan,
                                                int64_t requestStartSample,
                                                int64_t requestEndSampleExclusive,
                                                uint64_t contentRevision);

    struct PendingJob {
        double startSeconds{0.0};
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
        uint64_t targetRevision{0};
    };
    bool getNextPendingJob(PendingJob& outJob);

    enum class ChunkRenderResult : uint8_t {
        Published,
        Stale,
        InvalidInput
    };

    ChunkRenderResult completeChunkRenderWithAudio(int64_t startSample,
                                                    int64_t endSampleExclusive,
                                                    std::vector<float>&& audio,
                                                    uint64_t revision);

    void completeChunkRenderFailure(double startSeconds, uint64_t revision);

    /**
     * stale-generation 回退：仅当 chunk 仍正处 Running 且 runningRevision 匹配时，
     * 把 Running→Pending 并插入待拉取集合、runningRevision 清零。不 bump desired、
     * 不改几何、不发布快照。返回是否实际回退。
     */
    bool requeueRunningChunk(double startSeconds, uint64_t runningRevision);

    void markChunkAsBlank(double startSeconds, uint64_t revision);

    struct ChunkStats {
        int idle{0};
        int pending{0};
        int running{0};
        int blank{0};
        int failed{0};
        int total() const { return idle + pending + running + blank + failed; }
        bool hasActiveWork() const { return pending > 0 || running > 0; }
    };
    ChunkStats getChunkStats() const;

    struct StateSnapshot {
        ChunkStats chunkStats;
        bool hasPublishedAudio{false};
        bool hasNonBlankChunks{false};
    };
    StateSnapshot getStateSnapshot() const;

    // 是否处于 canonical settled：
    //  - chunks 非空
    //  - 无 Pending/Running chunk
    //  - 每个 chunk 若为 Blank 则已完成（runningRevision==0）
    //  - 仅存 Idle：每个 Idle chunk 须有非空 audio、publishedRevision>0、
    //    且 publishedRevision==desiredRevision
    //  - 失败时不标记 Idle 完成，原样返回 false
    bool isCanonicalSettled() const;

    RenderCache();
    ~RenderCache();

    // ============================================================
    // 准备方法：将 canonical chunk 重采样到目标播放采样率（使用自有 ResamplingManager）。
    // 调用方（ContentRenderService::preparePlaybackSampleRate）在设备/导出切换时调用。
    // ============================================================
    void prepareForPlaybackSampleRate(double targetSr);

    // 音频线程直接 copy prepared overlay（无插值）。只能读取已 prepared 的目标率数据。
    void overlayPreparedAudio(juce::AudioBuffer<float>& destination,
                              int destStartSample,
                              int numSamples,
                              int64_t readStartSample,
                              int targetSampleRate) const;

    // 非实时 canonical overlay（仅供 Stage2/export 等 44.1kHz 读取）。
    void overlayCanonicalAudio(juce::AudioBuffer<float>& destination,
                                int destStartSample,
                                int numSamples,
                                int64_t readStartSample) const;

    void clear();

private:
    struct PublishedChunk {
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
        std::shared_ptr<const std::vector<float>> audio;
    };

    struct PublishedRenderSnapshot {
        std::vector<PublishedChunk> chunks;  // sorted by startSample ascending
    };

    // Prepared chunk — 目标播放采样率下，从 canonical 绝对投影得到；
    // 相邻 chunk 边界精确连续，无缝隙。
    // 当目标率 == 44.1kHz 时 audio 别名 canonical chunk audio（shared_ptr copy）。
    struct PublishedPreparedChunk {
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
        std::shared_ptr<const std::vector<float>> audio;
    };

    struct PublishedPreparedSnapshot {
        double sampleRate{0.0};
        std::vector<PublishedPreparedChunk> chunks;  // sorted by startSample ascending
    };

    friend struct RenderCacheTestAccessor;
    mutable juce::SpinLock lock_;
    std::map<double, Chunk> chunks_;
    std::set<double> pendingChunks_;

    std::shared_ptr<const PublishedRenderSnapshot> publishedSnapshot_;
    std::shared_ptr<const PublishedPreparedSnapshot> preparedSnapshot_;

    // Prepared rebuild state: serialized by preparedBuildMutex_ (non-audio thread).
    double preparedSampleRate_{0.0};
    size_t preparedMemoryUsage_{0};
    ResamplingManager preparedResampler_;

    // Writer mutex serializes prepared rebuild + target rate update.
    mutable std::mutex preparedBuildMutex_;

    // Writer-owned release pool for old published generations.
    mutable std::vector<std::shared_ptr<const PublishedRenderSnapshot>> retiredSnapshots_;
    mutable std::vector<std::shared_ptr<const PublishedPreparedSnapshot>> retiredPreparedSnapshots_;

    void publishLocked();
    void rebuildPrepared();
    void pruneRetiredSnapshotsLocked() const;

public:
    static std::atomic<size_t>& globalCacheLimitBytes();
    static std::atomic<size_t>& globalCacheCurrentBytes();
    static std::atomic<size_t>& globalCachePeakBytes();
};

} // namespace OpenTune
