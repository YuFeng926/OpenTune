#include "DmlVocoder.h"
#include "../Utils/AppLogger.h"
#include <algorithm>
#include <cctype>

namespace OpenTune {

namespace { // anonymous: diagnostic helpers

struct DmlInitDiagnostic {
    std::string stage;
    uint32_t hresult = 0x80004005u;
    std::string detail;
    std::string remediation;

    std::string toString() const {
        juce::String line;
        line << "stage=" << juce::String(stage)
             << " hr=0x" << juce::String::toHexString(static_cast<juce::int64>(static_cast<uint32_t>(hresult))).paddedLeft('0', 8)
             << " detail=" << juce::String(detail)
             << " remediation=" << juce::String(remediation);
        return line.toStdString();
    }
};

uint32_t tryExtractHRESULT(const std::string& text) {
    auto isHex = [](char c) {
        return (c >= '0' && c <= '9')
            || (c >= 'A' && c <= 'F')
            || (c >= 'a' && c <= 'f');
    };

    for (size_t i = 0; i + 8 <= text.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < 8; ++j) {
            if (!isHex(text[i + j])) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            continue;
        }

        try {
            const unsigned long value = std::stoul(text.substr(i, 8), nullptr, 16);
            return static_cast<uint32_t>(value);
        } catch (...) {
            return 0x80004005u;
        }
    }

    return 0x80004005u;
}

DmlInitDiagnostic createOrtDiagnostic(const char* stage, OrtStatus* status, const OrtApi& api, const char* remediation) {
    DmlInitDiagnostic diag;
    diag.stage = stage;
    diag.remediation = remediation;

    if (status == nullptr) {
        diag.detail = "OrtStatus is null";
        diag.hresult = 0x80004005u;
        return diag;
    }

    // Get error code
    OrtErrorCode code = api.GetErrorCode(status);
    const char* codeName = "UNKNOWN";
    switch (code) {
        case ORT_FAIL: codeName = "ORT_FAIL"; break;
        case ORT_INVALID_ARGUMENT: codeName = "ORT_INVALID_ARGUMENT"; break;
        case ORT_NO_SUCHFILE: codeName = "ORT_NO_SUCHFILE"; break;
        case ORT_NO_MODEL: codeName = "ORT_NO_MODEL"; break;
        case ORT_ENGINE_ERROR: codeName = "ORT_ENGINE_ERROR"; break;
        case ORT_RUNTIME_EXCEPTION: codeName = "ORT_RUNTIME_EXCEPTION"; break;
        case ORT_INVALID_PROTOBUF: codeName = "ORT_INVALID_PROTOBUF"; break;
        case ORT_MODEL_LOADED: codeName = "ORT_MODEL_LOADED"; break;
        case ORT_NOT_IMPLEMENTED: codeName = "ORT_NOT_IMPLEMENTED"; break;
        case ORT_INVALID_GRAPH: codeName = "ORT_INVALID_GRAPH"; break;
        case ORT_EP_FAIL: codeName = "ORT_EP_FAIL"; break;
        default: codeName = "OTHER"; break;
    }

    juce::String detail;
    detail << "OrtErrorCode=" << codeName << " (" << (int)code << ")";

    // Get error message (may be empty, but we try)
    const char* msg = api.GetErrorMessage(status);
    if (msg != nullptr && msg[0] != '\0') {
        detail << " message=\"" << juce::String(msg) << "\"";
    } else {
        detail << " message=<empty>";
    }

    diag.detail = detail.toStdString();
    diag.hresult = tryExtractHRESULT(diag.detail);
    api.ReleaseStatus(status);
    return diag;
}

} // anonymous namespace

static std::string toLowerCopy(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return out;
}

struct DmlScratchBuffers {
    std::vector<float> melOwned;
    std::vector<float> uvData;
    std::vector<float> melTransposed;
    std::vector<const char*> inputNamesC;
    std::vector<Ort::Value> inputTensors;
    std::vector<std::string> inputNameStorage;
    std::vector<std::vector<float>> extraFloatBuffers;
    std::vector<std::vector<int64_t>> extraInt64Buffers;

    size_t recentPeakFrames_ = 0;

