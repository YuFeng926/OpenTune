#include "F0InferenceService.h"
#include "ModelFactory.h"
#include "FCPEExtractor.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <condition_variable>

namespace OpenTune {

class F0InferenceService::Impl {
public:
    Impl(std::shared_ptr<Ort::Env> env) : env_(std::move(env)) {
        resamplingManager_ = std::make_shared<ResamplingManager>();
    }

    // initialize() 只保存配置并做文件存在性检查，不创建 ONNX session。
    // session 在 extractF0() 内按需创建、调用返回前析构。
    bool initialize(const std::string& modelDir, F0ModelType initialModel, std::string* errorMessage) {
        std::unique_lock<std::mutex> lock(extractorMutex_);

        if (initialized_.load(std::memory_order_acquire))
            return true;

        if (!ModelFactory::isModelAvailable(initialModel, modelDir)) {
            if (errorMessage != nullptr)
                *errorMessage = "F0 model file not found: " + ModelFactory::getModelPath(initialModel, modelDir);
            AppLogger::error("[F0InferenceService] F0 model not available: "
                + juce::String(ModelFactory::getModelPath(initialModel, modelDir)));
            return false;
        }

        modelDir_ = modelDir;
        currentModelType_ = initialModel;
        initialized_.store(true, std::memory_order_release);

        AppLogger::info("[F0InferenceService] Configured with FCPE model (session created on demand)");
        return true;
    }

    F0ExtractionConfigSnapshot snapshotExtractionConfig() const
    {
        std::lock_guard<std::mutex> lock(extractorMutex_);
        F0ExtractionConfigSnapshot config;
        config.modelType = currentModelType_;
        config.modelDirectory = modelDir_;
        config.confidence = confidenceThreshold_;
        config.f0Min = f0Min_;
        config.f0Max = f0Max_;
        config.backend = AccelerationDetector::getInstance().getSelection();
        config.selectionRevision = selectionRevision_;
        if (cpuFallbackSticky_.load(std::memory_order_acquire))
        {
            config.backend.backend = AccelerationDetector::AccelBackend::CPU;
            config.backend.dmlAdapterIndex = 0;
        }
        return config;
    }

