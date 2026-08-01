#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "../Utils/SilentGapDetector.h"
#include "../Content/ContentKey.h"

namespace OpenTune {

class F0InferenceService;
struct F0RunOwnerState;

// F0 提取任务去重键：ContentKey 唯一标识一次提取
struct F0RequestKey {
    ContentKey contentKey;

    bool operator==(const F0RequestKey& o) const noexcept {
        return contentKey == o.contentKey;
    }
    bool operator!=(const F0RequestKey& o) const noexcept { return !(*this == o); }
};

} // namespace OpenTune

template <>
struct std::hash<OpenTune::F0RequestKey> {
    size_t operator()(const OpenTune::F0RequestKey& k) const noexcept {
        size_t h = static_cast<size_t>(k.contentKey.domainKind);
        h ^= static_cast<size_t>(k.contentKey.objectId * 1099511628211ULL);
        h ^= static_cast<size_t>(k.contentKey.sourceWindowDiscriminator * 1099511628211ULL);
        return h;
    }
};

namespace OpenTune {

class F0ExtractionService {
public:
    struct Result {
        bool success{false};
        int trackId{0};
        int placementIndexHint{-1};
        ContentKey contentKey;
        F0RequestKey requestKey;
        uint64_t requestToken{0};
        std::shared_ptr<const juce::AudioBuffer<float>> sourceAudioBuffer;
        int hopSize{0};
        int f0SampleRate{0};
        std::vector<float> f0;
        std::vector<float> energy;
        std::vector<SilentGap> silentGaps;
        const char* modelName{"Unknown"};
        std::string errorMessage;

        double audioDurationSeconds{0.0};
        double firstAudibleTimeSeconds{-1.0};
        double firstVoicedTimeSeconds{-1.0};
        int firstVoicedFrame{-1};
        int expectedInferenceFrameCount{0};
    };

    using ExecuteFn = std::function<Result(const std::shared_ptr<F0RunOwnerState>&)>;
    using CommitFn = std::function<void(Result&&)>;

    enum class SubmitResult : uint8_t {
        Accepted,
        AlreadyInProgress,
        QueueFull,
        InvalidTask
    };

    F0ExtractionService(int workerCount, size_t maxQueueSize,
                        std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver);
    // 析构：调用 shutdown() —— 先终止属于本 owner 的活跃推理 Run（若有），再
    // join 所有 worker，不悬挂线程（detach 的 worker 在 DLL 卸载后执行 DLL 内
    // 代码会崩溃）。
    ~F0ExtractionService();

    /// 显式幂等关闭：停止接受新提交、丢弃排队 job、清空 active 表（shutdown 后
    /// isActive() 恒为 false）、终止本 owner 的 Run、join worker。关闭开始后完成
    /// 的任务不再投递 commit。析构自动调用，owner 也可在自身析构的最前段主动调用。
    void shutdown();

    SubmitResult submit(F0RequestKey requestKey, ExecuteFn execute, CommitFn commit);

    bool isActive(F0RequestKey requestKey) const;
    void cancel(F0RequestKey requestKey);

private:
    struct Task {
        F0RequestKey requestKey;
        uint64_t token{0};
        ExecuteFn execute;
        CommitFn commit;
    };

    struct ActiveEntry {
        uint64_t token{0};
    };

    std::deque<Task> queue_;                        // entriesMutex_ 下访问；容量合同 maxQueueSize_
    size_t maxQueueSize_;                           // 真实容量合同，entriesMutex_ 下校验
    std::condition_variable queueCv_;               // 队列非空/shutdown 唤醒，与 entriesMutex_ 配合 wait
    std::unordered_map<F0RequestKey, std::unique_ptr<ActiveEntry>> activeEntries_;
    mutable std::mutex entriesMutex_;   // 线性化 submit/shutdown/commit/worker 状态访问
    uint64_t tokenCounter_{1};          // entriesMutex_ 下分配
    bool shutdownStarted_{false};       // entriesMutex_ 下访问，worker 唯一退出状态

    std::vector<std::thread> workers_;

    // 本服务唯一创建并持有的 owner state：worker 以 execute(runOwnerState_) 调用，
    // shutdown 以同一 state 调 terminateActiveRun。shared_ptr 保证 state 在最后一次
    // 使用结束后才释放。
    std::shared_ptr<F0RunOwnerState> runOwnerState_;
    // 运行时惰性解析 F0InferenceService（进程级单例），消除冷启动空快照：
    // 首个 owner 构造时 F0 服务可能尚未初始化，直接缓存 shared_ptr 会得到空值。
    std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver_;

    void workerLoop();
};

} // namespace OpenTune
