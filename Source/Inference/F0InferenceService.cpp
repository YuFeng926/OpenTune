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

    bool initialize(const std::string& modelDir, F0ModelType initialModel) {
        try {
            modelDir_ = modelDir;

            auto extractorResult = ModelFactory::createF0Extractor(
                initialModel, modelDir, *env_, resamplingManager_
            );

            if (!extractorResult && initialModel != F0ModelType::RMVPE) {
                AppLogger::warn("[F0InferenceService] Preferred F0 model unavailable; falling back to RMVPE");
                extractorResult = ModelFactory::createF0Extractor(
                    F0ModelType::RMVPE, modelDir, *env_, resamplingManager_);
                initialModel = F0ModelType::RMVPE;
            }

            if (!extractorResult) {
                AppLogger::error("[F0InferenceService] Failed to load F0 model: " 
                    + juce::String(extractorResult.error().fullMessage()));
                return false;
            }

            {
                std::unique_lock<std::shared_mutex> lock(extractorMutex_);
                currentExtractor_ = std::move(extractorResult).value();
                currentModelType_ = initialModel;
            }

            initialized_.store(true, std::memory_order_release);
#if defined(__APPLE__)
            AppLogger::info("[F0InferenceService] Initialized with "
                + juce::String(initialModel == F0ModelType::FCPE ? "FCPE" : "RMVPE")
                + " model (CoreML)");
#else
            AppLogger::info("[F0InferenceService] Initialized with "
                + juce::String(initialModel == F0ModelType::FCPE ? "FCPE" : "RMVPE")
                + " model (CPU/DML)");
#endif
            return true;

        } catch (...) {
            AppLogger::error("[F0InferenceService] Unknown initialization error");
            return false;
        }
    }

    Result<std::vector<float>> extractF0(
        const float* audio,
        size_t length,
        int sampleRate,
        std::shared_ptr<F0RunOwnerState> ownerState,
        std::function<void(float)> progressCallback,
        std::function<void(const std::vector<float>&, int)> partialCallback)
    {
        if (!initialized_.load(std::memory_order_acquire)) {
            if (!initialize(modelDir_, F0ModelType::RMVPE)) {
                return Result<std::vector<float>>::failure(
                    ErrorCode::NotInitialized, "F0InferenceService failed to re-initialize");
            }
        }

        // DML 合同：同一 Session 同一时刻仅允许一个线程 Run()。
        // admission gate：pendingLeases_ 排队，runCv_ 唤醒；无活跃 Run 或自身
        // 已被 terminateActiveRun 取消时入场。
        auto lease = std::make_shared<RunLease>();
        lease->ownerState = std::move(ownerState);

        std::unique_lock<std::mutex> runLock(runMutex_);
        // 晚到 lease 直接拒绝：owner state 已持久关闭（一次关闭、永不重新入场）。
        // 检查与 terminateActiveRun 的 closed.store 在同一 runMutex_ 下，
        // 检查后到 push 之间不可能交错，无"检查后关闭"窗口。
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

        // 已取消的 waiter 直接退出，不占用 Run 槽位、不等待其他 owner
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

        // RAII：Run 结束（含异常逃逸路径）后复位锁内状态并唤醒下一个 waiter
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

        Result<std::vector<float>> result = Result<std::vector<float>>::failure(
            ErrorCode::NotInitialized, "F0 extractor not available");

        {
            std::shared_lock<std::shared_mutex> lock(extractorMutex_);
            if (currentExtractor_) {
                try {
                    auto f0 = currentExtractor_->extractF0(audio, length, sampleRate,
                                                             runOptions_, progressCallback, partialCallback);
                    result = Result<std::vector<float>>::success(f0);
                } catch (...) {
                    AppLogger::error("[F0InferenceService] Unknown exception during F0 extraction");
                    result = Result<std::vector<float>>::failure(
                        ErrorCode::ModelInferenceFailed, "Unknown exception during F0 extraction");
                }
            }
        }

        return result;
    }

    void terminateActiveRun(const std::shared_ptr<F0RunOwnerState>& ownerState) {
        // 持久关闭：state->closed 一旦置位永不恢复。与 lease 取消、SetTerminate
        // 全部在同一状态锁下：晚到的 extractF0 入场检查（同锁）必能看到 closed，
        // 消除"检查后关闭"窗口。state 生命周期由共享所有权保证：本服务在最后一次
        // 调用结束后才析构，state 在最后一次使用结束后才释放。
        std::lock_guard<std::mutex> lk(runMutex_);
        ownerState->closed.store(true, std::memory_order_release);
        for (auto& lease : pendingLeases_)
            if (lease->ownerState == ownerState)
                lease->cancelled = true;
        if (activeLease_ && activeLease_->ownerState == ownerState)
            runOptions_.SetTerminate();
        runCv_.notify_all(); // 唤醒被取消的 waiter
    }

    bool setF0Model(F0ModelType type) {
        {
            std::shared_lock<std::shared_mutex> lock(extractorMutex_);
            if (type == currentModelType_ && currentExtractor_) {
                return true;
            }
        }

        auto extractorResult = ModelFactory::createF0Extractor(
            type, modelDir_, *env_, resamplingManager_
        );

        if (!extractorResult) {
            AppLogger::error("[F0InferenceService] Failed to load F0 model: " 
                + juce::String(extractorResult.error().fullMessage()));
            return false;
        }

        {
            std::unique_lock<std::shared_mutex> lock(extractorMutex_);
            currentModelType_ = type;
            currentExtractor_ = std::move(extractorResult).value();
        }
        return true;
    }

    F0ModelType getCurrentF0Model() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        return currentModelType_;
    }

    std::vector<F0ModelInfo> getAvailableF0Models() const {
        return ModelFactory::getAvailableF0Models(modelDir_);
    }

    void setConfidenceThreshold(float threshold) {
        std::unique_lock<std::shared_mutex> lock(extractorMutex_);
        if (currentExtractor_) {
            currentExtractor_->setConfidenceThreshold(threshold);
        }
    }

    void setF0Min(float minFreq) {
        std::unique_lock<std::shared_mutex> lock(extractorMutex_);
        if (currentExtractor_) {
            currentExtractor_->setF0Min(minFreq);
        }
    }

    void setF0Max(float maxFreq) {
        std::unique_lock<std::shared_mutex> lock(extractorMutex_);
        if (currentExtractor_) {
            currentExtractor_->setF0Max(maxFreq);
        }
    }

    float getConfidenceThreshold() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        if (currentExtractor_) return currentExtractor_->getConfidenceThreshold();
        return 0.03f;
    }

    float getF0Min() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        if (currentExtractor_) return currentExtractor_->getF0Min();
        return 30.0f;
    }

    float getF0Max() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        if (currentExtractor_) return currentExtractor_->getF0Max();
        return 2000.0f;
    }

    int getF0HopSize() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        return currentExtractor_ ? currentExtractor_->getHopSize() : 160;
    }

    int getF0SampleRate() const {
        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        return currentExtractor_ ? currentExtractor_->getTargetSampleRate() : 16000;
    }

    bool isInitialized() const {
        return initialized_.load(std::memory_order_acquire);
    }

private:
    // Run lease + admission gate：runMutex_ 是唯一状态锁，线性化入场、取消与
    // RunOptions 切换；runCv_ 唤醒被取消的 waiter 和下一个入场者。
    // cancelled 仅在 runMutex_ 下访问，用普通 bool。
    struct RunLease {
        std::shared_ptr<F0RunOwnerState> ownerState;
        bool cancelled{false};
    };

    std::shared_ptr<Ort::Env> env_;
    std::shared_ptr<ResamplingManager> resamplingManager_;
    std::unique_ptr<IF0Extractor> currentExtractor_;
    F0ModelType currentModelType_{F0ModelType::RMVPE};
    std::string modelDir_;
    mutable std::shared_mutex extractorMutex_;
    std::atomic<bool> initialized_{false};

    // DML 合同：同一 Session 同一时刻仅允许一个线程 Run()
    std::mutex runMutex_;
    std::condition_variable runCv_;
    bool runInProgress_{false};
    std::shared_ptr<RunLease> activeLease_;
    std::vector<std::shared_ptr<RunLease>> pendingLeases_;  // 等待入场的 lease（锁内访问）
    Ort::RunOptions runOptions_;                            // 唯一 RunOptions，全部切换在同一状态锁下
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
