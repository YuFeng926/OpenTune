#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include "../Utils/Note.h"
#include "IF0Extractor.h"
#include "IVocoder.h"

namespace OpenTune {

/**
 * @brief Singleton inference manager for AI models
 *
 * Manages:
 * - F0 Extractor: RMVPE
 * - Vocoders: HifiGAN
 */
class InferenceManager {
public:
    class OriginalF0ExtractionGuard {
    public:
        OriginalF0ExtractionGuard() = default;
        ~OriginalF0ExtractionGuard();

        OriginalF0ExtractionGuard(const OriginalF0ExtractionGuard&) = delete;
        OriginalF0ExtractionGuard& operator=(const OriginalF0ExtractionGuard&) = delete;

        OriginalF0ExtractionGuard(OriginalF0ExtractionGuard&& other) noexcept;
        OriginalF0ExtractionGuard& operator=(OriginalF0ExtractionGuard&& other) noexcept;

    private:
        friend class InferenceManager;
        explicit OriginalF0ExtractionGuard(InferenceManager* owner) : owner_(owner) {}

        InferenceManager* owner_ = nullptr;
    };

    static InferenceManager& getInstance();

    /**
     * @brief Initialize ONNX Runtime and load default models
     * @param modelDir Directory containing .onnx files
     * @return true if successful
     */
    bool initialize(const std::string& modelDir);

    /**
     * @brief Run warm-up inference to pre-allocate GPU memory
     */
    void warmup();

    /**
     * @brief Ensure the active F0 extractor is RMVPE
     * @param type Must be `F0ModelType::RMVPE`
     * @return true if RMVPE is available and loaded
     */
    bool setF0Model(F0ModelType type);

    /**
     * @brief Get current F0 model type
     */
    F0ModelType getCurrentF0Model() const;

    /**
     * @brief Get current vocoder type
     */
    VocoderType getCurrentVocoder() const;

    /**
     * @brief Get available F0 models in modelDir
     */
    std::vector<F0ModelInfo> getAvailableF0Models() const;

    /**
     * @brief Get list of available vocoders in modelDir
     */
    std::vector<VocoderInfo> getAvailableVocoders() const;

    /**
     * @brief Set confidence threshold for F0 extraction
     * @param threshold Value between 0.0 and 1.0
     */
    void setF0ConfidenceThreshold(float threshold);

    /**
     * @brief Set minimum frequency for F0 extraction
     * @param minFreq Frequency in Hz
     */
    void setF0Min(float minFreq);

    /**
     * @brief Set maximum frequency for F0 extraction
     * @param maxFreq Frequency in Hz
     */
    void setF0Max(float maxFreq);

    float getF0ConfidenceThreshold() const;
    float getF0Min() const;
    float getF0Max() const;

    // ==============================================================================
    // 统一模型访问接口
    // ==============================================================================

    /**
     * @brief Extract F0 using current model
     * @param audio Input audio buffer
     * @param length Number of samples
     * @param sampleRate Input sample rate (resampled internally)
     * @return F0 curve in Hz (one value per frame)
     */
    std::vector<float> extractF0(const float* audio, size_t length, int sampleRate, std::function<void(float)> progressCallback = nullptr, std::function<void(const std::vector<float>&, int)> partialCallback = nullptr);

    /**
     * @brief Synthesize audio using current vocoder
     * @param f0 F0 curve (one value per frame)
     * @param mel Optional mel spectrogram input
     * @return Synthesized audio
     */
    std::vector<float> synthesizeAudio(const std::vector<float>& f0, const float* mel = nullptr);

    std::vector<float> synthesizeAudioWithEnergy(const std::vector<float>& f0, const std::vector<float>& energy, const float* mel = nullptr);

    std::pair<std::vector<float>, std::vector<float>> extractF0WithEnergy(const float* audio, size_t length, int sampleRate, std::function<void(float)> progressCallback = nullptr, std::function<void(const std::vector<float>&, int)> partialCallback = nullptr);

    // ==============================================================================
    // F0 Model Memory Management
    // ==============================================================================

    /**
     * @brief Notify that an OriginalF0 extraction task has started
     */
    void notifyOriginalF0ExtractionStarted();

    /**
     * @brief Notify that an OriginalF0 extraction task has completed
     */
    void notifyOriginalF0ExtractionCompleted();

    /**
     * @brief Get current number of pending F0 extractions
     */
    int getPendingF0ExtractionsCount() const;

    /**
     * @brief Check if F0 model is currently loaded
     */
    bool isF0ModelLoaded() const;

    /**
     * @brief RAII guard for OriginalF0 extraction lifecycle accounting
     */
    OriginalF0ExtractionGuard beginOriginalF0Extraction();

    // ==============================================================================
    // Status & Info
    // ==============================================================================

    /**
     * @brief Check if models are loaded
     */
    bool isInitialized() const;

    /**
     * @brief Shutdown all inference operations and release models
     * 
     * Must be called before application exit to ensure all pending
     * operations complete before model destruction.
     */
    void shutdown();

    /**
     * @brief Get current F0 extractor hop size
     */
    int getF0HopSize() const;

    /**
     * @brief Get current F0 extractor target sample rate
     */
    int getF0SampleRate() const;

    /**
     * @brief Get current vocoder hop size
     */
    int getVocoderHopSize() const;

private:
    InferenceManager();
    ~InferenceManager();

    class Impl;
    std::unique_ptr<Impl> pImpl_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InferenceManager)
};

} // namespace OpenTune
