#pragma once

#include "../Content/EditableContentSnapshot.h"
#include "../Inference/VocoderDomain.h"
#include "../Render/ContentRenderService.h"
#include "../Utils/VocoderModelWeight.h"
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace OpenTune {

class ProcessRenderRuntime
{
public:
    enum class ControlResult : uint8_t { Changed, Unchanged, Failed };

    struct CompletionGate
    {
        std::mutex mutex;
        bool closed{false};
    };

    struct CompletionContext
    {
        std::shared_ptr<CompletionGate> gate;
        std::function<void(ContentKey,
                           std::shared_ptr<const EditableContentSnapshot>,
                           std::shared_ptr<const juce::AudioBuffer<float>>,
                           double)> chunkSettled;
        std::function<void(ContentKey)> chunkFailed;
    };

    struct ControlTransaction
    {
        std::shared_ptr<ContentRenderService> crs;
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> finishing{false};
        std::atomic<bool> closeRequested{false};
        std::atomic<bool> acked{false};
    };

    static ProcessRenderRuntime& getInstance();
    void retainOwner();
    void releaseOwner() noexcept;
    void shutdown();

    void processChunkRenderJob(std::shared_ptr<ContentRenderService> crs,
                               RenderJob& job,
                               bool lightPitchEnabled,
                               CompletionContext completion);

    /**
     * 异步模型切换 / 后端重置 API（UI 线程调用）：
     * 只投递命令到进程寿命 control worker 并立即返回。control worker 串行执行
     * 耗时 Session 销毁、按当前配置重建、AccelerationDetector resetAndDetect。
     * completion 经 MessageManager::callAsync 投递回消息线程；调用方在其
     * completion gate 关闭后直接丢弃（不访问 owner）。
     */
    void setVocoderModelWeight(const VocoderModelWeight& weight,
                               std::shared_ptr<ContentRenderService> crs,
                               std::function<void(ControlResult)> completion);
    void resetVocoder(std::shared_ptr<ContentRenderService> crs,
                      std::function<void(ControlResult)> completion);
    void resetInferenceBackend(bool forceCpu,
                               std::shared_ptr<ContentRenderService> crs,
                               std::function<void(ControlResult)> completion);

    /**
     * Vocoder submission / query entry points. All accesses to the underlying
     * VocoderDomain go through these (each locks vocoderMutex_), so a
     * concurrent control-worker reset/rebuild can never destroy the domain
     * while it is being used (no raw pointer escapes).
     *
     * submitVocoderJob requires expectedGeneration to match the current domain
     * generation: the caller captures the full configuration
     * (generation/conditioningBins/conditioningType/fMax) in a single locked
     * snapshot via acquireVocoderConfig(), so a job is never submitted to a
     * domain rebuilt since then with stale configuration.
     */
    bool submitVocoderJob(VocoderDomain::Job job, uint64_t expectedGeneration);
    bool isVocoderReady() const noexcept;
    bool isVocoderReconfiguring() const noexcept;

private:
    ProcessRenderRuntime();
    ~ProcessRenderRuntime();

    static std::string modelPathForWeight(const std::string& modelDir, const VocoderModelWeight& weight);

    // 在锁外创建并初始化完整 domain。模型加载、Session 创建和失败清理均不得
    // 持有 vocoderMutex_，保证 UI 查询与实例 detach 永远只经历短临界区。
    std::unique_ptr<VocoderDomain> createVocoderDomain(const VocoderModelWeight& weight);

    // 一次锁内“确保 domain 并返回 generation/conditioningBins/fMax”：配置与
    // domain 同代生成，杜绝跨域混用（旧 generation 配置配新 domain 等）。
    struct VocoderConfig
    {
        uint64_t generation{0};
        int conditioningBins{0};
        VocoderConditioningType conditioningType{VocoderConditioningType::LogMel};
        float fMax{16000.0f};
    };
    bool acquireVocoderConfig(VocoderConfig& out);

    // ---- 进程寿命 control worker：模型切换/后端重置的唯一执行者 ----
    struct ControlCommand
    {
        enum class Type : uint8_t
        {
            EnsureVocoder,
            SetVocoderWeight,
            ResetVocoder,
            ResetInferenceBackend,
            Stop
        };

        Type type{Type::ResetVocoder};
        VocoderModelWeight weight{kDefaultVocoderWeight};
        bool forceCpu{false};
        std::shared_ptr<ControlTransaction> transaction;
        std::function<void(ControlResult)> completion;
    };

    // control worker 唯一重配入口：短锁内摘除旧 domain 并推进 generation，
    // 锁外销毁旧 Session，随后锁外创建新 Session，最后短锁发布。
    ControlResult reconfigureVocoder(const ControlCommand& command);

    void controlWorkerLoop();
    void postControlCommand(ControlCommand command);
    void finishShutdown();
    static bool claimControlTransaction(const std::shared_ptr<ControlTransaction>& transaction) noexcept;
    static void completeClaimedControlTransaction(const std::shared_ptr<ControlTransaction>& transaction,
                                                  const char* reason) noexcept;
    static void finishControlTransaction(const std::shared_ptr<ControlTransaction>& transaction,
                                         const char* reason) noexcept;

    std::thread controlWorker_;
    std::mutex controlMutex_;
    std::condition_variable controlCv_;
    std::condition_variable workerExitCv_;
    std::deque<ControlCommand> controlQueue_;
    std::mutex shutdownMutex_;
    bool ensureVocoderQueued_{false}; // controlMutex_ protected
    std::atomic<bool> shuttingDown_{false};
    std::atomic<bool> workerExitDispatchAttempted_{false};
    std::atomic<bool> workerExitDispatchPosted_{false};
    std::atomic<int> ownerCount_{0};
    std::shared_ptr<ControlTransaction> activeTransaction_;

    std::unique_ptr<VocoderDomain> vocoderDomain_;
    VocoderModelWeight currentVocoderModelWeight_{kDefaultVocoderWeight};
    mutable std::mutex vocoderMutex_;
    std::condition_variable domainSubmitCv_;
    int domainSubmitInFlight_{0}; // vocoderMutex_ protected
    bool vocoderReconfiguring_{false}; // vocoderMutex_ 保护：control worker 已摘除旧 domain
    uint64_t vocoderGeneration_{0};  // vocoderMutex_ 保护

    // Deferred retry list: jobs that failed with generation mismatch or cancelled
    // during reconfigureVocoder. Flushed after new domain is published and
    // reconfiguring_ is cleared.
    struct DeferredRetry {
        std::weak_ptr<ContentRenderService> crs;
        RenderJob job;
        CompletionContext completion;
    };

    void failDeferredRetries(std::vector<DeferredRetry> retries);
    std::vector<DeferredRetry> deferredRetries_; // vocoderMutex_ protected

    void deferOrRequeue(std::shared_ptr<ContentRenderService> crs,
                        RenderJob job,
                        CompletionContext completion);

    ProcessRenderRuntime(const ProcessRenderRuntime&) = delete;
    ProcessRenderRuntime& operator=(const ProcessRenderRuntime&) = delete;
};

} // namespace OpenTune
