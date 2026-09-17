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

    conditioningOwned.clear();
    conditioningTransposed.clear();

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
        if (conditioningIndex_ < 0 && lowered.find("linear") != std::string::npos) {
            conditioningIndex_ = i;
            conditioningType_ = VocoderConditioningType::LogLinearSpec;
        }
        if (conditioningIndex_ < 0 && lowered.find("mel") != std::string::npos) {
            conditioningIndex_ = i;
            conditioningType_ = VocoderConditioningType::LogMel;
        }
        if (f0Index_ < 0 && (lowered == "f0" || lowered.find("f0") != std::string::npos || lowered.find("pitch") != std::string::npos)) f0Index_ = i;
        if (uvIndex_ < 0 && (lowered.find("uv") != std::string::npos || lowered.find("voiced") != std::string::npos)) uvIndex_ = i;
    }

    if (conditioningIndex_ < 0) {
        for (int i = 0; i < static_cast<int>(inputNames_.size()); ++i) {
            if (toLowerCopy(inputNames_[static_cast<size_t>(i)]) == "c") { conditioningIndex_ = i; break; }
        }
    }

    if (conditioningIndex_ >= 0 && conditioningIndex_ < static_cast<int>(inputShapes_.size())) {
        const auto& conditioningShape = inputShapes_[static_cast<size_t>(conditioningIndex_)];
        // 最后一个静态维(>1)即条件维：兼容 frames-major [1,T,bins] 与
        // bins-major [1,bins,T] 两种导出布局（动态帧维在 ORT shape 中为 0）。
        int64_t shapeBins = 0;
        for (auto d : conditioningShape) {
            if (d > 1)
                shapeBins = d;
        }
        if (shapeBins > 0)
            conditioningBinsHint_ = shapeBins;
        conditioningNeedsTranspose_ = (conditioningShape.size() == 3 && shapeBins > 0 && conditioningShape[2] == shapeBins);
    }
}

void OnnxVocoderBase::prepareInputTensors(
    VocoderScratchBuffers& scratch,
    const std::vector<float>& f0,
    const std::vector<float>& uv,
    const float* conditioning,
    size_t conditioningSize,
    Ort::MemoryInfo& memoryInfo)
{
    const size_t numFrames = f0.size();
    constexpr int64_t conditioningBinsDefault = 128;

    auto resolveShape = [this, numFrames, conditioningBinsDefault](const std::vector<int64_t>& rawShape, int64_t conditioningBinsValue) -> std::vector<int64_t> {
        std::vector<int64_t> out = rawShape;
        for (auto& d : out) {
            if (d <= 0)
                d = static_cast<int64_t>(numFrames);
            else if (d == conditioningBinsDefault)
                d = conditioningBinsValue;
        }
        return out;
    };

    int64_t conditioningBins = (conditioningBinsHint_ > 0) ? conditioningBinsHint_ : conditioningBinsDefault;

    if (conditioningIndex_ >= 0) {
        const auto& raw = inputShapes_[static_cast<size_t>(conditioningIndex_)];
        for (auto d : raw) {
            if (d > 1 && d != static_cast<int64_t>(numFrames)) conditioningBins = d;
        }
    }

    const size_t expectedConditioningSize = static_cast<size_t>(conditioningBins) * numFrames;

    if (conditioning == nullptr) {
        scratch.conditioningOwned.resize(expectedConditioningSize, 0.0f);
        conditioning = scratch.conditioningOwned.data();
        conditioningSize = expectedConditioningSize;
    } else if (conditioningSize != expectedConditioningSize) {
        throw std::runtime_error("Vocoder: conditioning size mismatch. Expected "
            + std::to_string(expectedConditioningSize) + " (" + std::to_string(conditioningBins) + " bins x "
            + std::to_string(numFrames) + " frames), got " + std::to_string(conditioningSize));
    }

    scratch.uvData.resize(numFrames);
    if (uv.empty()) {
        // 旧行为：模型无显式 UV 时由 f0>0 推导（1=voiced）。
        for (size_t i = 0; i < numFrames; ++i)
            scratch.uvData[i] = (f0[i] > 0.0f) ? 1.0f : 0.0f;
    } else {
        if (uv.size() != numFrames)
            throw std::runtime_error("Vocoder: uv frame count mismatch. Expected "
                + std::to_string(numFrames) + ", got " + std::to_string(uv.size()));
        scratch.uvData = uv;
    }

    const float* conditioningToUse = conditioning;
    if (conditioningIndex_ >= 0 && conditioningNeedsTranspose_) {
        scratch.conditioningTransposed.resize(static_cast<size_t>(conditioningBins) * numFrames);
        for (size_t t = 0; t < numFrames; ++t) {
            for (int64_t m = 0; m < conditioningBins; ++m) {
                scratch.conditioningTransposed[t * static_cast<size_t>(conditioningBins) + static_cast<size_t>(m)] =
                    conditioning[static_cast<size_t>(m) * numFrames + t];
            }
        }
        conditioningToUse = scratch.conditioningTransposed.data();
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

    if (conditioningIndex_ >= 0) {
        const auto& raw = inputShapes_[static_cast<size_t>(conditioningIndex_)];
        auto shape = resolveShape(raw, conditioningBins);
        addFloatTensor(inputNames_[static_cast<size_t>(conditioningIndex_)], conditioningToUse, static_cast<size_t>(conditioningBins) * numFrames, shape);
    }

    if (f0Index_ >= 0) {
        const auto& raw = inputShapes_[static_cast<size_t>(f0Index_)];
        auto shape = resolveShape(raw, conditioningBins);
        addFloatTensor(inputNames_[static_cast<size_t>(f0Index_)], f0.data(), numFrames, shape);
    }

    if (uvIndex_ >= 0) {
        const auto& raw = inputShapes_[static_cast<size_t>(uvIndex_)];
        auto shape = resolveShape(raw, conditioningBins);
        addFloatTensor(inputNames_[static_cast<size_t>(uvIndex_)], scratch.uvData.data(), numFrames, shape);
    }

    for (size_t i = 0; i < inputNames_.size(); ++i) {
        if (static_cast<int>(i) == conditioningIndex_ || static_cast<int>(i) == f0Index_ || static_cast<int>(i) == uvIndex_) continue;

        const auto& rawShape = inputShapes_[i];
        const auto elemType = inputElemTypes_[i];

        auto shape = resolveShape(rawShape, conditioningBins);
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
    const std::vector<float>& uv,
    const float* conditioning,
    size_t conditioningSize,
    Ort::RunOptions& runOptions)
{
    if (!session_)
        throw std::runtime_error("Vocoder: session not initialized");

    if (f0.empty())
        throw std::runtime_error("Vocoder: f0 is empty");

    thread_local VocoderScratchBuffers scratch;
    scratch.resetForRun(f0.size(), inputNames_.size());

    Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
    prepareInputTensors(scratch, f0, uv, conditioning, conditioningSize, memoryInfo);

    return runSession(scratch, f0.size(), runOptions);
}

} // namespace OpenTune
