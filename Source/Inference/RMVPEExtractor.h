#pragma once

#include "IF0Extractor.h"
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <vector>
#include <juce_dsp/juce_dsp.h>

namespace OpenTune {

class ResamplingManager;

/**
 * @brief RMVPE (Robust Multi-scale Vocal Pitch Estimator) implementation
 *
 * Uses RMVPE ONNX model.
 * Model specs:
 * - Input: Waveform [1, num_samples], Threshold [1]
 * - Output: F0 [1, num_frames], UV [1, num_frames]
 * - Hop size: 160 samples at 16kHz (10ms frames)
 *
 * Contract:
 * - Preflight checks only validate basic input/session preconditions
 * - Inference is single-pass on the full resampled waveform
 * - Failures are surfaced directly as exceptions
 */
class RMVPEExtractor : public IF0Extractor {
public:
    /**
     * @brief Preflight check result for basic contract validation
     */
    struct PreflightResult {
        bool success = false;
        std::string errorMessage;
    };

    RMVPEExtractor(
        std::unique_ptr<Ort::Session> session,
        std::shared_ptr<ResamplingManager> resampler
    );
    ~RMVPEExtractor() override;

    /**
     * @brief Extract F0 curve from audio
     * 
     * @param audio Non-null pointer to audio samples (contract: must not be null)
     * @param length Number of audio samples (contract: must be > 0)
     * @param sampleRate Audio sample rate in Hz (contract: must be > 0)
     * @param progressCallback Optional progress callback (0.0 to 1.0)
     * @param partialCallback Optional callback for partial F0 results
     * 
     * @return F0 values in Hz, one per frame (10ms hop at 16kHz)
     * 
     * @throws std::invalid_argument if audio is null, length is zero, or sampleRate <= 0
     * @throws std::logic_error if session or resampler not initialized
     * @throws std::runtime_error if preflight check fails or inference fails
     */
    std::vector<float> extractF0(
        const float* audio,
        size_t length,
        int sampleRate,
        std::function<void(float)> progressCallback = nullptr,
        std::function<void(const std::vector<float>&, int)> partialCallback = nullptr
    ) override;

    int getHopSize() const override { return 160; }
    int getTargetSampleRate() const override { return 16000; }
    F0ModelType getModelType() const override { return F0ModelType::RMVPE; }
    std::string getName() const override { return "RMVPE"; }
    size_t getModelSize() const override { return 0; } // Varies

    void setConfidenceThreshold(float threshold) override { confidenceThreshold_ = threshold; }
    void setF0Min(float minHz) override { f0Min_ = minHz; }
    void setF0Max(float maxHz) override { f0Max_ = maxHz; }
    void setEnableUvCheck(bool enable) { enableUvCheck_ = enable; }

    float getConfidenceThreshold() const override { return confidenceThreshold_; }
    float getF0Min() const override { return f0Min_; }
    float getF0Max() const override { return f0Max_; }
    bool getEnableUvCheck() const { return enableUvCheck_; }

    /**
     * @brief Validate preconditions before inference
     * @param audioLength Number of audio samples
     * @param sampleRate Audio sample rate
     * @return PreflightResult with success flag and error message
     */
    PreflightResult preflightCheck(size_t audioLength, int sampleRate) const;

private:
    std::unique_ptr<Ort::Session> session_;
    std::shared_ptr<ResamplingManager> resampler_;
    std::unique_ptr<Ort::MemoryInfo> memoryInfo_;
    
    // Post-processing
    void fixOctaveErrors(std::vector<float>& f0);
    void fillF0Gaps(std::vector<float>& f0, int maxGapFrames);

    // Parameters
    float confidenceThreshold_ = 0.5f; // UV threshold for RMVPE
    float f0Min_ = 50.0f;
    float f0Max_ = 1100.0f;
    bool enableUvCheck_ = false;

    // ============================================================
    // Post-processing Constants
    // ============================================================
    
    // F0 gap filling: max gap duration in frames (80ms @ 10ms/frame = 8 frames)
    static constexpr int kMaxGapFramesDefault = 8;
};

} // namespace OpenTune
