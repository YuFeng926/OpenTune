#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <map>
#include <set>
#include <atomic>
#include <cstdint>

#include "Utils/TimeCoordinate.h"
#include "DSP/CrossoverMixer.h"

struct RenderCacheTestAccessor;

namespace OpenTune {

class RenderCache {
public:
    static constexpr size_t kDefaultGlobalCacheLimitBytes = static_cast<size_t>(256) * 1024 * 1024;
    static constexpr double kSampleRate = TimeCoordinate::kRenderSampleRate;

    struct Chunk {
        double startSeconds{0.0};
        double endSeconds{0.0};
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
        std::vector<float> audio;

        enum class Status : uint8_t {
            Idle,    // 无待处理渲染需求
            Pending, // 有待渲染需求，等待 Worker 拉取
            Running, // 正在渲染中
            Blank    // 空白区域（无有效F0），无需渲染
        };
        Status status{Status::Idle};

        uint64_t desiredRevision{0};    // 目标版本（用户最新编辑产生）
        uint64_t publishedRevision{0};  // 已成功发布的版本
    };

    // 调度状态管理 API
    // 编辑事件入口：请求渲染（更新 desiredRevision，Idle -> Pending）
    void requestRenderPending(double startSeconds,
                              double endSeconds,
                              int64_t startSample,
                              int64_t endSampleExclusive);

    // Worker 拉取：查找 Pending 状态的 Chunk（供调度器遍历）
    struct PendingJob {
        double startSeconds{0.0};
        double endSeconds{0.0};
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
        uint64_t targetRevision{0};
    };
    bool getNextPendingJob(PendingJob& outJob);

    enum class CompletionResult : uint8_t {
        Succeeded,        // 已成功产出 revision 对应结果
        TerminalFailure   // 本次 revision 终态失败（不重试）
    };

    // 渲染完成回调：Running -> Idle/Pending
    // - Succeeded 且版本匹配：Idle，publishedRevision = revision
    // - Succeeded 但版本过期（revision < desiredRevision）：Pending，需重新渲染
    // - TerminalFailure：Idle，不重试
    void completeChunkRender(double startSeconds, uint64_t revision, CompletionResult result);

    // 标记 Chunk 为空白区域（无有效F0），从待渲染队列移除
    void markChunkAsBlank(double startSeconds);

    // 获取当前 Pending 任务数
    int getPendingCount() const;

    // 获取 Chunk 状态统计（用于 UI 显示）
    struct ChunkStats {
        int idle{0};
        int pending{0};
        int running{0};
        int blank{0};
        int total() const { return idle + pending + running + blank; }
        bool hasActiveWork() const { return pending > 0 || running > 0; }
    };
    ChunkStats getChunkStats() const;

    struct StateSnapshot {
        ChunkStats chunkStats;
        bool hasPublishedAudio{false};
        bool hasNonBlankChunks{false};
    };
    StateSnapshot getStateSnapshot() const;

    RenderCache();
    ~RenderCache();

    bool addChunk(int64_t startSample, int64_t endSampleExclusive, std::vector<float>&& audio, uint64_t targetRevision);

    void overlayPublishedAudioForRate(juce::AudioBuffer<float>& destination,
                                      int destStartSample,
                                      int numSamples,
                                      double timeSeconds,
                                      int targetSampleRate) const;

    void clear();

    /** Prepare the crossover mixer for the given playback sample rate. */
    void prepareCrossoverMixer(double sampleRate, int maxBlockSize, int numChannels = 2);

    /** Per-materialization crossover mixer (mutable for use in const read path). */
    CrossoverMixer& getCrossoverMixer() const { return crossoverMixer_; }

private:
    friend struct RenderCacheTestAccessor;
    mutable juce::SpinLock lock_;
    std::map<double, Chunk> chunks_;
    std::set<double> pendingChunks_;  // 待渲染 Chunk 的 startSeconds 索引
    size_t totalMemoryUsage_ = 0;

    static std::atomic<size_t>& globalCacheLimitBytes();
    static std::atomic<size_t>& globalCacheCurrentBytes();
    static std::atomic<size_t>& globalCachePeakBytes();

    mutable CrossoverMixer crossoverMixer_;
};

} // namespace OpenTune
