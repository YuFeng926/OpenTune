#pragma once

#include "VocoderInterface.h"
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <string>
#include <vector>

namespace OpenTune {

struct VocoderScratchBuffers {
    std::vector<float> conditioningOwned;
    std::vector<float> uvData;
    std::vector<float> conditioningTransposed;
    std::vector<const char*> inputNamesC;
    std::vector<Ort::Value> inputTensors;
    std::vector<std::string> inputNameStorage;
    std::vector<std::vector<float>> extraFloatBuffers;
    std::vector<std::vector<int64_t>> extraInt64Buffers;

    void resetForRun(size_t frameCount, size_t inputCount);
};

class OnnxVocoderBase : public VocoderInterface {
public:
    ~OnnxVocoderBase() override = default;

    std::vector<float> synthesize(
        const std::vector<float>& f0,
        const std::vector<float>& uv,
        const float* conditioning,
        size_t conditioningSize,
        Ort::RunOptions& runOptions) override;

    int getHopSize() const override { return 512; }
    int getSampleRate() const override { return 44100; }
    int getConditioningBins() const override { return static_cast<int>(conditioningBinsHint_); }
    VocoderConditioningType getConditioningType() const override { return conditioningType_; }
    float getFMax() const override { return fMax_; }
    void setMelFMax(float fMax) override { fMax_ = fMax; }

protected:
    void detectInputOutputNames();
    void prepareInputTensors(
        VocoderScratchBuffers& scratch,
        const std::vector<float>& f0,
        const std::vector<float>& uv,
        const float* conditioning,
        size_t conditioningSize,
        Ort::MemoryInfo& memoryInfo);

    virtual std::vector<float> runSession(
        VocoderScratchBuffers& scratch,
        size_t numFrames,
        Ort::RunOptions& runOptions) = 0;

    std::unique_ptr<Ort::Session> session_;
    std::vector<std::string> inputNames_;
    std::vector<std::string> outputNames_;
    std::vector<std::vector<int64_t>> inputShapes_;
    std::vector<ONNXTensorElementDataType> inputElemTypes_;

    int conditioningIndex_ = -1;
    int f0Index_ = -1;
    int uvIndex_ = -1;
    int64_t conditioningBinsHint_ = 128;
    VocoderConditioningType conditioningType_ = VocoderConditioningType::LogMel;
    float fMax_ = 16000.0f;
    bool conditioningNeedsTranspose_ = false;
};

} // namespace OpenTune
