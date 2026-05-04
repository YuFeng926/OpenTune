#include "OnnxVocoderBase.h"
#include "../Utils/AppLogger.h"
#include <algorithm>
#include <cctype>

namespace OpenTune {

namespace {
std::string toLowerCopy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
} // namespace

void VocoderScratchBuffers::resetForRun(size_t frameCount, size_t inputCount) {
    if (uvData.capacity() < frameCount)
        uvData.reserve(frameCount);
    uvData.clear();

    melOwned.clear();
    melTransposed.clear();

    inputNamesC.clear();
    if (inputNamesC.capacity() < inputCount) inputNamesC.reserve(inputCount);

    inputTensors.clear();
    if (inputTensors.capacity() < inputCount) inputTensors.reserve(inputCount);

    inputNameStorage.clear();
    if (inputNameStorage.capacity() < inputCount) inputNameStorage.reserve(inputCount);

    extraFloatBuffers.clear();
    if (extraFloatBuffers.capacity() < inputCount) extraFloatBuffers.reserve(inputCount);

    extraInt64Buffers.clear();
    if (extraInt64Buffers.capacity() < inputCount) extraInt64Buffers.reserve(inputCount);
}

void OnnxVocoderBase::detectInputOutputNames() {
    if (!session_) return;

    Ort::AllocatorWithDefaultOptions allocator;

    const size_t inputCount = session_->GetInputCount();
    inputNames_.reserve(inputCount);
    inputShapes_.reserve(inputCount);
    inputElemTypes_.reserve(inputCount);

    for (size_t i = 0; i < inputCount; ++i) {
        auto name = session_->GetInputNameAllocated(i, allocator);
        inputNames_.push_back(name.get());

        auto inputTypeInfo = session_->GetInputTypeInfo(i);
        auto tensorInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
        inputShapes_.push_back(tensorInfo.GetShape());
        inputElemTypes_.push_back(tensorInfo.GetElementType());
    }

    const size_t outputCount = session_->GetOutputCount();
    outputNames_.reserve(outputCount);
    for (size_t i = 0; i < outputCount; ++i) {
        auto name = session_->GetOutputNameAllocated(i, allocator);
        outputNames_.push_back(name.get());
    }

    for (int i = 0; i < static_cast<int>(inputNames_.size()); ++i) {
        const auto lowered = toLowerCopy(inputNames_[static_cast<size_t>(i)]);
        if (melIndex_ < 0 && lowered.find("mel") != std::string::npos) melIndex_ = i;
        if (f0Index_ < 0 && (lowered == "f0" || lowered.find("f0") != std::string::npos || lowered.find("pitch") != std::string::npos)) f0Index_ = i;
        if (uvIndex_ < 0 && (lowered.find("uv") != std::string::npos || lowered.find("voiced") != std::string::npos)) uvIndex_ = i;
    }

    if (melIndex_ < 0) {
        for (int i = 0; i < static_cast<int>(inputNames_.size()); ++i) {
            if (toLowerCopy(inputNames_[static_cast<size_t>(i)]) == "c") { melIndex_ = i; break; }
        }
    }

    if (melIndex_ >= 0 && melIndex_ < static_cast<int>(inputShapes_.size())) {
        const auto& melShape = inputShapes_[static_cast<size_t>(melIndex_)];
        for (auto d : melShape) {
            if (d > 1 && d != 128) {
                melBinsHint_ = d;
                break;
            }
        }
        melNeedsTranspose_ = (melShape.size() == 3 && melShape[2] == 128);
    }
}

void OnnxVocoderBase::prepareInputTensors(
    VocoderScratchBuffers& scratch,
    const std::vector<float>& f0,
    const float* mel,
    size_t melSize,
    Ort::MemoryInfo& memoryInfo)
{
    const size_t numFrames = f0.size();
    constexpr int64_t melBinsDefault = 128;

    auto resolveShape = [this, numFrames, melBinsDefault](const std::vector<int64_t>& rawShape, int64_t melBinsValue) -> std::vector<int64_t> {
        std::vector<int64_t> out = rawShape;
        for (auto& d : out) {
            if (d <= 0)
                d = static_cast<int64_t>(numFrames);
            else if (d == melBinsDefault)
                d = melBinsValue;
        }
        return out;
    };

    int64_t melBins = (melBinsHint_ > 0) ? melBinsHint_ : melBinsDefault;

    if (melIndex_ >= 0) {
        const auto& raw = inputShapes_[static_cast<size_t>(melIndex_)];
        for (auto d : raw) {
            if (d > 1 && d != static_cast<int64_t>(numFrames)) melBins = d;
        }
    }

    const size_t expectedMelSize = static_cast<size_t>(melBins) * numFrames;

    if (mel == nullptr) {
        scratch.melOwned.resize(expectedMelSize, 0.0f);
        mel = scratch.melOwned.data();
        melSize = expectedMelSize;
    } else if (melSize != expectedMelSize) {
        throw std::runtime_error("Vocoder: mel size mismatch. Expected "
            + std::to_string(expectedMelSize) + " (" + std::to_string(melBins) + " bins x "
            + std::to_string(numFrames) + " frames), got " + std::to_string(melSize));
    }

    scratch.uvData.resize(numFrames);
    for (size_t i = 0; i < numFrames; ++i)
        scratch.uvData[i] = (f0[i] > 0.0f) ? 1.0f : 0.0f;

    const float* melToUse = mel;
    if (melIndex_ >= 0 && melNeedsTranspose_) {
        scratch.melTransposed.resize(static_cast<size_t>(melBins) * numFrames);
        for (size_t t = 0; t < numFrames; ++t) {
            for (int64_t m = 0; m < melBins; ++m) {
                scratch.melTransposed[t * static_cast<size_t>(melBins) + static_cast<size_t>(m)] =
                    mel[static_cast<size_t>(m) * numFrames + t];
            }
        }
        melToUse = scratch.melTransposed.data();
    }

    auto addFloatTensor = [&](const std::string& name, const float* data, size_t dataCount, const std::vector<int64_t>& shape) {
        scratch.inputNameStorage.push_back(name);
        scratch.inputNamesC.push_back(scratch.inputNameStorage.back().c_str());
        scratch.inputTensors.push_back(Ort::Value::CreateTensor<float>(memoryInfo, const_cast<float*>(data), dataCount, shape.data(), shape.size()));
    };

    auto addOwnedFloatTensor = [&](const std::string& name, std::vector<float>&& data, const std::vector<int64_t>& shape) {
        scratch.extraFloatBuffers.push_back(std::move(data));
        addFloatTensor(name, scratch.extraFloatBuffers.back().data(), scratch.extraFloatBuffers.back().size(), shape);
    };

    auto addOwnedInt64Tensor = [&](const std::string& name, std::vector<int64_t>&& data, const std::vector<int64_t>& shape) {
        scratch.inputNameStorage.push_back(name);
        scratch.inputNamesC.push_back(scratch.inputNameStorage.back().c_str());
        scratch.extraInt64Buffers.push_back(std::move(data));
        scratch.inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(memoryInfo, scratch.extraInt64Buffers.back().data(), scratch.extraInt64Buffers.back().size(), shape.data(), shape.size()));
    };

