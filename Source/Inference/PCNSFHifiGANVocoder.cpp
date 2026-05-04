#include "PCNSFHifiGANVocoder.h"
#include "../Utils/AppLogger.h"

namespace OpenTune {

PCNSFHifiGANVocoder::PCNSFHifiGANVocoder(std::unique_ptr<Ort::Session> session) {
    session_ = std::move(session);
    detectInputOutputNames();

    juce::String inputsJoined;
    for (size_t i = 0; i < inputNames_.size(); ++i) {
        if (i > 0) inputsJoined << ", ";
        inputsJoined << inputNames_[i];
    }
    AppLogger::info("PCNSFHifiGANVocoder inputs: " + inputsJoined);

    for (size_t i = 0; i < inputNames_.size(); ++i) {
        const auto& shape = inputShapes_[i];
        juce::String shapeText;
        for (size_t d = 0; d < shape.size(); ++d) {
            if (d > 0) shapeText << "x";
            shapeText << juce::String(shape[d]);
        }
        AppLogger::info("PCNSF input[" + juce::String(static_cast<int>(i)) + "] "
            + juce::String(inputNames_[i]) + " shape=" + shapeText);
    }
}

PCNSFHifiGANVocoder::~PCNSFHifiGANVocoder() = default;

std::vector<float> PCNSFHifiGANVocoder::runSession(
    VocoderScratchBuffers& scratch,
    size_t /*numFrames*/)
{
    std::vector<const char*> outputNamesC;
    if (!outputNames_.empty()) {
        for (const auto& o : outputNames_)
            outputNamesC.push_back(o.c_str());
    } else {
        outputNamesC.push_back("audio");
    }

    auto out = session_->Run(
        Ort::RunOptions{nullptr},
        scratch.inputNamesC.data(),
        scratch.inputTensors.data(),
        scratch.inputTensors.size(),
        outputNamesC.data(),
        outputNamesC.size());

    if (out.empty())
        throw std::runtime_error("PCNSFHifiGANVocoder: ONNX output is empty");

    auto& outputTensor = out[0];
    auto shapeInfo = outputTensor.GetTensorTypeAndShapeInfo();
    auto audioShape = shapeInfo.GetShape();
    auto elementType = shapeInfo.GetElementType();

    if (elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("PCNSFHifiGANVocoder: Output tensor type is not float");

    if (audioShape.empty())
        throw std::runtime_error("PCNSFHifiGANVocoder: Invalid output shape");

    if (audioShape.size() == 2 && audioShape[0] != 1)
        throw std::runtime_error("PCNSFHifiGANVocoder: Output batch must be 1");

    const size_t audioLength = static_cast<size_t>(
        audioShape.size() == 2 ? audioShape[1] : audioShape[0]);

    if (audioLength == 0)
        throw std::runtime_error("PCNSFHifiGANVocoder: Audio output length is zero");

    float* audioData = outputTensor.GetTensorMutableData<float>();
    return std::vector<float>(audioData, audioData + audioLength);
}

} // namespace OpenTune
