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
 * Memory/Latency Protection:
 * - Preflight resource check before inference
 * - Fail-fast with structured logging (no silent degradation)
 * - All-or-nothing: returns complete F0 or empty on failure
 * - Uses GpuDetector and system memory for conservative budgeting
 */
class RMVPEExtractor : public IF0Extractor {
public:
    /**
     * @brief Preflight check result for resource validation
     */
    struct PreflightResult {
        bool success = false;
        std::string errorMessage;
        std::string errorCategory; // "MEMORY", "DURATION", "MODEL", "SYSTEM"
        size_t estimatedMemoryMB = 0;
        size_t availableMemoryMB = 0;
        double audioDurationSec = 0.0;
    };

    RMVPEExtractor(
        std::unique_ptr<Ort::Session> session,
        std::shared_ptr<ResamplingManager> resampler
    );
    ~RMVPEExtractor() override;

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
     * @brief Perform preflight resource check before inference
     * @param audioLength Number of audio samples
     * @param sampleRate Audio sample rate
     * @return PreflightResult with success status and diagnostic info
     */
    PreflightResult preflightCheck(size_t audioLength, int sampleRate) const;

private:
    std::unique_ptr<Ort::Session> session_;
    std::shared_ptr<ResamplingManager> resampler_;
    std::unique_ptr<Ort::MemoryInfo> memoryInfo_;
    
    // Post-processing
    void fixOctaveErrors(std::vector<float>& f0);
    void fillF0Gaps(std::vector<float>& f0, int maxGapFrames);
    std::vector<float> computeSTFTFrame(const float* audioData, size_t totalSamples, int startSample);

    // FFT resources
    static constexpr int fftOrder_ = 10;
    static constexpr int fftSize_ = 1024;
    static constexpr int hopSize_ = 160;
    static constexpr int sampleRate_ = 16000;

    std::unique_ptr<juce::dsp::FFT> forwardFFT_;
    std::vector<float> hannWindow_;

    // Parameters
    float confidenceThreshold_ = 0.5f; // UV threshold for RMVPE
    float f0Min_ = 50.0f;
    float f0Max_ = 1100.0f;
    bool enableUvCheck_ = false;

    // ============================================================
    // Memory/Latency Protection Constants
    // ============================================================
    
    // Maximum audio duration for single-pass inference (seconds)
    // Beyond this, memory requirements become prohibitive
    // Conservative limit: 10 minutes at 16kHz = 9.6M samples = ~38MB input alone
    static constexpr double kMaxAudioDurationSec = 600.0; // 10 minutes
    
    // Memory overhead multiplier for ONNX Runtime intermediate tensors
    // ONNX inference typically needs 3-5x input size for intermediate activations
    static constexpr double kMemoryOverheadFactor = 6.0;
    
    // Minimum free memory to reserve (MB) - ensures system stability
    static constexpr size_t kMinReservedMemoryMB = 512;
    
    // Minimum VRAM/GPU memory threshold (MB) for GPU inference
    static constexpr size_t kMinGpuMemoryMB = 256;
    
    // Model static memory footprint estimate (MB)
    // RMVPE model is ~140MB, plus ONNX runtime overhead
    static constexpr size_t kModelMemoryMB = 200;
    
    // F0 gap filling: max gap duration in frames (80ms @ 10ms/frame = 8 frames)
    static constexpr int kMaxGapFramesDefault = 8;
    
    // Memory estimation helper
    size_t estimateMemoryRequiredMB(size_t audioSamples16k) const;
    
    // System memory query helper
    static size_t getAvailableSystemMemoryMB();
};

} // namespace OpenTune
