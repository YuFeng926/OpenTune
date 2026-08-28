#include "F0InferenceService.h"
#include "ModelFactory.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/AppLogger.h"
#include <onnxruntime_cxx_api.h>
#include <shared_mutex>
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
    bool initialize(const std::string& modelDir, F0ModelType initialModel) {
        std::unique_lock<std::shared_mutex> lock(extractorMutex_);

        if (initialized_.load(std::memory_order_acquire))
            return true;

        if (!ModelFactory::isModelAvailable(initialModel, modelDir)) {
            AppLogger::error("[F0InferenceService] F0 model not available: "
                + juce::String(ModelFactory::getModelPath(initialModel, modelDir)));
            return false;
        }

        modelDir_ = modelDir;
        currentModelType_ = initialModel;
        initialized_.store(true, std::memory_order_release);

#if defined(__APPLE__)
        AppLogger::info("[F0InferenceService] Configured with "
            + juce::String(initialModel == F0ModelType::FCPE ? "FCPE" : "RMVPE")
            + " model (CoreML, session created on demand)");
#else
        AppLogger::info("[F0InferenceService] Configured with "
            + juce::String(initialModel == F0ModelType::FCPE ? "FCPE" : "RMVPE")
            + " model (CPU/DML, session created on demand)");
#endif
        return true;
    }

    Result<std::vector<float>> extractF0(
        const float* audio,
        size_t length,
        int sampleRate,
        std::shared_ptr<F0RunOwnerState> ownerState,
        std::function<void(float)> progressCallback,
        std::function<void(const std::vector<float>&, int)> partialCallback)
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

        // 在获得 admission 后快照当前配置。排队期间的模型切换因此会作用于
        // 本次实际创建的 session；配置在本调用内保持不变。
        struct ExtractionConfig {
            F0ModelType modelType;
            std::string modelDir;
            float confidence;
            float f0Min;
            float f0Max;
        };

        ExtractionConfig config;
        {
            std::shared_lock<std::shared_mutex> lock(extractorMutex_);
            if (!initialized_.load(std::memory_order_acquire)) {
                return Result<std::vector<float>>::failure(
                    ErrorCode::NotInitialized, "F0InferenceService not configured");
            }
            config.modelType = currentModelType_;
            config.modelDir = modelDir_;
            config.confidence = confidenceThreshold_;
            config.f0Min = f0Min_;
            config.f0Max = f0Max_;
        }

        Result<std::vector<float>> result = Result<std::vector<float>>::failure(
            ErrorCode::ModelInferenceFailed, "F0 extractor creation failed");

        // 按需创建局部 extractor，extractF0 完成后在本作用域退出时析构（含 session），
        // 早于 RunStateGuard 析构，确保 session 释放先于唤醒下一个 waiter。
        // 创建/销毁均在调用方工作线程完成，不在音频线程/UI 线程。
        {
            auto extractorResult = ModelFactory::createF0Extractor(
                config.modelType, config.modelDir, *env_, resamplingManager_);

            if (!extractorResult) {
                AppLogger::error("[F0InferenceService] Failed to create F0 extractor: "
                    + juce::String(extractorResult.error().fullMessage()));
            } else {
                auto extractor = std::move(extractorResult).value();

                // 缓存 hop/sample rate 供 getter 在 session 释放后使用
                {
                    std::unique_lock<std::shared_mutex> elock(extractorMutex_);
                    f0HopSize_ = extractor->getHopSize();
                    f0SampleRate_ = extractor->getTargetSampleRate();
                }

                // 应用用户配置的参数
                extractor->setConfidenceThreshold(config.confidence);
                extractor->setF0Min(config.f0Min);
                extractor->setF0Max(config.f0Max);

                try {
                    auto f0 = extractor->extractF0(audio, length, sampleRate,
                                                    runOptions_, progressCallback, partialCallback);
                    result = Result<std::vector<float>>::success(f0);
                } catch (...) {
                    AppLogger::error("[F0InferenceService] Exception during F0 extraction");
                    result = Result<std::vector<float>>::failure(
                        ErrorCode::ModelInferenceFailed, "Exception during F0 extraction");
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
    bool setF0Model(F0ModelType type) {
        std::unique_lock<std::shared_mutex> lock(extractorMutex_);

        if (type == currentModelType_)
            return true;

        if (!ModelFactory::isModelAvailable(type, modelDir_)) {
            AppLogger::error("[F0InferenceService] F0 model not available: "
                + juce::String(ModelFactory::getModelPath(type, modelDir_)));
            return false;
        }

        currentModelType_ = type;
        AppLogger::info("[F0InferenceService] Selected model: "
            + juce::String(type == F0ModelType::FCPE ? "FCPE" : "RMVPE")
            + " (session will be created on next extraction)");
        return true;
    }

    F0ModelType getCurrentF0Model() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        return currentModelType_;
    }

    std::vector<F0ModelInfo> getAvailableF0Models() const {
        std::string modelDir;
        {
            std::shared_lock<std::shared_mutex> lock(extractorMutex_);
            modelDir = modelDir_;
        }
        return ModelFactory::getAvailableF0Models(modelDir);
    }

    void setConfidenceThreshold(float threshold) {
        std::unique_lock<std::shared_mutex> lock(extractorMutex_);
        confidenceThreshold_ = threshold;
    }

    void setF0Min(float minFreq) {
        std::unique_lock<std::shared_mutex> lock(extractorMutex_);
        f0Min_ = minFreq;
    }

    void setF0Max(float maxFreq) {
        std::unique_lock<std::shared_mutex> lock(extractorMutex_);
        f0Max_ = maxFreq;
    }

    float getConfidenceThreshold() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        return confidenceThreshold_;
    }

    float getF0Min() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        return f0Min_;
    }

    float getF0Max() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        return f0Max_;
    }

    int getF0HopSize() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        return f0HopSize_;
    }

    int getF0SampleRate() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        return f0SampleRate_;
    }

    bool isInitialized() const {
        // 语义：service 已配置（modelDir + modelType 已设置），不是 session 常驻。
        return initialized_.load(std::memory_order_acquire);
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
    F0ModelType currentModelType_{F0ModelType::FCPE};
    std::string modelDir_;
    mutable std::shared_mutex extractorMutex_;   // guards config fields (no session)
    std::atomic<bool> initialized_{false};
    // FCPE 默认值；session 释放后 getter 仍返回正确值。
    // set 参数后下一次 extractF0 创建的新 session 应继承。
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

bool F0InferenceService::initialize(const std::string& modelDir, F0ModelType initialModel) {
    return pImpl_->initialize(modelDir, initialModel);
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

void F0InferenceService::terminateActiveRun(const std::shared_ptr<F0RunOwnerState>& ownerState) {
    pImpl_->terminateActiveRun(ownerState);
}

bool F0InferenceService::setF0Model(F0ModelType type) {
    return pImpl_->setF0Model(type);
}

F0ModelType F0InferenceService::getCurrentF0Model() const {
    return pImpl_->getCurrentF0Model();
}

std::vector<F0ModelInfo> F0InferenceService::getAvailableF0Models() const {
    return pImpl_->getAvailableF0Models();
}

void F0InferenceService::setConfidenceThreshold(float threshold) {
    pImpl_->setConfidenceThreshold(threshold);
}

void F0InferenceService::setF0Min(float minFreq) {
    pImpl_->setF0Min(minFreq);
}

void F0InferenceService::setF0Max(float maxFreq) {
    pImpl_->setF0Max(maxFreq);
}

float F0InferenceService::getConfidenceThreshold() const {
    return pImpl_->getConfidenceThreshold();
}

float F0InferenceService::getF0Min() const {
    return pImpl_->getF0Min();
}

float F0InferenceService::getF0Max() const {
    return pImpl_->getF0Max();
}

int F0InferenceService::getF0HopSize() const {
    return pImpl_->getF0HopSize();
}

int F0InferenceService::getF0SampleRate() const {
    return pImpl_->getF0SampleRate();
}

bool F0InferenceService::isInitialized() const {
    return pImpl_->isInitialized();
}

} // namespace OpenTune
