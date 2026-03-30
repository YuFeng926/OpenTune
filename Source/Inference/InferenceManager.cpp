#include "InferenceManager.h"
#include "ModelFactory.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/SimdPerceptualPitchEstimator.h"
#include "../Utils/CpuFeatures.h"
#include "../Utils/GpuDetector.h"
#include "../Utils/SimdAccelerator.h"
#include "../Utils/AppLogger.h"
#include <onnxruntime_cxx_api.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>

namespace OpenTune {

// ==============================================================================
// Implementation Class
// ==============================================================================

class InferenceManager::Impl {
public:
    Impl() {
        Ort::InitApi();
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OpenTune");
        resamplingManager_ = std::make_shared<ResamplingManager>();
    }

    ~Impl() {
        std::unique_lock<std::shared_mutex> f0WriteLock(f0ExtractorMutex_);
        currentF0Extractor_.reset();
        currentVocoder_.reset();
    }

    bool initialize(const std::string& modelDir) {
        try {
            // 硬件检测（只在首次初始化时执行）
            // 检测 CPU SIMD 支持级别（SSE/AVX/AVX2/AVX512）
            CpuFeatures::getInstance().detect();
            // 检测 SIMD 加速器
            SimdAccelerator::getInstance().detect();
            // 检测 GPU 加速支持（DirectML > CPU）
            GpuDetector::getInstance().detect();

            const bool gpuMode = GpuDetector::getInstance().getSelectedBackend() != GpuDetector::GpuBackend::CPU;
            ModelFactory::configureCpuBudgetHints(gpuMode);
            
            modelDir_ = modelDir;

            // 1. Load F0 model (RMVPE)
            auto initialF0Extractor = ModelFactory::createF0Extractor(
                F0ModelType::RMVPE, modelDir, *env_, resamplingManager_, ModelFactory::InferenceProvider::CPUOnly
            );

            if (!initialF0Extractor) {
                AppLogger::error("[InferenceManager] RMVPE model not available!");
                return false;
            }

            {
                std::unique_lock<std::shared_mutex> f0WriteLock(f0ExtractorMutex_);
                currentF0Extractor_ = std::move(initialF0Extractor);
            }
            currentF0ModelType_ = F0ModelType::RMVPE;

            // 2. Load vocoder (PC-NSF-HifiGAN preferred)
            currentVocoder_ = ModelFactory::createVocoder(
                VocoderType::PC_NSF_HifiGAN, modelDir, *env_
            );
            
            if (currentVocoder_) {
                currentVocoderType_ = VocoderType::PC_NSF_HifiGAN;
            } else {
                AppLogger::info("[InferenceManager] PC-NSF-HifiGAN not found, trying standard HifiGAN...");
                currentVocoder_ = ModelFactory::createVocoder(
                    VocoderType::HifiGAN, modelDir, *env_
                );
                currentVocoderType_ = VocoderType::HifiGAN;
            }

            if (!currentVocoder_) {
                AppLogger::error("[InferenceManager] Failed to load any vocoder");
            }

            initialized_ = true;

            std::string f0ModelName = "(none)";
            {
                std::shared_lock<std::shared_mutex> f0ReadLock(f0ExtractorMutex_);
                if (currentF0Extractor_) {
                    f0ModelName = currentF0Extractor_->getName();
                }
            }

            AppLogger::info("[InferenceManager] Initialized with F0 model: " + juce::String(f0ModelName));
            if (currentVocoder_) {
                AppLogger::info("[InferenceManager] Vocoder: " + juce::String(currentVocoder_->getName()));
            } else {
                AppLogger::info("[InferenceManager] Vocoder: (none)");
            }

            // Cold-start warmup: trigger minimal valid inference on loaded models.
            warmup();

            return true;

        } catch (const Ort::Exception& e) {
            AppLogger::error("[InferenceManager] ONNX Runtime error: " + juce::String(e.what()));
            return false;
        } catch (const std::exception& e) {
            AppLogger::error("[InferenceManager] Error: " + juce::String(e.what()));
            return false;
        } catch (...) {
            AppLogger::error("[InferenceManager] Unknown error");
            return false;
        }
    }

