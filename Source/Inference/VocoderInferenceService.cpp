#include "VocoderInferenceService.h"
#include "VocoderFactory.h"
#include "../Utils/AppLogger.h"
#include <onnxruntime_cxx_api.h>

namespace OpenTune {

class VocoderInferenceService::Impl {
public:
    Impl(std::shared_ptr<Ort::Env> env) : env_(std::move(env)) {}
    ~Impl() { shutdown(); }

    bool initialize(const std::string& modelDir) {
        try {
            const std::string modelPath = modelDir + "/hifigan.onnx";
            auto result = VocoderFactory::create(modelPath, *env_);

            if (!result.success()) {
                AppLogger::error("[VocoderInferenceService] Failed to load vocoder: "
                    + juce::String(result.errorMessage));
                return false;
            }

            currentVocoder_ = std::move(result.vocoder);
            currentBackend_ = result.backend;
            initialized_.store(true, std::memory_order_release);

            const char* backendName = "CPU";
            if (currentBackend_ == VocoderBackend::DML) backendName = "DML";
            else if (currentBackend_ == VocoderBackend::CoreML) backendName = "CoreML";
            AppLogger::info("[VocoderInferenceService] Initialized with vocoder ("
                + juce::String(backendName) + ")");
            return true;

        } catch (const std::exception& e) {
            AppLogger::error("[VocoderInferenceService] Initialization error: " + juce::String(e.what()));
            return false;
        }
    }

    void shutdown() {
        initialized_.store(false, std::memory_order_release);
        currentVocoder_.reset();
    }

    Result<std::vector<float>> synthesize(
        const std::vector<float>& f0,
        const float* mel,
        size_t melSize)
    {
        if (!initialized_.load(std::memory_order_acquire)) {
            return Result<std::vector<float>>::failure(
                ErrorCode::NotInitialized, "VocoderInferenceService not initialized");
        }

        try {
            auto audio = currentVocoder_->synthesize(f0, mel, melSize);
            return Result<std::vector<float>>::success(audio);
        } catch (const std::exception& e) {
            return Result<std::vector<float>>::failure(
                ErrorCode::ModelInferenceFailed, std::string(e.what()));
        }
    }

    int getVocoderHopSize() const {
        return currentVocoder_ ? currentVocoder_->getHopSize() : 512;
    }

    int getMelBins() const {
        return currentVocoder_ ? currentVocoder_->getMelBins() : 128;
    }

private:
    std::shared_ptr<Ort::Env> env_;
    std::unique_ptr<VocoderInterface> currentVocoder_;
    VocoderBackend currentBackend_ = VocoderBackend::CPU;
    std::atomic<bool> initialized_{false};
};

VocoderInferenceService::VocoderInferenceService(std::shared_ptr<Ort::Env> env)
    : pImpl_(std::make_unique<Impl>(std::move(env))) {}

VocoderInferenceService::~VocoderInferenceService() = default;

bool VocoderInferenceService::initialize(const std::string& modelDir) {
    return pImpl_->initialize(modelDir);
}

void VocoderInferenceService::shutdown() {
    pImpl_->shutdown();
}

Result<std::vector<float>> VocoderInferenceService::synthesize(
    const std::vector<float>& f0,
    const float* mel,
    size_t melSize)
{
    return pImpl_->synthesize(f0, mel, melSize);
}

int VocoderInferenceService::getVocoderHopSize() const {
    return pImpl_->getVocoderHopSize();
}

int VocoderInferenceService::getMelBins() const {
    return pImpl_->getMelBins();
}

} // namespace OpenTune