    void resetForRun(size_t frameCount, size_t inputCount)
    {
        recentPeakFrames_ = std::max(recentPeakFrames_, frameCount);

        if (uvData.capacity() < frameCount) {
            uvData.reserve(frameCount);
        }
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
};

DmlVocoder::DmlVocoder(const std::string& modelPath, 
                       Ort::Env& env, 
                       const DmlConfig& config)
    : cpuMemoryInfo_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
{
    initializeSession(modelPath, env, config);
    detectInputOutputNames();
}

DmlVocoder::~DmlVocoder() = default;

void DmlVocoder::initializeSession(const std::string& modelPath, 
                                    Ort::Env& env, 
                                    const DmlConfig& config)
{
    Ort::SessionOptions sessionOptions;

    auto& api = Ort::GetApi();
    
    const OrtDmlApi* dmlApi = nullptr;
    {
        OrtStatus* probeStatus = api.GetExecutionProviderApi(
            "DML",
            ORT_API_VERSION,
            reinterpret_cast<const void**>(&dmlApi));
        if (probeStatus != nullptr) {
            DmlInitDiagnostic diag = createOrtDiagnostic(
                "ort_get_dml_api",
                probeStatus,
                api,
                "Ensure onnxruntime.dll is the DirectML-enabled package and was deployed correctly."
            );
            throw std::runtime_error(std::string("DmlVocoder initialization failed: ") + diag.toString());
        }
    }

    if (dmlApi == nullptr) {
        throw std::runtime_error("DmlVocoder: DML API pointer is null after GetExecutionProviderApi");
    }

    OrtStatus* dmlStatus = nullptr;

    OrtDmlDeviceOptions devOpts;
    devOpts.Preference = static_cast<OrtDmlPerformancePreference>(config.performancePreference);
    devOpts.Filter = static_cast<OrtDmlDeviceFilter>(config.deviceFilter);

    AppLogger::info(std::string("[DmlVocoder] Trying DML2 API with device_id=")
        + juce::String(config.deviceId).toStdString()
        + " perfPref=" + juce::String(config.performancePreference).toStdString()
        + " filter=" + juce::String(config.deviceFilter).toStdString());

    dmlStatus = dmlApi->SessionOptionsAppendExecutionProvider_DML2(sessionOptions, &devOpts);
    if (dmlStatus != nullptr) {
        DmlInitDiagnostic diag = createOrtDiagnostic(
            "ort_append_dml2",
            dmlStatus,
            api,
            "Verify DML runtime probe report and ensure deployed DirectML + Agility SDK versions satisfy DML feature level 5_0."
        );
        throw std::runtime_error(std::string("DmlVocoder initialization failed: ") + diag.toString());
    }

    const std::string dmlModeUsed = "DML2";
    AppLogger::info("[DmlVocoder] DML2 API registered successfully");

    sessionOptions.SetIntraOpNumThreads(1);
    sessionOptions.SetInterOpNumThreads(1);
    sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    
    sessionOptions.DisableMemPattern();
    
    sessionOptions.AddConfigEntry("session.intra_op.allow_spinning", "0");
    sessionOptions.AddConfigEntry("session.inter_op.allow_spinning", "0");
    
    sessionOptions.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);

    AppLogger::info("[DmlVocoder] Creating ONNX session with "
        + juce::String(dmlModeUsed)
        + " on device_id=" + juce::String(config.deviceId)
        + " model=" + juce::String(modelPath));

#ifdef _WIN32
    std::wstring wPath(modelPath.begin(), modelPath.end());
    session_ = std::make_unique<Ort::Session>(env, wPath.c_str(), sessionOptions);
#else
    session_ = std::make_unique<Ort::Session>(env, modelPath.c_str(), sessionOptions);
#endif

    AppLogger::info("[DmlVocoder] ONNX session created (DML backend)");
}

void DmlVocoder::detectInputOutputNames()
{
    if (!session_) return;
    
    Ort::AllocatorWithDefaultOptions allocator;

    const size_t inputCount = session_->GetInputCount();
    inputNames_.reserve(inputCount);
    inputShapes_.reserve(inputCount);
    inputElemTypes_.reserve(inputCount);
    
    for (size_t i = 0; i < inputCount; ++i) {
        auto name = session_->GetInputNameAllocated(i, allocator);
        inputNames_.push_back(name.get());

        const auto typeInfo = session_->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo();
        inputShapes_.push_back(typeInfo.GetShape());
        inputElemTypes_.push_back(typeInfo.GetElementType());
    }

    const size_t outputCount = session_->GetOutputCount();
    outputNames_.reserve(outputCount);
    for (size_t i = 0; i < outputCount; ++i) {
        auto name = session_->GetOutputNameAllocated(i, allocator);
        outputNames_.push_back(name.get());
    }

    for (int i = 0; i < static_cast<int>(inputNames_.size()); ++i) {
        const auto lowered = toLowerCopy(inputNames_[i]);
        if (melIndex_ < 0 && lowered.find("mel") != std::string::npos) melIndex_ = i;
        if (f0Index_ < 0 && (lowered == "f0" || lowered.find("f0") != std::string::npos || lowered.find("pitch") != std::string::npos)) f0Index_ = i;
        if (uvIndex_ < 0 && (lowered.find("uv") != std::string::npos || lowered.find("voiced") != std::string::npos)) uvIndex_ = i;
    }
    
    if (melIndex_ < 0) {
        for (int i = 0; i < static_cast<int>(inputNames_.size()); ++i) {
            if (toLowerCopy(inputNames_[i]) == "c") { melIndex_ = i; break; }
        }
    }
    
    if (melIndex_ >= 0 && melIndex_ < static_cast<int>(inputShapes_.size())) {
        const auto& melShape = inputShapes_[melIndex_];
        for (auto d : melShape) {
            if (d > 1 && d != 128) {
                melBinsHint_ = d;
                break;
            }
        }
        melNeedsTranspose_ = (melShape.size() == 3 && melShape[2] == 128);
    }
}

