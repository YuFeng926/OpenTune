#include "F0InferenceService.h"
#include "ModelFactory.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/AppLogger.h"
#include <onnxruntime_cxx_api.h>
#include <shared_mutex>

namespace OpenTune {

class F0InferenceService::Impl {
public:
    Impl() {
        Ort::InitApi();
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OpenTune-F0");
        resamplingManager_ = std::make_shared<ResamplingManager>();
    }

    ~Impl() {
        shutdown();
    }

    bool initialize(const std::string& modelDir) {
        try {
            modelDir_ = modelDir;

            auto extractorResult = ModelFactory::createF0Extractor(
                F0ModelType::RMVPE, modelDir, *env_, resamplingManager_
            );

            if (!extractorResult) {
                AppLogger::error("[F0InferenceService] Failed to load F0 model: " 
                    + juce::String(extractorResult.error().fullMessage()));
                return false;
            }

            {
                std::unique_lock<std::shared_mutex> lock(extractorMutex_);
                currentExtractor_ = std::move(extractorResult).value();
                currentModelType_ = F0ModelType::RMVPE;
            }

            initialized_.store(true, std::memory_order_release);
#if defined(__APPLE__)
            AppLogger::info("[F0InferenceService] Initialized with RMVPE model (CoreML)");
#else
            AppLogger::info("[F0InferenceService] Initialized with RMVPE model (CPU-only)");
#endif
            return true;

        } catch (const std::exception& e) {
            AppLogger::error("[F0InferenceService] Initialization error: " + juce::String(e.what()));
            return false;
        } catch (...) {
            AppLogger::error("[F0InferenceService] Unknown initialization error");
            return false;
        }
    }

    void shutdown() {
        initialized_.store(false, std::memory_order_release);
        {
            std::unique_lock<std::shared_mutex> lock(extractorMutex_);
            currentExtractor_.reset();
        }
    }

    Result<std::vector<float>> extractF0(
        const float* audio,
        size_t length,
        int sampleRate,
        std::function<void(float)> progressCallback,
        std::function<void(const std::vector<float>&, int)> partialCallback)
    {
        if (!initialized_.load(std::memory_order_acquire)) {
            return Result<std::vector<float>>::failure(
                ErrorCode::NotInitialized, "F0InferenceService not initialized");
        }

        std::shared_lock<std::shared_mutex> lock(extractorMutex_);
        if (!currentExtractor_) {
            return Result<std::vector<float>>::failure(
                ErrorCode::NotInitialized, "F0 extractor not available");
        }

        try {
            auto f0 = currentExtractor_->extractF0(audio, length, sampleRate, 
                                                    progressCallback, partialCallback);
            return Result<std::vector<float>>::success(f0);
        } catch (const std::exception& e) {
            return Result<std::vector<float>>::failure(
                ErrorCode::ModelInferenceFailed, std::string(e.what()));
        } catch (...) {
            AppLogger::error("[F0InferenceService] Unknown exception during F0 extraction");
            return Result<std::vector<float>>::failure(
                ErrorCode::ModelInferenceFailed, "Unknown exception during F0 extraction");
        }
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
    std::unique_ptr<Ort::Env> env_;
    std::shared_ptr<ResamplingManager> resamplingManager_;
    std::unique_ptr<IF0Extractor> currentExtractor_;
    F0ModelType currentModelType_{F0ModelType::RMVPE};
    std::string modelDir_;
    mutable std::shared_mutex extractorMutex_;
    std::atomic<bool> initialized_{false};
};

F0InferenceService::F0InferenceService() : pImpl_(std::make_unique<Impl>()) {}

F0InferenceService::~F0InferenceService() = default;

bool F0InferenceService::initialize(const std::string& modelDir) {
    return pImpl_->initialize(modelDir);
}

void F0InferenceService::shutdown() {
    pImpl_->shutdown();
}

Result<std::vector<float>> F0InferenceService::extractF0(
    const float* audio,
    size_t length,
    int sampleRate,
    std::function<void(float)> progressCallback,
    std::function<void(const std::vector<float>&, int)> partialCallback)
{
    return pImpl_->extractF0(audio, length, sampleRate, progressCallback, partialCallback);
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
