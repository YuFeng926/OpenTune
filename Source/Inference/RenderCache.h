#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <atomic>
#include <mutex>
#include "Utils/TimeCoordinate.h"

struct RenderCacheTestAccessor;

namespace OpenTune {

class RenderCache {
public:
    static constexpr size_t kDefaultGlobalCacheLimitBytes = static_cast<size_t>(1536) * 1024 * 1024;
    static constexpr double kSampleRate = TimeCoordinate::kRenderSampleRate;

    struct GlobalMemoryStats {
        size_t cacheLimitBytes{0};
        size_t cacheCurrentBytes{0};
        size_t cachePeakBytes{0};
    };

    struct Chunk {
        double startSeconds{0.0};
        double endSeconds{0.0};
        std::vector<float> audio;
        std::unordered_map<int, std::vector<float>> resampledAudio;

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
    void requestRenderPending(double startSeconds, double endSeconds);

    // Worker 拉取：查找 Pending 状态的 Chunk（供调度器遍历）
    struct PendingJob {
        double startSeconds{0.0};
        double endSeconds{0.0};
        uint64_t targetRevision{0};
    };
    bool getNextPendingJob(PendingJob& outJob);

    enum class CompletionResult : uint8_t {
        Succeeded,        // 已成功产出 revision 对应结果
        RetryableFailure, // 未执行成功且可重试（如并发拒绝）
        TerminalFailure   // 本次 revision 终态失败（不重试）
    };

    // 渲染完成回调：Running -> Idle/Pending
    // - Succeeded 且版本匹配：Idle，publishedRevision = revision
    // - Succeeded 但版本过期（revision < desiredRevision）：Pending，需重新渲染
    // - RetryableFailure：Pending，等待后续重试
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

    // 删除 Clip 时清理该 cache 的 pending 状态
    void clearAllPending();

    RenderCache();
    ~RenderCache();

    bool addChunk(double startSeconds, double endSeconds, std::vector<float>&& audio, uint64_t targetRevision);

    bool addResampledChunk(double startSeconds, double endSeconds, int targetSampleRate,
                          std::vector<float>&& resampledAudio, uint64_t targetRevision);

    struct PublishedChunk {
        double startSeconds;
        double endSeconds;
        const std::vector<float>* audio;
    };

    std::vector<PublishedChunk> getPublishedChunks() const;

    int readAtTimeForRate(float* dest, int numSamples, double timeSeconds,
                          int targetSampleRate, bool nonBlocking = false);

    bool isRevisionPublished(double startSeconds, double endSeconds, uint64_t revision) const;

    void clearResampledCache();

    void clear();

    size_t getTotalMemoryUsage() const;
    void setMemoryLimit(size_t bytes);

    static GlobalMemoryStats getGlobalMemoryStats();

private:
    friend struct RenderCacheTestAccessor;
    mutable juce::SpinLock lock_;
    std::map<double, Chunk> chunks_;
    std::set<double> pendingChunks_;  // 待渲染 Chunk 的 startSeconds 索引
    size_t totalMemoryUsage_ = 0;

    static std::atomic<size_t>& globalCacheLimitBytes();
    static std::atomic<size_t>& globalCacheCurrentBytes();
    static std::atomic<size_t>& globalCachePeakBytes();
};

} // namespace OpenTune
