#pragma once

#include "VocoderInterface.h"
#include "DmlConfig.h"
#include <onnxruntime_cxx_api.h>
#ifdef _WIN32
#include <dml_provider_factory.h>
#endif
#include <memory>
#include <mutex>
#include <vector>
#include <string>

namespace OpenTune {

class DmlVocoder : public VocoderInterface {
public:
    explicit DmlVocoder(const std::string& modelPath,
                          Ort::Env& env,
                          const DmlConfig& config);

    ~DmlVocoder() override;

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
    std::string getName() const override { return "PC-NSF-HifiGAN (DML)"; }
    bool requiresMel() const override { return true; }

private:
    void initializeSession(const std::string& modelPath,
                           Ort::Env& env,
                           const DmlConfig& config);
    void detectInputOutputNames();
    void initializeIOBinding();
    std::vector<float> synthesizeWithIOBinding(
        const std::vector<float>& f0,
        const float* mel,
        size_t melSize
    );

    std::unique_ptr<Ort::Session> session_;
    std::unique_ptr<Ort::IoBinding> ioBinding_;
    
    Ort::MemoryInfo cpuMemoryInfo_;
    
    std::unique_ptr<Ort::Value> preallocatedOutput_;
    std::vector<float> outputBuffer_;
    size_t preallocatedFrames_ = 0;
    bool ioBindingInitialized_ = false;
    mutable std::mutex runMutex_;

    std::vector<std::string> inputNames_;
    std::vector<std::string> outputNames_;
    std::vector<std::vector<int64_t>> inputShapes_;
    std::vector<ONNXTensorElementDataType> inputElemTypes_;

    int melIndex_ = -1;
    int f0Index_ = -1;
    int uvIndex_ = -1;
    int64_t melBinsHint_ = 128;
    bool melNeedsTranspose_ = false;

};

} // namespace OpenTune
