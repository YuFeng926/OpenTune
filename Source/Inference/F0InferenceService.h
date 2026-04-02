#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include "IF0Extractor.h"
#include "../Utils/Error.h"

namespace OpenTune {

/**
 * F0InferenceService - CPU-only F0 extraction service
 * 
 * Responsibilities:
 * - Manage F0 extractor lifecycle (CPU-only, no GPU)
 * - Support concurrent F0 extraction
 * - Handle model switching and configuration
 * 
 * Thread-safe: Yes (shared_mutex for model access)
 * Concurrency: Allows parallel extractF0 calls
 */
class F0InferenceService {
public:
    F0InferenceService();
    ~F0InferenceService();

    /**
     * Initialize F0 service with model directory
     * @param modelDir Path to model directory
     * @return true if initialization successful
     */
    bool initialize(const std::string& modelDir);

    /**
     * Shutdown and cleanup resources
     */
    void shutdown();

    /**
     * Extract F0 from audio
     * @param audio Audio samples
     * @param length Number of samples
     * @param sampleRate Sample rate
     * @param progressCallback Optional progress callback (0.0 to 1.0)
     * @param partialCallback Optional partial result callback
     * @return Result containing F0 vector
     */
    Result<std::vector<float>> extractF0(
        const float* audio,
        size_t length,
        int sampleRate,
        std::function<void(float)> progressCallback = nullptr,
        std::function<void(const std::vector<float>&, int)> partialCallback = nullptr);

    /**
     * Set F0 model type
     * @param type F0 model type (e.g., RMVPE)
     * @return true if model switched successfully
     */
    bool setF0Model(F0ModelType type);

    /**
     * Get current F0 model type
     */
    F0ModelType getCurrentF0Model() const;

    /**
     * Get available F0 models in model directory
     */
    std::vector<F0ModelInfo> getAvailableF0Models() const;

    /**
     * Configure F0 extraction parameters
     */
    void setConfidenceThreshold(float threshold);
    void setF0Min(float minFreq);
    void setF0Max(float maxFreq);

    float getConfidenceThreshold() const;
    float getF0Min() const;
    float getF0Max() const;

    /**
     * Get F0 extraction parameters
     */
    int getF0HopSize() const;
    int getF0SampleRate() const;

    bool isInitialized() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(F0InferenceService)
};

} // namespace OpenTune