    void setF0ConfidenceThreshold(float threshold) {
        std::unique_lock<std::shared_mutex> f0WriteLock(f0ExtractorMutex_);
        if (currentF0Extractor_) {
            currentF0Extractor_->setConfidenceThreshold(threshold);
        }
    }

    void setF0Min(float minFreq) {
        std::unique_lock<std::shared_mutex> f0WriteLock(f0ExtractorMutex_);
        if (currentF0Extractor_) {
            currentF0Extractor_->setF0Min(minFreq);
        }
    }

    void setF0Max(float maxFreq) {
        std::unique_lock<std::shared_mutex> f0WriteLock(f0ExtractorMutex_);
        if (currentF0Extractor_) {
            currentF0Extractor_->setF0Max(maxFreq);
        }
    }

    float getF0ConfidenceThreshold() const {
        std::shared_lock<std::shared_mutex> f0ReadLock(f0ExtractorMutex_);
        if (currentF0Extractor_) return currentF0Extractor_->getConfidenceThreshold();
        return 0.03f;
    }

    float getF0Min() const {
        std::shared_lock<std::shared_mutex> f0ReadLock(f0ExtractorMutex_);
        if (currentF0Extractor_) return currentF0Extractor_->getF0Min();
        return 30.0f;
    }

    float getF0Max() const {
        std::shared_lock<std::shared_mutex> f0ReadLock(f0ExtractorMutex_);
        if (currentF0Extractor_) return currentF0Extractor_->getF0Max();
        return 2000.0f;
    }

    bool isInitialized() const { return initialized_; }

    int getF0HopSize() const {
        std::shared_lock<std::shared_mutex> f0ReadLock(f0ExtractorMutex_);
        return currentF0Extractor_ ? currentF0Extractor_->getHopSize() : 160;
    }

    int getF0SampleRate() const {
        std::shared_lock<std::shared_mutex> f0ReadLock(f0ExtractorMutex_);
        return currentF0Extractor_ ? currentF0Extractor_->getTargetSampleRate() : 16000;
    }

    int getVocoderHopSize() const {
        return 512; 
    }

    // ==============================================================================
    // Model Switching
    // ==============================================================================

    bool setF0Model(F0ModelType type) {
        {
            std::shared_lock<std::shared_mutex> f0ReadLock(f0ExtractorMutex_);
            if (type == currentF0ModelType_ && currentF0Extractor_) {
                return true;  // Already using this model
            }
        }

        AppLogger::info("[InferenceManager] Switching F0 model to: RMVPE");

        auto newExtractor = ModelFactory::createF0Extractor(
            type, modelDir_, *env_, resamplingManager_, ModelFactory::InferenceProvider::CPUOnly
        );

        if (!newExtractor) {
            AppLogger::error("[InferenceManager] Failed to load RMVPE model!");
            return false;
        }

        {
            std::unique_lock<std::shared_mutex> f0WriteLock(f0ExtractorMutex_);
            currentF0ModelType_ = type;
            currentF0Extractor_ = std::move(newExtractor);
        }
        warmup();
        return true;
    }

    void warmup() {
        if (!initialized_) {
            return;
        }

        try {
            std::shared_lock<std::shared_mutex> f0ReadLock(f0ExtractorMutex_);
            if (currentF0Extractor_) {
                const int sr = std::max(8000, currentF0Extractor_->getTargetSampleRate());
                const size_t samples = (size_t)std::max(sr / 2, currentF0Extractor_->getHopSize() * 8);
                std::vector<float> warmAudio(samples, 0.0f);
                const float freq = 220.0f;
                const float amp = 0.03f;
                for (size_t i = 0; i < warmAudio.size(); ++i) {
                    warmAudio[i] = amp * std::sin(2.0f * juce::MathConstants<float>::pi * freq * (float)i / (float)sr);
                }
                juce::ignoreUnused(currentF0Extractor_->extractF0(warmAudio.data(), warmAudio.size(), sr));
            }

            if (currentVocoder_) {
                constexpr int kWarmFrames = 64;
                constexpr int kMelBins = 128;
                std::vector<float> warmF0((size_t)kWarmFrames, 220.0f);
                std::vector<float> warmMel((size_t)kMelBins * (size_t)kWarmFrames, 0.0f);

                const float* melPtr = currentVocoder_->requiresMel() ? warmMel.data() : nullptr;
                juce::ignoreUnused(currentVocoder_->synthesize(warmF0, melPtr));
            }
        }
        catch (const std::exception& e) {
            AppLogger::warn("[InferenceManager] Warmup failed: " + juce::String(e.what()));
        }
        catch (...) {
            AppLogger::warn("[InferenceManager] Warmup failed with unknown exception");
        }
    }

