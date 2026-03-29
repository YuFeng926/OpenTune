#pragma once

#include "IF0Extractor.h"
#include "IVocoder.h"
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <string>
#include <vector>
#include <atomic>

namespace OpenTune {

class ResamplingManager;

/**
 * @brief Factory for creating AI model instances
 *
 * This factory provides centralized model creation with:
 * - File existence checking
 * - ONNX session configuration
 * - Model discovery and availability checking
 * - Consistent error handling
 */
class ModelFactory {
public:
    enum class InferenceProvider {
        Auto,
        CPUOnly
    };

    /**
     * @brief Create F0 extractor instance
     * @param type Model type to create (RMVPE)
     * @param modelDir Directory containing .onnx files
     * @param env Shared ONNX Runtime environment
     * @param resampler Shared resampling manager
     * @return Unique pointer to F0 extractor, nullptr on failure
     */
    static std::unique_ptr<IF0Extractor> createF0Extractor(
        F0ModelType type,
        const std::string& modelDir,
        Ort::Env& env,
        std::shared_ptr<ResamplingManager> resampler,
        InferenceProvider provider
    );

    /**
     * @brief Create vocoder instance
     * @param type Vocoder type to create (currently only HifiGAN)
     * @param modelDir Directory containing .onnx files
     * @param env Shared ONNX Runtime environment
     * @return Unique pointer to vocoder, nullptr on failure
     */
    static std::unique_ptr<IVocoder> createVocoder(
        VocoderType type,
        const std::string& modelDir,
        Ort::Env& env
    );

    /**
     * @brief Get model file path for F0 extractor
     */
    static std::string getModelPath(F0ModelType type, const std::string& modelDir);

    /**
     * @brief Get model file path for vocoder
     */
    static std::string getModelPath(VocoderType type, const std::string& modelDir);

    /**
     * @brief Check if F0 model file exists
     */
    static bool isModelAvailable(F0ModelType type, const std::string& modelDir);

    /**
     * @brief Check if vocoder model file exists
     */
    static bool isModelAvailable(VocoderType type, const std::string& modelDir);

    /**
     * @brief Get available F0 models in directory
     */
    static std::vector<F0ModelInfo> getAvailableF0Models(const std::string& modelDir);

    /**
     * @brief Get all available vocoders in directory
     */
    static std::vector<VocoderInfo> getAvailableVocoders(const std::string& modelDir);

    /**
     * @brief Configure CPU budget hints for ONNX Runtime
     * @param gpuMode Whether GPU acceleration is enabled
     */
    static void configureCpuBudgetHints(bool gpuMode);

    /**
     * @brief Reset factory state (for testing)
     */
    static void resetState();

private:
    static Ort::SessionOptions createSessionOptions(InferenceProvider provider);

    static std::unique_ptr<Ort::Session> loadONNXSession(
        const std::string& modelPath,
        Ort::Env& env,
        InferenceProvider provider
    );

    static void logExecutionProviderInfo(Ort::Session* session, InferenceProvider provider);

    static std::atomic<bool>& getGpuModeHint() {
        static std::atomic<bool> instance{ false };
        return instance;
    }
};

} // namespace OpenTune
