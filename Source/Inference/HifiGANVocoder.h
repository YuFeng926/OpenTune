#pragma once

#include "IVocoder.h"
#include <onnxruntime_cxx_api.h>
#include <memory>

namespace OpenTune {

/**
 * @brief HifiGAN neural vocoder implementation
 *
 * Model specs:
 * - Input: F0 curve + Mel spectrogram [1, 128, frames]
 * - Output: Audio at 44.1kHz
 * - Hop size: 512 samples at 44.1kHz
 * - Model size: ~14MB
 */
class HifiGANVocoder : public IVocoder {
public:
    explicit HifiGANVocoder(std::unique_ptr<Ort::Session> session);
    ~HifiGANVocoder() override;

    std::vector<float> synthesize(
        const std::vector<float>& f0,
        const float* mel = nullptr
    ) override;

    int getHopSize() const override { return 512; }
    int getSampleRate() const override { return 44100; }
    VocoderType getModelType() const override { return VocoderType::HifiGAN; }
    std::string getName() const override { return "HifiGAN Neural Synthesis"; }
    bool requiresMel() const override { return true; }
    size_t getModelSize() const override { return 14 * 1024 * 1024; }

private:
    std::unique_ptr<Ort::Session> session_;
    std::unique_ptr<Ort::MemoryInfo> memoryInfo_;
};

} // namespace OpenTune