    F0ModelType getCurrentF0Model() const { return currentF0ModelType_; }
    VocoderType getCurrentVocoder() const { return currentVocoderType_; }

    std::vector<F0ModelInfo> getAvailableF0Models() const {
        return ModelFactory::getAvailableF0Models(modelDir_);
    }

    std::vector<VocoderInfo> getAvailableVocoders() const {
        return ModelFactory::getAvailableVocoders(modelDir_);
    }

    // ==============================================================================
    // Unified Access
    // ==============================================================================

    std::pair<std::vector<float>, std::vector<float>> extractF0WithEnergy(const float* audio, size_t length, int sampleRate, std::function<void(float)> progressCallback, std::function<void(const std::vector<float>&, int)> partialCallback) {
        if (!initialized_) {
            AppLogger::error("[InferenceManager] Not initialized!");
            return {};
        }

        std::shared_lock<std::shared_mutex> f0ReadLock(f0ExtractorMutex_);
        if (!currentF0Extractor_) {
            AppLogger::error("[InferenceManager] F0 extractor not loaded!");
            return {};
        }
        
        auto f0 = currentF0Extractor_->extractF0(audio, length, sampleRate, progressCallback, partialCallback);
        
        // 2. Compute RMS Energy for each frame
        // Align energy with F0 frames
        int f0HopSize = currentF0Extractor_->getHopSize();
        int f0SampleRate = currentF0Extractor_->getTargetSampleRate();
        
        // Resample audio to model SR if needed for energy calculation alignment (optional, but cleaner)
        // Or calculate energy in original domain mapping to F0 frames.
        // Let's use the resampler to get 16k audio first if not already done inside extractor.
        // Extractor handles resampling internally but doesn't return it.
        // So we might need to resample again or use a ratio.
        // Using ratio on original audio is faster.
        
        std::vector<float> energy;
        energy.reserve(f0.size());
        
        double ratio = static_cast<double>(sampleRate) / static_cast<double>(f0SampleRate);
        int inputHopSize = static_cast<int>(f0HopSize * ratio);
        int windowSize = inputHopSize * 2; // Overlap for smoothing
        
        for (size_t i = 0; i < f0.size(); ++i) {
            int64_t centerSample = static_cast<int64_t>(i) * inputHopSize;
            int64_t startSample = centerSample - windowSize / 2;
            
            float sumSq = 0.0f;
            int count = 0;
            
            for (int j = 0; j < windowSize; ++j) {
                int64_t idx = startSample + j;
                if (idx >= 0 && idx < static_cast<int64_t>(length)) {
                    float val = audio[idx];
                    sumSq += val * val;
                    count++;
                }
            }
            
            float rms = (count > 0) ? std::sqrt(sumSq / count) : 0.0f;
            // Linear RMS is fine for weighting, but PIP algorithm expects suitable weights.
            // User note: "suggest log transform (dB) ... to prevent dominance"
            // BUT user code uses raw energy passed in.
            // Let's store Linear RMS for weight, but ensure it's not too peaky?
            // Actually, for weighted average, linear energy (amplitude^2 or amplitude) is standard.
            // If we use dB, negative values mess up weighted average unless offset.
            // SimdPerceptualPitchEstimator::prepareWeightBuffer takes energy directly.
            // Let's pass Linear RMS.
            energy.push_back(rms);
        }
        
        return {f0, energy};
    }

    std::vector<float> extractF0(const float* audio, size_t length, int sampleRate, std::function<void(float)> progressCallback, std::function<void(const std::vector<float>&, int)> partialCallback) {
        return extractF0WithEnergy(audio, length, sampleRate, progressCallback, partialCallback).first;
    }

