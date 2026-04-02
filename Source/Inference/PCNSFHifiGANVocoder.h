#pragma once

#include "VocoderInterface.h"
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <string>
#include <vector>

namespace OpenTune {

class PCNSFHifiGANVocoder : public VocoderInterface {
public:
    explicit PCNSFHifiGANVocoder(std::unique_ptr<Ort::Session> session);
    ~PCNSFHifiGANVocoder() override;

    std::vector<float> synthesize(
        const std::vector<float>& f0,
        const float* mel,
        size_t melSize
    ) override;

    VocoderInfo getInfo() const override;
    size_t estimateMemoryUsage(size_t frames) const override;
    
    int getHopSize() const override { return 512; }
    int getSampleRate() const override { return 44100; }
    int getMelBins() const override { return static_cast<int>(melBinsHint_); }
    std::string getName() const override { return "PC-NSF-HifiGAN (CPU)"; }
    bool requiresMel() const override { return true; }

private:
    std::unique_ptr<Ort::Session> session_;
    std::unique_ptr<Ort::MemoryInfo> memoryInfo_;
    std::vector<std::string> inputNames_;
    std::vector<std::string> outputNames_;
    std::vector<std::vector<int64_t>> inputShapes_;
    std::vector<ONNXTensorElementDataType> inputElemTypes_;

    int melIndex_{-1};
    int f0Index_{-1};
    int uvIndex_{-1};
    int64_t melBinsHint_{128};
    bool melNeedsTranspose_{false};
};

} // namespace OpenTune
