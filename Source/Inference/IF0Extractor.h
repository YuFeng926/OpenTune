#pragma once

#include <vector>
#include <string>
#include <functional>

namespace OpenTune {

enum class F0ModelType {
    RMVPE = 0    // Robust Multi-scale Vocal Pitch Estimator (361MB)
};

struct F0ModelInfo {
    F0ModelType type;
    std::string name;
    std::string displayName;
    size_t modelSizeBytes;
    bool isAvailable;
};

/**
 * @brief Abstract interface for F0 (fundamental frequency) extraction models
 *
 * This interface allows using RMVPE F0 extraction algorithm at runtime.
 *
 * All implementations must:
 * - Accept raw audio at any sample rate
 * - Return F0 curve in Hz (one value per hop frame)
 * - Handle resampling internally to their target sample rate
 */
class IF0Extractor {
public:
    virtual ~IF0Extractor() = default;

    /**
     * @brief Extract F0 from audio buffer
     * @param audio Input audio buffer (raw PCM float samples)
     * @param length Number of samples in audio buffer
     * @param sampleRate Input sample rate (e.g., 44100, 48000, 96000)
     * @return F0 curve in Hz (one value per hop frame at target sample rate)
     *         Empty vector on error
     */
    virtual std::vector<float> extractF0(
        const float* audio,
        size_t length,
        int sampleRate,
        std::function<void(float)> progressCallback = nullptr,
        std::function<void(const std::vector<float>&, int)> partialCallback = nullptr
    ) = 0;

    /**
     * @brief Get hop size in samples at target sample rate
     * @return Hop size (e.g., 160 samples at 16kHz = 10ms frame interval)
     */
    virtual int getHopSize() const = 0;

    /**
     * @brief Get target sample rate for this extractor
     * @return Target sample rate in Hz (typically 16000 for F0 extraction)
     */
    virtual int getTargetSampleRate() const = 0;

    /**
     * @brief Get model type identifier
     */
    virtual F0ModelType getModelType() const = 0;

    /**
     * @brief Get human-readable model name
     * @return Model name (e.g., "RMVPE (Robust)")
     */
    virtual std::string getName() const = 0;

    /**
     * @brief Get estimated model size in bytes
     * @return Model size (used for VRAM management and display)
     */
    virtual size_t getModelSize() const = 0;

    /**
     * @brief Set confidence threshold for voiced/unvoiced detection
     * @param threshold Value between 0.0 and 1.0 (default typically 0.03-0.05)
     */
    virtual void setConfidenceThreshold(float threshold) { (void)threshold; }

    /**
     * @brief Set minimum F0 frequency
     * @param minHz Minimum frequency in Hz (default typically 30-50Hz)
     */
    virtual void setF0Min(float minHz) { (void)minHz; }

    /**
     * @brief Set maximum F0 frequency
     * @param maxHz Maximum frequency in Hz (default typically 1000-2000Hz)
     */
    virtual void setF0Max(float maxHz) { (void)maxHz; }

    virtual float getConfidenceThreshold() const { return 0.03f; }
    virtual float getF0Min() const { return 30.0f; }
    virtual float getF0Max() const { return 2000.0f; }
};

} // namespace OpenTune
