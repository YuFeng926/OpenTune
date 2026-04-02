#include "VocoderInferenceService.h"
#include "VocoderFactory.h"
#include "../Utils/AppLogger.h"
#include <onnxruntime_cxx_api.h>

namespace OpenTune {

class VocoderInferenceService::Impl {
public:
    Impl() {
        Ort::InitApi();
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OpenTune-Vocoder");
    }

    ~Impl() {
        shutdown();
    }

    bool initialize(const std::string& modelDir) {
        try {
            modelDir_ = modelDir;

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
            
            const char* backendName = (currentBackend_ == VocoderBackend::DML) ? "DML" : "CPU";
            AppLogger::info("[VocoderInferenceService] Initialized with vocoder (" 
                + juce::String(backendName) + ")");
            return true;

        } catch (const std::exception& e) {
            AppLogger::error("[VocoderInferenceService] Initialization error: " + juce::String(e.what()));
            return false;
        } catch (...) {
            AppLogger::error("[VocoderInferenceService] Unknown initialization error");
            return false;
        }
    }

    void shutdown() {
        initialized_.store(false, std::memory_order_release);
        currentVocoder_.reset();
    }

    Result<std::vector<float>> synthesizeAudioWithEnergy(
        const std::vector<float>& f0,
        const std::vector<float>& energy,
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
        } catch (...) {
            AppLogger::error("[VocoderInferenceService] Unknown exception during synthesis");
            return Result<std::vector<float>>::failure(
                ErrorCode::ModelInferenceFailed, "Unknown exception during vocoder synthesis");
        }
    }

    int getVocoderHopSize() const {
        return currentVocoder_ ? currentVocoder_->getHopSize() : 512;
    }

    int getMelBins() const {
        return currentVocoder_ ? currentVocoder_->getMelBins() : 128;
    }

    bool isInitialized() const {
        return initialized_.load(std::memory_order_acquire);
    }

private:
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<VocoderInterface> currentVocoder_;
    VocoderBackend currentBackend_ = VocoderBackend::CPU;
    std::string modelDir_;
    std::atomic<bool> initialized_{false};
};

VocoderInferenceService::VocoderInferenceService() : pImpl_(std::make_unique<Impl>()) {}

VocoderInferenceService::~VocoderInferenceService() = default;

bool VocoderInferenceService::initialize(const std::string& modelDir) {
    return pImpl_->initialize(modelDir);
}

void VocoderInferenceService::shutdown() {
    pImpl_->shutdown();
}

Result<std::vector<float>> VocoderInferenceService::synthesizeAudioWithEnergy(
    const std::vector<float>& f0,
    const std::vector<float>& energy,
    const float* mel,
    size_t melSize)
{
    std::lock_guard<std::mutex> lock(runMutex_);
    return doSynthesizeAudioWithEnergy(f0, energy, mel, melSize);
}

Result<std::vector<float>> VocoderInferenceService::doSynthesizeAudioWithEnergy(
    const std::vector<float>& f0,
    const std::vector<float>& energy,
    const float* mel,
    size_t melSize)
{
    return pImpl_->synthesizeAudioWithEnergy(f0, energy, mel, melSize);
}

int VocoderInferenceService::getVocoderHopSize() const {
    return pImpl_->getVocoderHopSize();
}

int VocoderInferenceService::getMelBins() const {
    return pImpl_->getMelBins();
}

bool VocoderInferenceService::isInitialized() const {
    return pImpl_->isInitialized();
}

} // namespace OpenTune