    Result<std::vector<float>> extractF0(
        const float* audio,
        size_t length,
        int sampleRate,
        std::shared_ptr<F0RunOwnerState> ownerState,
        std::function<void(float)> progressCallback,
        std::function<void(const std::vector<float>&, int)> partialCallback,
        const F0ExtractionConfigSnapshot* requestedConfig = nullptr,
        const F0ModelVersion* requestedModel = nullptr,
        int* actualHopSize = nullptr,
        int* actualSampleRate = nullptr,
        std::string* actualBackend = nullptr)
    {
        // DML 合同：同一 Session 同一时刻仅允许一个线程 Run()。
        // admission gate：pendingLeases_ 排队，runCv_ 唤醒；无活跃 Run 或自身
        // 已被 terminateActiveRun 取消时入场。
        auto lease = std::make_shared<RunLease>();
        lease->ownerState = std::move(ownerState);

        std::unique_lock<std::mutex> runLock(runMutex_);
        // 晚到 lease 直接拒绝：owner state 已持久关闭（一次关闭、永不重新入场）。
        if (lease->ownerState->closed.load(std::memory_order_acquire)) {
            return Result<std::vector<float>>::failure(
                ErrorCode::OperationCancelled, "F0InferenceService owner closed");
        }
        pendingLeases_.push_back(lease);
        runCv_.wait(runLock, [&] {
            return !runInProgress_ || lease->cancelled;
        });

        // 从等待队列移除自身（无论入场还是被取消）
        {
            auto it = std::find(pendingLeases_.begin(), pendingLeases_.end(), lease);
            if (it != pendingLeases_.end())
                pendingLeases_.erase(it);
        }

        // 已取消的 waiter 直接退出，不占用 Run 槽位
        if (lease->cancelled) {
            return Result<std::vector<float>>::failure(
                ErrorCode::OperationCancelled, "F0InferenceService run cancelled");
        }

        // 成为活跃 Run。UnsetTerminate 与 terminateActiveRun 的 SetTerminate
        // 均在同一 runMutex_ 下完成，无 TOCTOU。
        runInProgress_ = true;
        activeLease_ = lease;
        runOptions_.UnsetTerminate();
        runLock.unlock();

        // RAII：Run 结束后复位锁内状态并唤醒下一个 waiter。
        // 析构顺序保证：局部 extractor（含 session）先析构，然后本 guard 触发
        // notify_all 唤醒下一个 waiter，满足 DML 约束（session 释放先于 waiter 唤醒）。
        struct RunStateGuard {
            std::mutex& mutex;
            std::condition_variable& cv;
            bool& inProgress;
            std::shared_ptr<RunLease>& activeLease;
            ~RunStateGuard() {
                {
                    std::lock_guard<std::mutex> lk(mutex);
                    inProgress = false;
                    activeLease.reset();
                }
                cv.notify_all();
            }
        } stateGuard{runMutex_, runCv_, runInProgress_, activeLease_};

        F0ExtractionConfigSnapshot config;
        {
            std::lock_guard<std::mutex> lock(extractorMutex_);
            if (!initialized_.load(std::memory_order_acquire)) {
                return Result<std::vector<float>>::failure(
                    ErrorCode::NotInitialized, "F0InferenceService not configured");
            }
            if (requestedConfig == nullptr) {
                config.modelType = currentModelType_;
                config.modelDirectory = modelDir_;
                config.confidence = confidenceThreshold_;
                config.f0Min = f0Min_;
                config.f0Max = f0Max_;
            }
        }
        if (requestedConfig != nullptr)
            config = *requestedConfig;

        Result<std::vector<float>> result = Result<std::vector<float>>::failure(
            ErrorCode::ModelInferenceFailed, "F0 extraction did not produce a result");

        // 按需创建局部 extractor，extractF0 完成后在本作用域退出时析构（含 session），
        // 早于 RunStateGuard 析构，确保 session 释放先于唤醒下一个 waiter。
        // 创建/销毁均在调用方工作线程完成，不在音频线程/UI 线程。
        {
            // 运行期 DML 失败后的 sticky CPU fallback：一旦某次 FCPE 提取
            // 发生 DML Run 失败，本 service 生命周期内的后续提取全部走 CPU，
            // 避免每个任务重复 DML 失败。不影响全局 AccelerationDetector，
            // 也不影响 vocoder。
            auto backend = requestedConfig != nullptr
                ? config.backend : AccelerationDetector::getInstance().getSelection();
            if (requestedConfig == nullptr && cpuFallbackSticky_.load(std::memory_order_acquire))
                backend = {};
            const auto model = requestedModel != nullptr
                ? *requestedModel : ModelFactory::resolveF0ModelVersion(config.modelType, config.modelDirectory);
            auto extractorResult = ModelFactory::createF0Extractor(
                config.modelType, model, *env_, resamplingManager_, backend);

            if (!extractorResult) {
                AppLogger::error("[F0InferenceService] Failed to create F0 extractor: "
                    + juce::String(extractorResult.error().fullMessage()));
                result = Result<std::vector<float>>::failure(extractorResult.error());
            } else {
                auto extractor = std::move(extractorResult).value();
                const auto updateActualBackend = [&] {
                    if (actualBackend == nullptr)
                        return;
                    *actualBackend = AccelerationDetector::backendName(backend.backend);
                    if (const auto* fcpe = dynamic_cast<const FCPEExtractor*>(extractor.get());
                        fcpe != nullptr && !fcpe->isGpuActive())
                        *actualBackend = "CPU";
                };
                updateActualBackend();

                // 缓存 hop/sample rate 供 getter 在 session 释放后使用
                const int hopSize = extractor->getHopSize();
                const int targetSampleRate = extractor->getTargetSampleRate();
                if (actualHopSize != nullptr)
                    *actualHopSize = hopSize;
                if (actualSampleRate != nullptr)
                    *actualSampleRate = targetSampleRate;
                {
                    std::unique_lock<std::mutex> elock(extractorMutex_);
                    f0HopSize_ = hopSize;
                    f0SampleRate_ = targetSampleRate;
                }

                if (hopSize <= 0 || targetSampleRate <= 0) {
                    result = Result<std::vector<float>>::failure(
                        ErrorCode::InvalidParameter,
                        "F0 extractor returned invalid coordinates (hopSize="
                            + std::to_string(hopSize) + ", sampleRate="
                            + std::to_string(targetSampleRate) + ")");
                    return result;
                }

                // 应用用户配置的参数
                extractor->setConfidenceThreshold(config.confidence);
                extractor->setF0Min(config.f0Min);
                extractor->setF0Max(config.f0Max);

                try {
                    // CPU 重试授权回调：FCPE 在 DML Run 失败后请求建立 CPU
                    // session 前，必须在 runMutex_ 下确认本 lease 仍是
                    // activeLease_ 且 ownerState 未关闭——与 terminateActiveRun
                    // 的 SetTerminate 共享同一把锁，构成重试与 terminate 的
                    // 线性化顺序。terminate 赢得锁时回调返回 false，DML
                    // 失败以取消语义抛出（RetryCancelled），不再建立 CPU session。
                    if (auto* fcpe = dynamic_cast<FCPEExtractor*>(extractor.get())) {
                        auto selfLease = lease;
                        fcpe->setCpuRetryAuthorize([this, selfLease]() -> bool {
                            std::lock_guard<std::mutex> lk(runMutex_);
                            return activeLease_ == selfLease
                                && !selfLease->ownerState->closed.load(std::memory_order_acquire);
                        });
                    }

                    auto f0 = extractor->extractF0(audio, length, sampleRate,
                                                    runOptions_, progressCallback, partialCallback);
                    updateActualBackend();
                    result = Result<std::vector<float>>::success(f0);
                } catch (const FCPEExtractor::RetryCancelled&) {
                    // CPU retry 被授权 gate 拒绝（terminate 赢得 runMutex_）：
                    // 按取消返回，而非 ModelInferenceFailed。
                    AppLogger::warn("[F0InferenceService] F0 run cancelled before CPU retry");
                    result = Result<std::vector<float>>::failure(
                        ErrorCode::OperationCancelled,
                        "F0 run terminated before CPU retry");
                } catch (const Ort::Exception& e) {
                    // 仅 ONNX 标准的 SetTerminate() 消息映射为取消；
                    // 其它异常保持 ModelInferenceFailed 并传播原始诊断。
                    constexpr char kTerminateMessage[] =
                        "Exiting due to terminate flag being set to true.";
                    if (std::string(e.what()).find(kTerminateMessage) != std::string::npos) {
                        AppLogger::warn("[F0InferenceService] F0 run terminated via RunOptions");
                        result = Result<std::vector<float>>::failure(
                            ErrorCode::OperationCancelled,
                            "F0 run terminated: " + std::string(e.what()));
                    } else {
                        AppLogger::error("[F0InferenceService] ORT exception during F0 extraction: "
                            + juce::String(e.what()));
                        result = Result<std::vector<float>>::failure(
                            ErrorCode::ModelInferenceFailed,
                            "F0 inference ORT exception: " + std::string(e.what()));
                    }
                } catch (const std::exception& e) {
                    AppLogger::error("[F0InferenceService] Exception during F0 extraction: "
                        + juce::String(e.what()));
                    result = Result<std::vector<float>>::failure(
                        ErrorCode::ModelInferenceFailed,
                        "F0 inference threw std::exception: " + std::string(e.what()));
                } catch (...) {
                    AppLogger::error("[F0InferenceService] Exception during F0 extraction: unknown exception");
                    result = Result<std::vector<float>>::failure(
                        ErrorCode::ModelInferenceFailed, "F0 inference threw an unknown exception");
                }

                // 本轮提取结束时检查：若 extractor 经历了 DML Run 失败（无论
                // CPU 重试是否成功），sticky 置位，后续提取直接走 CPU。
                if (auto* fcpe = dynamic_cast<FCPEExtractor*>(extractor.get())) {
                    if (fcpe->didDmlRunFail()) {
                        std::lock_guard<std::mutex> lock(extractorMutex_);
                        if (!cpuFallbackSticky_.exchange(true, std::memory_order_acq_rel))
                            ++selectionRevision_;
                    }
                }
            }
        }
        // extractor 在此处析构，session 释放；RunStateGuard 尚未析构。

        return result;
    }

