#include "PCNSFHifiGANVocoder.h"
#include "RenderCache.h"
#include "../Utils/AppLogger.h"
#include <algorithm>
#include <cctype>

namespace OpenTune {

static std::string toLowerCopy(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return out;
}

struct PCNSFScratchBuffers {
    std::vector<float> melOwned;
    std::vector<float> uvData;
    std::vector<float> melTransposed;
    std::vector<const char*> inputNamesC;
    std::vector<Ort::Value> inputTensors;
    std::vector<std::string> inputNameStorage;
    std::vector<std::vector<float>> extraFloatBuffers;
    std::vector<std::vector<int64_t>> extraInt64Buffers;
    std::vector<const char*> outputNamesC;

    void resetForRun(size_t frameCount, size_t inputCount, size_t outputCount)
    {
        if (uvData.capacity() < frameCount) uvData.reserve(frameCount);
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

        outputNamesC.clear();
        if (outputNamesC.capacity() < outputCount) outputNamesC.reserve(outputCount);
    }
};

size_t estimateScratchBytes(const PCNSFScratchBuffers& scratch)
{
    size_t total = 0;
    total += scratch.melOwned.capacity() * sizeof(float);
    total += scratch.uvData.capacity() * sizeof(float);
    total += scratch.melTransposed.capacity() * sizeof(float);
    total += scratch.inputNamesC.capacity() * sizeof(const char*);
    total += scratch.inputTensors.capacity() * sizeof(Ort::Value);
    total += scratch.inputNameStorage.capacity() * sizeof(std::string);
    total += scratch.extraFloatBuffers.capacity() * sizeof(std::vector<float>);
    for (const auto& buf : scratch.extraFloatBuffers) {
        total += buf.capacity() * sizeof(float);
    }
    total += scratch.extraInt64Buffers.capacity() * sizeof(std::vector<int64_t>);
    for (const auto& buf : scratch.extraInt64Buffers) {
        total += buf.capacity() * sizeof(int64_t);
    }
    total += scratch.outputNamesC.capacity() * sizeof(const char*);
    return total;
}

PCNSFHifiGANVocoder::PCNSFHifiGANVocoder(std::unique_ptr<Ort::Session> session)
    : session_(std::move(session))
{
    memoryInfo_ = std::make_unique<Ort::MemoryInfo>(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

    if (session_)
    {
        Ort::AllocatorWithDefaultOptions allocator;

        const size_t inputCount = session_->GetInputCount();
        inputNames_.reserve(inputCount);
        inputShapes_.reserve(inputCount);
        inputElemTypes_.reserve(inputCount);
        for (size_t i = 0; i < inputCount; ++i)
        {
            auto name = session_->GetInputNameAllocated(i, allocator);
            inputNames_.push_back(name.get());

            const auto typeInfo = session_->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo();
            inputShapes_.push_back(typeInfo.GetShape());
            inputElemTypes_.push_back(typeInfo.GetElementType());
        }

        const size_t outputCount = session_->GetOutputCount();
        outputNames_.reserve(outputCount);
        for (size_t i = 0; i < outputCount; ++i)
        {
            auto name = session_->GetOutputNameAllocated(i, allocator);
            outputNames_.push_back(name.get());
        }

        juce::String inputsJoined;
        for (size_t i = 0; i < inputNames_.size(); ++i) {
            if (i > 0) inputsJoined << ", ";
            inputsJoined << inputNames_[i];
        }
        juce::String outputsJoined;
        for (size_t i = 0; i < outputNames_.size(); ++i) {
            if (i > 0) outputsJoined << ", ";
            outputsJoined << outputNames_[i];
        }
        AppLogger::info("PCNSFHifiGANVocoder inputs: " + inputsJoined);
        AppLogger::info("PCNSFHifiGANVocoder outputs: " + outputsJoined);

        for (size_t i = 0; i < inputCount; ++i)
        {
            const auto& shape = inputShapes_[i];
            juce::String shapeText;
            for (size_t d = 0; d < shape.size(); ++d) {
                if (d > 0) shapeText << "x";
                shapeText << juce::String(shape[d]);
            }
            AppLogger::info("PCNSF input[" + juce::String((int)i) + "] " + juce::String(inputNames_[i]) + " shape=" + shapeText);
        }

        for (int i = 0; i < (int) inputNames_.size(); ++i)
        {
            const auto lowered = toLowerCopy(inputNames_[(size_t) i]);
            if (melIndex_ < 0 && lowered.find("mel") != std::string::npos) melIndex_ = i;
            if (f0Index_ < 0 && (lowered == "f0" || lowered.find("f0") != std::string::npos || lowered.find("pitch") != std::string::npos)) f0Index_ = i;
            if (uvIndex_ < 0 && (lowered.find("uv") != std::string::npos || lowered.find("voiced") != std::string::npos)) uvIndex_ = i;
        }
        if (melIndex_ < 0) {
            for (int i = 0; i < (int) inputNames_.size(); ++i) {
                if (toLowerCopy(inputNames_[(size_t) i]) == "c") { melIndex_ = i; break; }
            }
        }
        if (melIndex_ >= 0 && melIndex_ < (int)inputShapes_.size()) {
            const auto& melShape = inputShapes_[(size_t)melIndex_];
            for (auto d : melShape) {
                if (d > 1 && d != 128) {
                    melBinsHint_ = d;
                    break;
                }
            }
            melNeedsTranspose_ = (melShape.size() == 3 && melShape[2] == 128);
        }

        for (size_t i = 0; i < outputCount; ++i)
        {
            const auto shape = session_->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
            juce::String shapeText;
            for (size_t d = 0; d < shape.size(); ++d) {
                if (d > 0) shapeText << "x";
                shapeText << juce::String(shape[d]);
            }
            AppLogger::info("PCNSF output[" + juce::String((int)i) + "] " + juce::String(outputNames_[i]) + " shape=" + shapeText);
        }
    }
}

PCNSFHifiGANVocoder::~PCNSFHifiGANVocoder() = default;

std::vector<float> PCNSFHifiGANVocoder::synthesize(const std::vector<float>& f0, const float* mel)
{
    if (!session_)
        return {};

    const size_t numFrames = f0.size();
    if (numFrames == 0)
        return {};

    try
    {
        thread_local PCNSFScratchBuffers scratch;
        scratch.resetForRun(numFrames, inputNames_.size(), outputNames_.size());

        const int64_t melBinsDefault = 128;

        const int melIndex = melIndex_;
        const int f0Index = f0Index_;
        const int uvIndex = uvIndex_;

        if (melIndex < 0 || f0Index < 0) {
            AppLogger::warn("PCNSFHifiGANVocoder missing required inputs. melIndex=" + juce::String(melIndex) + " f0Index=" + juce::String(f0Index));
        }

        auto resolveShape = [&](const std::vector<int64_t>& rawShape, int64_t numFramesValue, int64_t melBinsValue) -> std::vector<int64_t>
        {
            std::vector<int64_t> out = rawShape;
            for (auto& d : out) {
                if (d <= 0) d = -1;
            }

            for (auto& d : out) {
                if (d == -1) d = numFramesValue;
            }

            for (auto& d : out) {
                if (d == -1) d = 1;
            }

            for (auto& d : out) {
                if (d == 0) d = 1;
            }

            for (auto& d : out) {
                if (d == melBinsDefault) d = melBinsValue;
            }

            return out;
        };

        auto& melOwned = scratch.melOwned;
        if (mel == nullptr)
        {
            melOwned.resize((size_t) melBinsDefault * numFrames, 0.0f);
            mel = melOwned.data();
        }

        auto& uvData = scratch.uvData;
        uvData.resize(numFrames);
        for (size_t i = 0; i < numFrames; ++i)
            uvData[i] = (f0[i] > 0.0f) ? 1.0f : 0.0f;

        auto& melTransposed = scratch.melTransposed;
        const float* melToUse = mel;
        int64_t melBins = (melBinsHint_ > 0) ? melBinsHint_ : melBinsDefault;

        if (melIndex >= 0)
        {
            const auto& raw = inputShapes_[(size_t) melIndex];
            for (auto d : raw) {
                if (d > 1 && d != (int64_t) numFrames) melBins = d;
            }

            if (melNeedsTranspose_) {
                melTransposed.resize((size_t) melBins * numFrames);
                for (size_t t = 0; t < numFrames; ++t) {
                    for (int64_t m = 0; m < melBins; ++m) {
                        melTransposed[t * (size_t) melBins + (size_t) m] = mel[(size_t) m * numFrames + t];
                    }
                }
                melToUse = melTransposed.data();
            }
        }

        auto& inputNamesC = scratch.inputNamesC;
        auto& inputTensors = scratch.inputTensors;
        auto& inputNameStorage = scratch.inputNameStorage;
        auto& extraFloatBuffers = scratch.extraFloatBuffers;
        auto& extraInt64Buffers = scratch.extraInt64Buffers;

        auto addFloatTensor = [&](const std::string& name, const float* data, size_t dataCount, const std::vector<int64_t>& shape) {
            inputNameStorage.push_back(name);
            inputNamesC.push_back(inputNameStorage.back().c_str());
            inputTensors.push_back(Ort::Value::CreateTensor<float>(*memoryInfo_, const_cast<float*>(data), dataCount, shape.data(), shape.size()));
        };

        auto addOwnedFloatTensor = [&](const std::string& name, std::vector<float>&& data, const std::vector<int64_t>& shape) {
            extraFloatBuffers.push_back(std::move(data));
            addFloatTensor(name, extraFloatBuffers.back().data(), extraFloatBuffers.back().size(), shape);
        };

        auto addOwnedInt64Tensor = [&](const std::string& name, std::vector<int64_t>&& data, const std::vector<int64_t>& shape) {
            inputNameStorage.push_back(name);
            inputNamesC.push_back(inputNameStorage.back().c_str());
            extraInt64Buffers.push_back(std::move(data));
            inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(*memoryInfo_, extraInt64Buffers.back().data(), extraInt64Buffers.back().size(), shape.data(), shape.size()));
        };

        if (melIndex >= 0)
        {
            const auto& raw = inputShapes_[(size_t) melIndex];
            auto shape = resolveShape(raw, (int64_t) numFrames, melBins);
            addFloatTensor(inputNames_[(size_t) melIndex], melToUse, (size_t) melBins * numFrames, shape);
        }

        if (f0Index >= 0)
        {
            const auto& raw = inputShapes_[(size_t) f0Index];
            auto shape = resolveShape(raw, (int64_t) numFrames, melBins);
            addFloatTensor(inputNames_[(size_t) f0Index], f0.data(), numFrames, shape);
        }

        if (uvIndex >= 0)
        {
            const auto& raw = inputShapes_[(size_t) uvIndex];
            auto shape = resolveShape(raw, (int64_t) numFrames, melBins);
            addFloatTensor(inputNames_[(size_t) uvIndex], uvData.data(), numFrames, shape);
        }

        for (size_t i = 0; i < inputNames_.size(); ++i)
        {
            if ((int)i == melIndex || (int)i == f0Index || (int)i == uvIndex) continue;

            const auto& rawShape = inputShapes_[i];
            const auto elemType = inputElemTypes_[i];

            auto shape = resolveShape(rawShape, (int64_t) numFrames, melBins);
            size_t count = 1;
            for (auto d : shape) count *= (size_t) std::max<int64_t>(1, d);

            if (elemType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
            {
                std::vector<int64_t> zeros(count, 0);
                addOwnedInt64Tensor(inputNames_[i], std::move(zeros), shape);
            }
            else
            {
                std::vector<float> zeros(count, 0.0f);
                addOwnedFloatTensor(inputNames_[i], std::move(zeros), shape);
            }
        }

        auto& outputNamesC = scratch.outputNamesC;
        if (!outputNames_.empty())
        {
            for (const auto& o : outputNames_)
                outputNamesC.push_back(o.c_str());
        }
        else
        {
            outputNamesC.push_back("audio");
        }

        auto out = session_->Run(
            Ort::RunOptions{ nullptr },
            inputNamesC.data(),
            inputTensors.data(),
            inputTensors.size(),
            outputNamesC.data(),
            outputNamesC.size()
        );

        if (out.empty())
            return {};

        float* audioData = out[0].GetTensorMutableData<float>();
        auto audioShape = out[0].GetTensorTypeAndShapeInfo().GetShape();
        const size_t audioLength = static_cast<size_t>(audioShape.back());
        return std::vector<float>(audioData, audioData + audioLength);
    }
    catch (const Ort::Exception& e)
    {
        AppLogger::error("PCNSFHifiGANVocoder ONNX error: " + juce::String(e.what()));
        return std::vector<float>(numFrames * 512, 0.0f);
    }
    catch (const std::exception& e)
    {
        AppLogger::error("PCNSFHifiGANVocoder error: " + juce::String(e.what()));
        return std::vector<float>(numFrames * 512, 0.0f);
    }
}

} // namespace OpenTune
