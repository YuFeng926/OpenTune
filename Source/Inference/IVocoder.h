#pragma once

#include <vector>
#include <string>

namespace OpenTune {

enum class VocoderType {
    HifiGAN = 0,
    PC_NSF_HifiGAN = 1,
    FishGan = 2
    // Future: NSF = 1, PWG = 2, BigVGAN = 3, etc.
};

struct VocoderInfo {
    VocoderType type;
    std::string name;
    std::string displayName;
    size_t modelSizeBytes;
    bool isAvailable;
};

/**
 * @brief Abstract interface for neural vocoders
 *
 * This interface allows switching between different neural vocoder models
 * (HifiGAN, NSF, PWG, etc.) for high-quality audio synthesis.
 *
 * All implementations must:
 * - Accept F0 curve and optional mel spectrogram
 * - Return synthesized audio at their target sample rate
 * - Handle padding and crossfading internally
 */
class IVocoder {
public:
    virtual ~IVocoder() = default;

    /**
     * @brief Synthesize audio from F0 and optional mel spectrogram
     * @param f0 F0 curve in Hz (one value per hop frame)
     * @param mel Optional mel spectrogram (can be nullptr for some vocoders)
     * @return Synthesized audio samples at vocoder's sample rate
     *         Empty vector on error
     */
    virtual std::vector<float> synthesize(
        const std::vector<float>& f0,
        const float* mel = nullptr
    ) = 0;

    /**
     * @brief Get hop size in samples at vocoder's sample rate
     * @return Hop size (e.g., 512 samples at 44.1kHz for HifiGAN)
     */
    virtual int getHopSize() const = 0;

    /**
     * @brief Get output sample rate
     * @return Sample rate in Hz (e.g., 44100 for HifiGAN)
     */
    virtual int getSampleRate() const = 0;

    /**
     * @brief Get vocoder type identifier
     */
    virtual VocoderType getModelType() const = 0;

    /**
     * @brief Get human-readable vocoder name
     * @return Vocoder name (e.g., "HifiGAN Neural Synthesis")
     */
    virtual std::string getName() const = 0;

    /**
     * @brief Check if this vocoder requires mel spectrogram input
     * @return true if mel is required, false if F0-only synthesis
     */
    virtual bool requiresMel() const = 0;

    /**
     * @brief Get estimated model size in bytes
     * @return Model size (used for VRAM management and display)
     */
    virtual size_t getModelSize() const = 0;
};

} // namespace OpenTune