    void terminateActiveRun(const std::shared_ptr<F0RunOwnerState>& ownerState) {
        // 持久关闭：state->closed 一旦置位永不恢复。与 lease 取消、SetTerminate
        // 全部在同一状态锁下：晚到的 extractF0 入场检查（同锁）必能看到 closed。
        std::lock_guard<std::mutex> lk(runMutex_);
        ownerState->closed.store(true, std::memory_order_release);
        for (auto& lease : pendingLeases_)
            if (lease->ownerState == ownerState)
                lease->cancelled = true;
        if (activeLease_ && activeLease_->ownerState == ownerState)
            runOptions_.SetTerminate();
        runCv_.notify_all();
    }

    // setF0Model() 只更新所选模型（检查文件存在），不创建 session。
    // 正在进行的 extractF0() 调用继续使用已快照的旧配置，
    // 下一次调用使用新模型；不同时存在新旧 session。
    bool setF0Model(F0ModelType type, std::string* errorMessage) {
        std::unique_lock<std::mutex> lock(extractorMutex_);

        if (type == currentModelType_)
            return true;

        if (!ModelFactory::isModelAvailable(type, modelDir_)) {
            if (errorMessage != nullptr)
                *errorMessage = "F0 model file not found: " + ModelFactory::getModelPath(type, modelDir_);
            AppLogger::error("[F0InferenceService] F0 model not available: "
                + juce::String(ModelFactory::getModelPath(type, modelDir_)));
            return false;
        }

        currentModelType_ = type;
        ++selectionRevision_;
        AppLogger::info("[F0InferenceService] Selected model: FCPE (session will be created on next extraction)");
        return true;
    }