    if (melIndex_ >= 0) {
        const auto& raw = inputShapes_[static_cast<size_t>(melIndex_)];
        auto shape = resolveShape(raw, melBins);
        addFloatTensor(inputNames_[static_cast<size_t>(melIndex_)], melToUse, static_cast<size_t>(melBins) * numFrames, shape);
    }

    if (f0Index_ >= 0) {
        const auto& raw = inputShapes_[static_cast<size_t>(f0Index_)];
        auto shape = resolveShape(raw, melBins);
        addFloatTensor(inputNames_[static_cast<size_t>(f0Index_)], f0.data(), numFrames, shape);
    }

    if (uvIndex_ >= 0) {
        const auto& raw = inputShapes_[static_cast<size_t>(uvIndex_)];
        auto shape = resolveShape(raw, melBins);
        addFloatTensor(inputNames_[static_cast<size_t>(uvIndex_)], scratch.uvData.data(), numFrames, shape);
    }

    for (size_t i = 0; i < inputNames_.size(); ++i) {
        if (static_cast<int>(i) == melIndex_ || static_cast<int>(i) == f0Index_ || static_cast<int>(i) == uvIndex_) continue;

        const auto& rawShape = inputShapes_[i];
        const auto elemType = inputElemTypes_[i];

        auto shape = resolveShape(rawShape, melBins);
        size_t count = 1;
        for (auto d : shape) count *= static_cast<size_t>(std::max<int64_t>(1, d));

        if (elemType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
            std::vector<int64_t> zeros(count, 0);
            addOwnedInt64Tensor(inputNames_[i], std::move(zeros), shape);
        } else {
            std::vector<float> zeros(count, 0.0f);
            addOwnedFloatTensor(inputNames_[i], std::move(zeros), shape);
        }
    }
}

std::vector<float> OnnxVocoderBase::synthesize(
    const std::vector<float>& f0,
    const float* mel,
    size_t melSize)
{
    if (!session_)
        throw std::runtime_error("Vocoder: session not initialized");

    if (f0.empty())
        throw std::runtime_error("Vocoder: f0 is empty");

    thread_local VocoderScratchBuffers scratch;
    scratch.resetForRun(f0.size(), inputNames_.size());

    Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
    prepareInputTensors(scratch, f0, mel, melSize, memoryInfo);

    return runSession(scratch, f0.size());
}

} // namespace OpenTune
