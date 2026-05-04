#pragma once

/**
 * F0 提取服务
 * 
 * 多线程异步 F0 提取任务管理器：
 * - 使用工作线程池处理 F0 提取请求
 * - 支持请求去重（同一 materialization 不会同时提取）
 * - 支持任务取消
 * - 线程安全的结果回调
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "../Utils/LockFreeQueue.h"
#include "../Utils/SilentGapDetector.h"

namespace OpenTune {

class F0ExtractionService {
public:
    struct Result {
        bool success{false};
        int trackId{0};
        int placementIndexHint{-1};
        uint64_t materializationId{0};
        uint64_t requestKey{0};
        uint64_t requestToken{0};
        std::shared_ptr<const juce::AudioBuffer<float>> sourceAudioBuffer;
        int hopSize{0};
        int f0SampleRate{0};
        std::vector<float> f0;
        std::vector<float> energy;
        std::vector<SilentGap> silentGaps;
        const char* modelName{"Unknown"};
        std::string errorMessage;

        // F0Alignment diagnostics (populated by extractOriginalF0ForImportedClip)
        double audioDurationSeconds{0.0};
        double firstAudibleTimeSeconds{-1.0};
        double firstVoicedTimeSeconds{-1.0};
        int firstVoicedFrame{-1};
        int expectedInferenceFrameCount{0};
    };

    using ExecuteFn = std::function<Result()>;
    using CommitFn = std::function<void(Result&&)>;

    enum class SubmitResult : uint8_t {
        Accepted,
        AlreadyInProgress,
        QueueFull,
        InvalidTask
    };

    explicit F0ExtractionService(int workerCount = 1, size_t maxQueueSize = 64);
    ~F0ExtractionService();

    static uint64_t makeRequestKey(uint64_t materializationId, int trackId, int placementIndex);

    SubmitResult submit(uint64_t requestKey, ExecuteFn execute, CommitFn commit);

    bool isActive(uint64_t requestKey) const;
    void cancel(uint64_t requestKey);

private:
    struct Task {
        uint64_t requestKey{0};
        uint64_t token{0};
        ExecuteFn execute;
        CommitFn commit;
    };

    struct ActiveEntry {
        std::atomic<uint64_t> token{0};
    };

    void workerLoop();

    LockFreeQueue<Task> queue_;
    std::unordered_map<uint64_t, std::unique_ptr<ActiveEntry>> activeEntries_;
    mutable std::mutex entriesMutex_;
    std::vector<std::thread> workers_;
    size_t maxQueueSize_{64};
    std::atomic<uint64_t> tokenCounter_{1};
    std::atomic<bool> running_{true};
};

} // namespace OpenTune
