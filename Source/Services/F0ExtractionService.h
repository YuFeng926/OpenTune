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
    // 实例析构只关闭 owner（shutdown()）：不 join worker。worker 是 detached
    // 进程常驻执行器，捕获 shared state 而非 service this；owner 销毁后自行结束，
    // 期间只访问进程寿命的 F0InferenceService 与纯数据 execute，绝不访问已析构
    // 的 service。
    ~F0ExtractionService();

    /// 显式幂等关闭：停止接受新提交、丢弃排队 job、清空 active 表（shutdown 后
    /// isActive() 恒为 false）、关闭 ownerState 并终止本 owner 的活跃 F0 Run
    /// （SetTerminate 使已 dequeue 的 execute 快速返回）。不 join worker：worker
    /// 见 shutdownStarted_ 后自行退出。关闭开始后完成的任务不再投递 commit。
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

    // worker 线程执行状态全部放入 shared state：worker 捕获 state（shared_ptr）
    // 而非 service this；owner 销毁后 state 由 worker 自身持有直到其退出，因此
    // worker 永不触碰已析构的 service。
    struct SharedState {
        std::deque<Task> queue_;                        // entriesMutex_ 下访问；容量合同 maxQueueSize_
        size_t maxQueueSize_{0};                        // 真实容量合同，entriesMutex_ 下校验
        std::condition_variable queueCv_;               // 队列非空/shutdown 唤醒，与 entriesMutex_ 配合 wait
        std::unordered_map<F0RequestKey, std::unique_ptr<ActiveEntry>> activeEntries_;
        mutable std::mutex entriesMutex_;   // 线性化 submit/shutdown/commit/worker 状态访问
        uint64_t tokenCounter_{1};          // entriesMutex_ 下分配
        bool shutdownStarted_{false};       // entriesMutex_ 下访问，worker 唯一退出状态

        // 本服务唯一创建并持有的 owner state：worker 以 execute(runOwnerState) 调用，
        // shutdown 以同一 state 调 terminateActiveRun。shared_ptr 保证 state 在最后一次
        // 使用结束后才释放。
        std::shared_ptr<F0RunOwnerState> runOwnerState_;
    };

    std::shared_ptr<SharedState> state_;
    // 运行时惰性解析 F0InferenceService（进程级单例），消除冷启动空快照：
    // 首个 owner 构造时 F0 服务可能尚未初始化，直接缓存 shared_ptr 会得到空值。
    std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver_;

    static void workerLoop(std::shared_ptr<SharedState> state);
};

} // namespace OpenTune
