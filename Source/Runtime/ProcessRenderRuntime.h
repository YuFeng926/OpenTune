#pragma once

#include "../Content/EditableContentSnapshot.h"
#include "../Inference/VocoderDomain.h"
#include "../Render/ContentRenderService.h"
#include "../Utils/VocoderModelWeight.h"
#include <condition_variable>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace OpenTune {

class ProcessRenderRuntime
{
public:
    static constexpr std::size_t kDeferredSnapshotBudgetBytes =
        static_cast<std::size_t>(6) * 1024 * 1024 * 1024;

    enum class ControlResult : uint8_t { Changed, Unchanged, Failed };
    using ControlCompletion = std::function<void(ControlResult, juce::String)>;
    enum class VocoderSubmitResult : uint8_t {
        Submitted,
        PayloadTooLarge,
        Retryable
    };

    struct CompletionGate
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool closed{false};
        int activeCallbacks{0};
        std::unordered_map<std::thread::id, int> activeCallbackThreads;

        bool tryEnter() noexcept
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed)
                return false;
            ++activeCallbacks;
            ++activeCallbackThreads[std::this_thread::get_id()];
            return true;
        }

        void leave() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                --activeCallbacks;
                const auto threadIt = activeCallbackThreads.find(std::this_thread::get_id());
                if (threadIt != activeCallbackThreads.end()) {
                    if (--threadIt->second == 0)
                        activeCallbackThreads.erase(threadIt);
                }
            }
            cv.notify_all();
        }

        void closeAndWait() noexcept
        {
            std::unique_lock<std::mutex> lock(mutex);
            closed = true;
            if (activeCallbackThreads.count(std::this_thread::get_id()) != 0)
                std::terminate();
            cv.wait(lock, [this] { return activeCallbacks == 0; });
        }

    };

    struct CompletionCallbackLease
    {
        explicit CompletionCallbackLease(CompletionGate& gate) noexcept
            : gate_(gate), entered_(gate_.tryEnter()) {}

        CompletionCallbackLease(const CompletionCallbackLease&) = delete;
        CompletionCallbackLease& operator=(const CompletionCallbackLease&) = delete;
        CompletionCallbackLease(CompletionCallbackLease&&) = delete;
        CompletionCallbackLease& operator=(CompletionCallbackLease&&) = delete;

        ~CompletionCallbackLease()
        {
            if (entered_)
                gate_.leave();
        }

        explicit operator bool() const noexcept { return entered_; }

        CompletionGate& gate_;
        bool entered_{false};
    };

    struct CompletionContext
    {
        std::shared_ptr<CompletionGate> gate;
        std::function<void(ContentKey,
                           std::shared_ptr<const EditableContentSnapshot>,
                           std::shared_ptr<const juce::AudioBuffer<float>>,
                           double)> chunkSettled;
        std::function<void(ContentKey, const juce::String&)> chunkFailed;
        // Application-level failure notification uses the same lifecycle gate.
        uint64_t contentRevision{0};
        std::function<void(ContentKey, uint64_t, uint64_t, const juce::String&,
                           std::shared_ptr<RenderCache>)> applicationFailure;
        std::shared_ptr<RenderCache> renderCache;
    };

    struct ControlTransaction
    {
        std::shared_ptr<ContentRenderService> crs;
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> finishing{false};
        std::atomic<bool> acked{false};
        std::atomic<bool> paused{false};
    };

    static ProcessRenderRuntime& getInstance();
    // Posts an application-level render failure notification. Safe to call from
    // a render worker thread; the completion gate suppresses teardown cancellation.
    static void notifyApplicationRenderFailure(ContentKey key,
                                                uint64_t contentRevision,
                                                uint64_t targetRevision,
                                                const juce::String& reason,
                                                std::shared_ptr<RenderCache> renderCache,
                                                std::shared_ptr<CompletionGate> gate,
                                                std::shared_ptr<std::atomic<bool>> reconfigurationNotificationClaim = {});
    void retainOwner();
    void releaseOwner() noexcept;

    /**
     * Final close. Standalone and VST3 call these from their exit lanes and
     * never reopen the runtime afterwards.
     *
     * shutdownAsync closes command acceptance, appends the single Stop behind
     * every accepted command, and returns. The control worker executes all
     * accepted commands, tears down Domain/Scheduler/DML, then posts one
     * message completion; that callback waits for the worker exit predicate,
     * unlocks, joins and cleans up before invoking completion.
     *
     * shutdownAndJoin uses the same Stop path without a message completion;
     * it waits on the mutex-paired worker-exit predicate, unlocks, joins, then
     * drains every accepted async delivery while the module is still loaded.
     * Callers may invoke it from the message thread or from any other thread:
     * all runtime deliveries queued before the final shutdown are executed
     * before it returns, and later deliveries are rejected, so no runtime
     * message-thread callback remains queued after module unload.
     */
    void shutdownAsync(std::function<void()> completion);
    void shutdownAndJoin();

    // Single entry for process-runtime message-thread deliveries. Normal
    // operation preserves juce::MessageManager::callAsync ordering; the final
    // shutdown observes every accepted delivery through this fence.
    static bool postAsyncCallback(std::function<void()> callback);

    // Registered by ContentRenderService along its execution lease attach and
    // detach. A successful shared backend change notifies every registered
    // service once on the message thread.
    void registerBackendChangeListener(std::weak_ptr<ContentRenderService> service);
    void unregisterBackendChangeListener(ContentRenderService* service);

    void processChunkRenderJob(std::shared_ptr<ContentRenderService> crs,
                               RenderJob& job,
                               bool lightPitchEnabled,
                               CompletionContext completion);

    /**
     * 异步模型切换 / 后端重置 API（UI 线程调用）：
     * 只投递命令到进程寿命 control worker 并立即返回。control worker 串行执行
     * 耗时 Session 销毁、按当前配置重建、AccelerationDetector resetAndDetect。
     * setVocoderModelWeight 返回 true 表示命令已在 control queue 锁内接受；
     * false 表示拒绝且不执行、不持久化。completion 经 MessageManager::callAsync
     * 投递回消息线程；调用方在其 completion gate 关闭后直接丢弃（不访问 owner）。
     */
    bool setVocoderModelWeight(const VocoderModelWeight& weight,
                               std::shared_ptr<ContentRenderService> crs,
                               ControlCompletion completion,
                               std::shared_ptr<CompletionGate> completionGate);
    void resetVocoder(std::shared_ptr<ContentRenderService> crs,
                      ControlCompletion completion);
    void resetInferenceBackend(bool forceCpu,
                               std::shared_ptr<ContentRenderService> crs,
                               ControlCompletion completion,
                               std::shared_ptr<CompletionGate> completionGate);
    void detachDeferredJobs(void* owner,
                            const std::shared_ptr<CompletionGate>& gate);
    void pruneDeferredJobsForCache(const std::shared_ptr<RenderCache>& cache);
    void pruneStaleDeferredJobsForCache(const std::shared_ptr<RenderCache>& cache);
    void pruneDeferredJobsForService(const ContentRenderService* service);
    void renderQueueCapacityAvailable();
    bool hasDeferredRetriesForService(const ContentRenderService* service) const;
    bool flushDeferredRetriesForService(const ContentRenderService* service);

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
    VocoderSubmitResult submitVocoderJob(VocoderDomain::Job job,
                                          uint64_t expectedGeneration,
                                          juce::String& rejectionReason);
    VocoderModelWeight getVocoderModelWeight() const;
    bool isVocoderReady() const noexcept;
    bool isVocoderReconfiguring() const noexcept;

    std::size_t controlQueueDepth() const noexcept;
    std::size_t deferredRetryCount() const noexcept;
    std::size_t deferredRetrySnapshotBytes() const;
    bool hasActiveTransaction() const noexcept;
    int domainSubmitInFlight() const noexcept;
    uint64_t vocoderGeneration() const noexcept;
    int ownerCount() const noexcept;
    bool isControlWorkerJoinable() const noexcept;