    std::vector<float> synthesizeAudio(const std::vector<float>& f0, const float* mel) {
        if (!initialized_ || !currentVocoder_) return {};
        
        // 直接使用原始F0进行合成
        // 能量相关的心理声学校准需通过 synthesizeAudioWithEnergy 接口实现
        return currentVocoder_->synthesize(f0, mel);
    }
    
    std::vector<float> synthesizeAudioWithEnergy(const std::vector<float>& f0, const std::vector<float>& energy, const float* mel) {
        if (!initialized_ || !currentVocoder_) return {};

        std::vector<float> calibratedF0 = f0;
        
        // Apply Psychoacoustic Calibration
        if (!energy.empty()) {
            // Ensure length match (resample energy if needed, or nearest neighbor)
            // Assuming aligned for now
            size_t n = std::min(f0.size(), energy.size());
            
            for (size_t i = 0; i < n; ++i) {
                float hz = f0[i];
                if (hz <= 0.0f) continue;
                
                // Energy is linear RMS? User formula uses dB.
                // Convert linear to dB
                float lin = energy[i];
                float db = (lin > 1e-9f) ? 20.0f * std::log10(lin) : -100.0f;
                
                float targetOffset = SimdPerceptualPitchEstimator::getPerceptualOffset(hz, db);

                // Apply offset (cents)
                // f_new = f * 2^(cents/1200)
                calibratedF0[i] = hz * std::pow(2.0f, targetOffset / 1200.0f);
            }
        }
        
        return currentVocoder_->synthesize(calibratedF0, mel);
    }
    
    void notifyOriginalF0ExtractionStarted() {
        pendingF0Extractions_.fetch_add(1, std::memory_order_relaxed);
        AppLogger::debug("[InferenceManager] F0 extraction started, pending: " 
                  + juce::String(pendingF0Extractions_.load()));
    }
    
    void notifyOriginalF0ExtractionCompleted() {
        const int prevCount = pendingF0Extractions_.fetch_sub(1, std::memory_order_acq_rel);
        if (prevCount <= 0) {
            pendingF0Extractions_.store(0, std::memory_order_release);
            AppLogger::warn("[InferenceManager] F0 extraction completed without matching start");
            return;
        }

        AppLogger::debug("[InferenceManager] F0 extraction completed, pending: "
                  + juce::String(prevCount - 1));
    }
    
    int getPendingF0ExtractionsCount() const {
        return pendingF0Extractions_.load(std::memory_order_relaxed);
    }
    
    bool isF0ModelLoaded() const {
        std::shared_lock<std::shared_mutex> f0ReadLock(f0ExtractorMutex_);
        return currentF0Extractor_ != nullptr;
    }
    