    int getF0HopSize() const {
        std::lock_guard<std::mutex> lock(extractorMutex_);
        return f0HopSize_;
    }

    int getF0SampleRate() const {
        std::lock_guard<std::mutex> lock(extractorMutex_);
        return f0SampleRate_;
    }

private:
    // Run lease + admission gate：runMutex_ 是唯一状态锁，线性化入场、取消与
    // RunOptions 切换；runCv_ 唤醒被取消的 waiter 和下一个入场者。
    struct RunLease {
        std::shared_ptr<F0RunOwnerState> ownerState;
        bool cancelled{false};
    };

    std::shared_ptr<Ort::Env> env_;
    std::shared_ptr<ResamplingManager> resamplingManager_;
    // Sticky per-service CPU fallback after a runtime DML Run failure.
    std::atomic<bool> cpuFallbackSticky_{false};
    F0ModelType currentModelType_{F0ModelType::FCPE};
    std::string modelDir_;
    mutable std::mutex extractorMutex_;   // guards config fields (no session)
    std::atomic<bool> initialized_{false};
    // FCPE 默认值；session 释放后 getter 仍返回正确值。
    // set 参数后下一次 extractF0 创建的新 session 应继承。
    uint64_t selectionRevision_{0};
    float confidenceThreshold_{0.006f};
    float f0Min_{32.7f};
    float f0Max_{1975.5f};
    int f0HopSize_{160};
    int f0SampleRate_{16000};

    // DML 合同：同一 Session 同一时刻仅允许一个线程 Run()
    std::mutex runMutex_;
    std::condition_variable runCv_;
    bool runInProgress_{false};
    std::shared_ptr<RunLease> activeLease_;
    std::vector<std::shared_ptr<RunLease>> pendingLeases_;
    Ort::RunOptions runOptions_;
};

F0InferenceService::F0InferenceService(std::shared_ptr<Ort::Env> env)
    : pImpl_(std::make_unique<Impl>(std::move(env))) {}

F0InferenceService::~F0InferenceService() = default;

bool F0InferenceService::initialize(const std::string& modelDir,
                                    F0ModelType initialModel,
                                    std::string* errorMessage) {
    return pImpl_->initialize(modelDir, initialModel, errorMessage);
}

Result<std::vector<float>> F0InferenceService::extractF0(
    const float* audio,
    size_t length,
    int sampleRate,
    std::shared_ptr<F0RunOwnerState> ownerState,
    std::function<void(float)> progressCallback,
    std::function<void(const std::vector<float>&, int)> partialCallback)
{
    return pImpl_->extractF0(audio, length, sampleRate, std::move(ownerState),
                             std::move(progressCallback), std::move(partialCallback));
}

F0ExtractionConfigSnapshot F0InferenceService::snapshotExtractionConfig() const
{
    return pImpl_->snapshotExtractionConfig();
}

Result<std::vector<float>> F0InferenceService::extractF0ForAnalysis(
    const float* audio,
    size_t length,
    int sampleRate,
    std::shared_ptr<F0RunOwnerState> ownerState,
    const F0ExtractionConfigSnapshot& config,
    const F0ModelVersion& model,
    int& actualHopSize,
    int& actualSampleRate,
    std::string& actualBackend)
{
    return pImpl_->extractF0(audio, length, sampleRate, std::move(ownerState), {}, {},
                             &config, &model, &actualHopSize, &actualSampleRate, &actualBackend);
}

void F0InferenceService::terminateActiveRun(const std::shared_ptr<F0RunOwnerState>& ownerState) {
    pImpl_->terminateActiveRun(ownerState);
}

bool F0InferenceService::setF0Model(F0ModelType type, std::string* errorMessage) {
    return pImpl_->setF0Model(type, errorMessage);
}

int F0InferenceService::getF0HopSize() const {
    return pImpl_->getF0HopSize();
}

int F0InferenceService::getF0SampleRate() const {
    return pImpl_->getF0SampleRate();
}

} // namespace OpenTune