private:
    friend struct ProcessRenderRuntimeTestAccess;

    ProcessRenderRuntime();
    ~ProcessRenderRuntime() = default;

    static std::string modelPathForWeight(const std::string& modelDir, const VocoderModelWeight& weight);

    // 在锁外创建并初始化完整 domain。模型加载、Session 创建和失败清理均不得
    // 持有 vocoderMutex_，保证 UI 查询与实例 detach 永远只经历短临界区。
    std::unique_ptr<VocoderDomain> createVocoderDomain(const VocoderModelWeight& weight,
                                                       juce::String& errorMessage);

    // 一次锁内“确保 domain 并返回 generation/conditioningBins/fMax”：配置与
    // domain 同代生成，杜绝跨域混用（旧 generation 配置配新 domain 等）。
    struct VocoderConfig
    {
        uint64_t generation{0};
        VocoderModelWeight modelWeight{kDefaultVocoderWeight};
        int conditioningBins{0};
        VocoderConditioningType conditioningType{VocoderConditioningType::LogMel};
        float fMax{16000.0f};
        MelFilterbankSpec melFilterbank;
        float melLogEps{1.0e-5f};
        bool needsHighFrequencyOriginal{false};
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
        ControlCompletion completion;
        std::shared_ptr<CompletionGate> completionGate;
        bool shutdownPostCompletion{false};
        uint64_t shutdownGeneration{0};
    };

    struct DeferredFailureNotification
    {
        ContentKey contentKey;
        uint64_t contentRevision{0};
        uint64_t targetRevision{0};
        juce::String reason;
        std::shared_ptr<RenderCache> renderCache;
        std::shared_ptr<CompletionGate> gate;
    };

    // control worker 唯一重配入口：短锁内摘除旧 domain 并推进 generation，
    // 锁外销毁旧 Session，随后锁外创建新 Session，最后短锁发布。
    ControlResult reconfigureVocoder(
        const ControlCommand& command,
        juce::String& errorMessage,
        std::vector<DeferredFailureNotification>& deferredFailureNotifications);

    void controlWorkerLoop();
    bool postControlCommand(ControlCommand command);
    void startControlWorker();
    // Requires shutdownMutex_. Closes acceptance, appends the single Stop
    // behind every accepted command and starts the worker when needed.
    void beginShutdownLocked(bool postCompletion);
    void completeAsyncShutdown(uint64_t generation);
    bool postAsyncDelivery(std::function<void()> callback);
    void drainAsyncDeliveries();
    void finishShutdown(uint64_t generation);
    void shutdownLocked(std::unique_lock<std::mutex>& shutdownLock);
    void postBackendChangeNotification();
    void notifyBackendChangeListeners();
    static bool claimControlTransaction(const std::shared_ptr<ControlTransaction>& transaction) noexcept;
    static void completeClaimedControlTransaction(const std::shared_ptr<ControlTransaction>& transaction,
                                                  const char* reason) noexcept;
    static void finishControlTransaction(const std::shared_ptr<ControlTransaction>& transaction,
                                         const char* reason) noexcept;
    static void waitForControlTransactionAck(const std::shared_ptr<ControlTransaction>& transaction);

    std::thread controlWorker_;
    mutable std::mutex controlMutex_;
    std::condition_variable controlCv_;
    std::condition_variable workerExitCv_;
    std::deque<ControlCommand> controlQueue_;
    mutable std::mutex shutdownMutex_;
    bool ensureVocoderQueued_{false}; // controlMutex_ protected
    bool acceptingCommands_{true}; // controlMutex_ protected
    bool stopQueued_{false}; // controlMutex_ protected
    std::atomic<bool> deferredEnsureRetryPending_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<uint64_t> shutdownGeneration_{0};
    bool workerExited_{false}; // shutdownMutex_ protected
    bool workerExitDispatchPosted_{false}; // shutdownMutex_ protected
    bool shutdownFinishStarted_{false}; // shutdownMutex_ protected
    bool shutdownFinished_{false}; // shutdownMutex_ protected
    std::function<void()> shutdownCompletion_; // shutdownMutex_ protected
    std::mutex asyncDeliveryMutex_;
    std::condition_variable asyncDeliveryCv_;
    bool asyncDeliveryClosed_{false}; // asyncDeliveryMutex_ protected
    int asyncPostingCount_{0}; // asyncDeliveryMutex_ protected
    int asyncOutstandingCount_{0}; // asyncDeliveryMutex_ protected
    int ownerCount_{0}; // shutdownMutex_ protected
    std::shared_ptr<ControlTransaction> activeTransaction_; // controlMutex_ protected
    std::mutex backendListenersMutex_;
    std::vector<std::weak_ptr<ContentRenderService>> backendListeners_; // backendListenersMutex_ protected

    std::unique_ptr<VocoderDomain> vocoderDomain_;
    VocoderModelWeight currentVocoderModelWeight_{kDefaultVocoderWeight};
    mutable std::mutex vocoderMutex_;
    std::condition_variable domainSubmitCv_;
    int domainSubmitInFlight_{0}; // vocoderMutex_ protected
    bool vocoderReconfiguring_{false}; // vocoderMutex_ 保护：control worker 已摘除旧 domain
    uint64_t vocoderGeneration_{0};  // vocoderMutex_ 保护

    // Deferred retry list: retains the latest retry per cache/chunk until it is
    // atomically admitted by RenderWorker or pruned as stale.
    struct DeferredRetry {
        std::weak_ptr<ContentRenderService> crs;
        RenderJob job;
        CompletionContext completion;
        void* ownerIdentity{nullptr};
        const ContentRenderService* serviceIdentity{nullptr};
        uint64_t audioRevision{0};
        int audioSamples{0};
        int audioChannels{0};
        juce::String reason;
        struct PayloadAllocation {
            const void* identity{nullptr};
            std::size_t bytes{0};
        };
        std::vector<PayloadAllocation> payloads;
    };

    bool deferredSnapshotFitsLocked(const DeferredRetry& retry,
                                    std::size_t budgetBytes) const;
    void retainDeferredPayloadsLocked(const DeferredRetry& retry);
    void releaseDeferredPayloadsLocked(const DeferredRetry& retry);
    bool resumeDeferredRetries(const ContentRenderService* serviceIdentity = nullptr);
    void postDeferredRetryEvent();
    std::vector<DeferredRetry> deferredRetries_; // vocoderMutex_ protected
    struct DeferredPayloadReference {
        std::size_t bytes{0};
        std::size_t references{0};
    };
    std::unordered_map<const void*, DeferredPayloadReference> deferredPayloads_; // vocoderMutex_ protected
    std::size_t deferredSnapshotBytes_{0}; // unique payload bytes; vocoderMutex_ protected
    static constexpr std::size_t kMaxControlQueueDepth = 100;
    std::atomic<bool> deferredRetryEventPosted_{false};

    void deferOrRequeue(std::shared_ptr<ContentRenderService> crs,
                        RenderJob job,
                        CompletionContext completion,
                        juce::String reason,
                        std::size_t snapshotBudgetBytes = kDeferredSnapshotBudgetBytes);

    ProcessRenderRuntime(const ProcessRenderRuntime&) = delete;
    ProcessRenderRuntime& operator=(const ProcessRenderRuntime&) = delete;
};

} // namespace OpenTune
