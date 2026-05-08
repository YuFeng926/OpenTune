#pragma once

/**
 * MDXNetInferenceService - MDX-NET vocal separation inference
 *
 * Uses UVR_MDXNET_KARA ONNX model to extract clean vocals from mixed audio.
 * The "Other" output channel contains the extracted vocal stem.
 *
 * Model I/O:
 *   Input:  [1, 4, 2048, 256]  4-channel complex STFT (stereo real+imag)
 *   Output: [1, 4, 2048, 256]  processed vocal spectrogram
 *
 * Pipeline:
 *   audio → resample to 44100 → stereo (dup mono) → STFT(4096, hop=1024)
 *   → chunk into 256-frame overlapping windows
 *   → ONNX inference per chunk → overlap-add → ISTFT → mono output
 *
 * Thread-safe: No (single-threaded, call from background worker)
 * Lifecycle: Lazy-init on first use
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <juce_core/juce_core.h>

namespace Ort { struct Env; struct Session; struct MemoryInfo; }

namespace OpenTune {

class MDXNetInferenceService
{
public:
    MDXNetInferenceService(std::shared_ptr<Ort::Env> env);
    ~MDXNetInferenceService();

    /** Initialize with path to models directory.
     *  The model file must be at modelsDir/mdxnet_kara.onnx.
     *  Returns true on success. */
    bool initialize(const std::string& modelsDir);

    /** Release ONNX session and buffers. */
    void shutdown();

    /** Extract clean vocals from mixed audio.
     *  @param audio  Mono or stereo interleaved audio samples
     *  @param numSamples  Number of samples
     *  @param sampleRate  Input sample rate (will be resampled to 44100Hz internally)
     *  @param numChannels  Input channel count (1 = mono, 2 = stereo)
     *  @return Mono vocal audio at 44100Hz, or empty on failure */
    std::vector<float> extractVocals(
        const float* audio,
        int64_t numSamples,
        int sampleRate,
        int numChannels);

    bool isInitialized() const { return initialized_; }

private:
    // STFT parameters
    static constexpr int kFftSize = 4096;
    static constexpr int kHopSize = 1024;
    static constexpr int kFftOrder = 12;
    static constexpr int kDimF = 2048;       // n_fft/2+1 - 1 (pad to 2049 internally)
    static constexpr int kDimT = 256;         // model chunk size (time frames)
    static constexpr int kModelChannels = 4;  // 2 audio channels × 2 real/imag
    static constexpr int kTargetSampleRate = 44100;

    // Overlap: 75% → chunk step = 256 * 0.25 = 64 frames
    static constexpr int kChunkStep = 64;

    struct StftBuffers;
    std::unique_ptr<StftBuffers> stft_;

    std::shared_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    std::unique_ptr<Ort::MemoryInfo> memoryInfo_;
    bool initialized_ = false;
};

} // namespace OpenTune