void DmlVocoder::initializeIOBinding()
{
    if (!session_ || ioBindingInitialized_) return;
    
    ioBinding_ = std::make_unique<Ort::IoBinding>(*session_);

    ioBindingInitialized_ = true;
    AppLogger::info("[DmlVocoder] IOBinding initialized");
}

std::vector<float> DmlVocoder::synthesizeWithIOBinding(
    const std::vector<float>& f0,
    const float* mel,
    size_t melSize)
{
    std::lock_guard<std::mutex> lock(runMutex_);

    if (!session_) {
        throw std::runtime_error("DmlVocoder: session not initialized");
    }

    if (!ioBindingInitialized_) {
        initializeIOBinding();
    }

    const size_t numFrames = f0.size();
    if (numFrames == 0) {
        throw std::runtime_error("DmlVocoder: f0 is empty");
    }

    thread_local DmlScratchBuffers scratch;
    scratch.resetForRun(numFrames, inputNames_.size());

    const int64_t melBinsDefault = 128;
    const int melIndex = melIndex_;
    const int f0Index = f0Index_;
    const int uvIndex = uvIndex_;

    auto resolveShape = [this, numFrames, melBinsDefault](const std::vector<int64_t>& rawShape, int64_t melBinsValue) -> std::vector<int64_t> {
        std::vector<int64_t> out = rawShape;
        for (auto& d : out) {
            if (d <= 0) {
                d = static_cast<int64_t>(numFrames);
            } else if (d == melBinsDefault) {
                d = melBinsValue;
            }
        }
        return out;
    };

    auto& melOwned = scratch.melOwned;
    int64_t melBins = (melBinsHint_ > 0) ? melBinsHint_ : melBinsDefault;
    
    if (melIndex >= 0) {
        const auto& raw = inputShapes_[melIndex];
        for (auto d : raw) {
            if (d > 1 && d != static_cast<int64_t>(numFrames)) melBins = d;
        }
    }

    const size_t expectedMelSize = static_cast<size_t>(melBins) * numFrames;
    
    if (mel == nullptr) {
        melOwned.resize(expectedMelSize, 0.0f);
        mel = melOwned.data();
        melSize = expectedMelSize;
    }
    else if (melSize != expectedMelSize) {
        throw std::runtime_error("DmlVocoder: mel size mismatch. Expected " 
            + std::to_string(expectedMelSize) + " (" + std::to_string(melBins) + " bins x " 
            + std::to_string(numFrames) + " frames), got " + std::to_string(melSize));
    }

    auto& uvData = scratch.uvData;
    uvData.resize(numFrames);
    for (size_t i = 0; i < numFrames; ++i) {
        uvData[i] = (f0[i] > 0.0f) ? 1.0f : 0.0f;
    }

    auto& melTransposed = scratch.melTransposed;
    const float* melToUse = mel;

    if (melIndex >= 0 && melNeedsTranspose_) {
        melTransposed.resize(static_cast<size_t>(melBins) * numFrames);
        for (size_t t = 0; t < numFrames; ++t) {
            for (int64_t m = 0; m < melBins; ++m) {
                melTransposed[t * static_cast<size_t>(melBins) + static_cast<size_t>(m)] = 
                    mel[static_cast<size_t>(m) * numFrames + t];
            }
        }
        melToUse = melTransposed.data();
    }

    auto& inputNamesC = scratch.inputNamesC;
    auto& inputTensors = scratch.inputTensors;
    auto& inputNameStorage = scratch.inputNameStorage;
    auto& extraFloatBuffers = scratch.extraFloatBuffers;
    auto& extraInt64Buffers = scratch.extraInt64Buffers;

    auto addFloatTensor = [&](const std::string& name, const float* data, size_t dataCount, const std::vector<int64_t>& shape) {
        inputNameStorage.push_back(name);
        inputNamesC.push_back(inputNameStorage.back().c_str());
        inputTensors.push_back(Ort::Value::CreateTensor<float>(cpuMemoryInfo_, const_cast<float*>(data), dataCount, shape.data(), shape.size()));
    };

    auto addOwnedFloatTensor = [&](const std::string& name, std::vector<float>&& data, const std::vector<int64_t>& shape) {
        extraFloatBuffers.push_back(std::move(data));
        addFloatTensor(name, extraFloatBuffers.back().data(), extraFloatBuffers.back().size(), shape);
    };

    auto addOwnedInt64Tensor = [&](const std::string& name, std::vector<int64_t>&& data, const std::vector<int64_t>& shape) {
        inputNameStorage.push_back(name);
        inputNamesC.push_back(inputNameStorage.back().c_str());
        extraInt64Buffers.push_back(std::move(data));
        inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(cpuMemoryInfo_, extraInt64Buffers.back().data(), extraInt64Buffers.back().size(), shape.data(), shape.size()));
    };

    if (melIndex >= 0) {
        const auto& raw = inputShapes_[melIndex];
        auto shape = resolveShape(raw, melBins);
        addFloatTensor(inputNames_[melIndex], melToUse, static_cast<size_t>(melBins) * numFrames, shape);
    }

    if (f0Index >= 0) {
        const auto& raw = inputShapes_[f0Index];
        auto shape = resolveShape(raw, melBins);
        addFloatTensor(inputNames_[f0Index], f0.data(), numFrames, shape);
    }

    if (uvIndex >= 0) {
        const auto& raw = inputShapes_[uvIndex];
        auto shape = resolveShape(raw, melBins);
        addFloatTensor(inputNames_[uvIndex], uvData.data(), numFrames, shape);
    }

    for (size_t i = 0; i < inputNames_.size(); ++i) {
        if (static_cast<int>(i) == melIndex || static_cast<int>(i) == f0Index || static_cast<int>(i) == uvIndex) continue;

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

    ioBinding_->ClearBoundInputs();
    ioBinding_->ClearBoundOutputs();

    for (size_t i = 0; i < inputTensors.size(); ++i) {
        ioBinding_->BindInput(inputNamesC[i], inputTensors[i]);
    }

    const std::string outputName = outputNames_.empty() ? "audio" : outputNames_[0];
    
    const size_t expectedAudioLength = numFrames * 512;
    
    bool needReallocate = !preallocatedOutput_ || preallocatedFrames_ != numFrames;
    
    if (needReallocate) {
        outputBuffer_.assign(expectedAudioLength, 0.0f);
        std::vector<int64_t> outputShape = {1, static_cast<int64_t>(expectedAudioLength)};
        
        preallocatedOutput_ = std::make_unique<Ort::Value>(
            Ort::Value::CreateTensor<float>(
                cpuMemoryInfo_,
                outputBuffer_.data(),
                outputBuffer_.size(),
                outputShape.data(),
                outputShape.size()
            )
        );
        preallocatedFrames_ = numFrames;
        
        AppLogger::info("[DmlVocoder] Preallocated CPU output tensor: "
            + juce::String(expectedAudioLength) + " samples");
    }
    
    ioBinding_->BindOutput(outputName.c_str(), *preallocatedOutput_);

    session_->Run(Ort::RunOptions{nullptr}, *ioBinding_);
    ioBinding_->SynchronizeOutputs();

    return outputBuffer_;
}

std::vector<float> DmlVocoder::synthesize(
    const std::vector<float>& f0,
    const float* mel,
    size_t melSize)
{
    return synthesizeWithIOBinding(f0, mel, melSize);
}

VocoderInfo DmlVocoder::getInfo() const
{
    VocoderInfo info;
    info.name = "PC-NSF-HifiGAN (DML)";
    info.backend = "DirectML";
    info.hopSize = 512;
    info.sampleRate = 44100;
    info.melBins = static_cast<int>(melBinsHint_);
    return info;
}

size_t DmlVocoder::estimateMemoryUsage(size_t frames) const
{
    const size_t melSize = frames * static_cast<size_t>(melBinsHint_) * sizeof(float);
    const size_t f0Size = frames * sizeof(float);
    const size_t uvSize = frames * sizeof(float);
    const size_t audioSize = frames * 512 * sizeof(float);
    return melSize + f0Size + uvSize + audioSize;
}

} // namespace OpenTune