    void shutdown() {
        while (pendingF0Extractions_.load(std::memory_order_acquire) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        std::unique_lock<std::shared_mutex> f0WriteLock(f0ExtractorMutex_);
        currentF0Extractor_.reset();
        currentVocoder_.reset();
        initialized_ = false;
    }
    
private:
    std::string modelDir_;
    std::unique_ptr<Ort::Env> env_;
    std::shared_ptr<ResamplingManager> resamplingManager_;

    std::unique_ptr<IF0Extractor> currentF0Extractor_;
    std::unique_ptr<IVocoder> currentVocoder_;
    mutable std::shared_mutex f0ExtractorMutex_;

    F0ModelType currentF0ModelType_ = F0ModelType::RMVPE;
    VocoderType currentVocoderType_ = VocoderType::HifiGAN;

    bool initialized_ = false;
    
    std::atomic<int> pendingF0Extractions_{0};
};

// ==============================================================================
// Singleton Interface
// ==============================================================================

InferenceManager& InferenceManager::getInstance() {
    static InferenceManager instance;
    return instance;
}

// Constructor (private)
InferenceManager::InferenceManager() : pImpl_(std::make_unique<Impl>()) {}

// Destructor
InferenceManager::~InferenceManager() = default;

InferenceManager::OriginalF0ExtractionGuard::~OriginalF0ExtractionGuard() {
    if (owner_ != nullptr) {
        owner_->notifyOriginalF0ExtractionCompleted();
        owner_ = nullptr;
    }
}

InferenceManager::OriginalF0ExtractionGuard::OriginalF0ExtractionGuard(OriginalF0ExtractionGuard&& other) noexcept {
    owner_ = other.owner_;
    other.owner_ = nullptr;
}

InferenceManager::OriginalF0ExtractionGuard& InferenceManager::OriginalF0ExtractionGuard::operator=(OriginalF0ExtractionGuard&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (owner_ != nullptr) {
        owner_->notifyOriginalF0ExtractionCompleted();
    }

    owner_ = other.owner_;
    other.owner_ = nullptr;
    return *this;
}

InferenceManager::OriginalF0ExtractionGuard InferenceManager::beginOriginalF0Extraction() {
    notifyOriginalF0ExtractionStarted();
    return OriginalF0ExtractionGuard(this);
}

// Forward calls to pImpl
bool InferenceManager::initialize(const std::string& modelDir) { return pImpl_->initialize(modelDir); }
void InferenceManager::warmup() { pImpl_->warmup(); }

bool InferenceManager::setF0Model(F0ModelType type) { return pImpl_->setF0Model(type); }

F0ModelType InferenceManager::getCurrentF0Model() const { return pImpl_->getCurrentF0Model(); }
VocoderType InferenceManager::getCurrentVocoder() const { return pImpl_->getCurrentVocoder(); }

std::vector<F0ModelInfo> InferenceManager::getAvailableF0Models() const { return pImpl_->getAvailableF0Models(); }
std::vector<VocoderInfo> InferenceManager::getAvailableVocoders() const { return pImpl_->getAvailableVocoders(); }

std::vector<float> InferenceManager::extractF0(const float* audio, size_t length, int sampleRate, std::function<void(float)> progressCallback, std::function<void(const std::vector<float>&, int)> partialCallback) {
    return pImpl_->extractF0(audio, length, sampleRate, progressCallback, partialCallback);
}

std::vector<float> InferenceManager::synthesizeAudio(const std::vector<float>& f0, const float* mel) {
    return pImpl_->synthesizeAudio(f0, mel);
}

std::vector<float> InferenceManager::synthesizeAudioWithEnergy(const std::vector<float>& f0, const std::vector<float>& energy, const float* mel) {
    return pImpl_->synthesizeAudioWithEnergy(f0, energy, mel);
}

std::pair<std::vector<float>, std::vector<float>> InferenceManager::extractF0WithEnergy(const float* audio, size_t length, int sampleRate, std::function<void(float)> progressCallback, std::function<void(const std::vector<float>&, int)> partialCallback) {
    return pImpl_->extractF0WithEnergy(audio, length, sampleRate, progressCallback, partialCallback);
}

void InferenceManager::notifyOriginalF0ExtractionStarted() {
    pImpl_->notifyOriginalF0ExtractionStarted();
}

void InferenceManager::notifyOriginalF0ExtractionCompleted() {
    pImpl_->notifyOriginalF0ExtractionCompleted();
}

int InferenceManager::getPendingF0ExtractionsCount() const {
    return pImpl_->getPendingF0ExtractionsCount();
}

bool InferenceManager::isF0ModelLoaded() const {
    return pImpl_->isF0ModelLoaded();
}

void InferenceManager::setF0ConfidenceThreshold(float threshold) {
    pImpl_->setF0ConfidenceThreshold(threshold);
}

void InferenceManager::setF0Min(float minFreq) {
    pImpl_->setF0Min(minFreq);
}

void InferenceManager::setF0Max(float maxFreq) {
    pImpl_->setF0Max(maxFreq);
}

float InferenceManager::getF0ConfidenceThreshold() const {
    return pImpl_->getF0ConfidenceThreshold();
}

float InferenceManager::getF0Min() const {
    return pImpl_->getF0Min();
}

float InferenceManager::getF0Max() const {
    return pImpl_->getF0Max();
}

bool InferenceManager::isInitialized() const {
    return pImpl_->isInitialized();
}

int InferenceManager::getF0HopSize() const {
    return pImpl_->getF0HopSize();
}

int InferenceManager::getF0SampleRate() const {
    return pImpl_->getF0SampleRate();
}

int InferenceManager::getVocoderHopSize() const {
    return pImpl_->getVocoderHopSize();
}

void InferenceManager::shutdown() {
    pImpl_->shutdown();
}

} // namespace OpenTune
