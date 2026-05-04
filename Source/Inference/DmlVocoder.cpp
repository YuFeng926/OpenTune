#ifdef _WIN32

#include "DmlVocoder.h"
#include "../Utils/AppLogger.h"

namespace OpenTune {

namespace {

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
            if (!isHex(text[i + j])) { ok = false; break; }
        }
        if (!ok) continue;

        try {
            return static_cast<uint32_t>(std::stoul(text.substr(i, 8), nullptr, 16));
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
        return diag;
    }

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
    detail << "OrtErrorCode=" << codeName << " (" << static_cast<int>(code) << ")";

    const char* msg = api.GetErrorMessage(status);
    if (msg != nullptr && msg[0] != '\0')
        detail << " message=\"" << juce::String(msg) << "\"";
    else
        detail << " message=<empty>";

    diag.detail = detail.toStdString();
    diag.hresult = tryExtractHRESULT(diag.detail);
    api.ReleaseStatus(status);
    return diag;
}

} // anonymous namespace

DmlVocoder::DmlVocoder(const std::string& modelPath,
                       Ort::Env& env,
                       const DmlConfig& config)
    : cpuMemoryInfo_(Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault))
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
            "DML", ORT_API_VERSION,
            reinterpret_cast<const void**>(&dmlApi));
        if (probeStatus != nullptr) {
            DmlInitDiagnostic diag = createOrtDiagnostic(
                "ort_get_dml_api", probeStatus, api,
                "Ensure onnxruntime.dll is the DirectML-enabled package.");
            throw std::runtime_error(std::string("DmlVocoder initialization failed: ") + diag.toString());
        }
    }

    if (dmlApi == nullptr)
        throw std::runtime_error("DmlVocoder: DML API pointer is null after GetExecutionProviderApi");

    AppLogger::info("[DmlVocoder] Appending DML EP with adapterIndex="
        + juce::String(config.adapterIndex).toStdString());

    OrtStatus* dmlStatus = dmlApi->SessionOptionsAppendExecutionProvider_DML(
        sessionOptions, config.adapterIndex);
    if (dmlStatus != nullptr) {
        DmlInitDiagnostic diag = createOrtDiagnostic(
            "ort_append_dml", dmlStatus, api,
            "DML EP append failed for the selected GPU adapter.");
        throw std::runtime_error(std::string("DmlVocoder initialization failed: ") + diag.toString());
    }

    sessionOptions.SetIntraOpNumThreads(1);
    sessionOptions.SetInterOpNumThreads(1);
    sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    sessionOptions.DisableMemPattern();
    sessionOptions.DisableCpuMemArena();
    sessionOptions.AddConfigEntry("session.intra_op.allow_spinning", "0");
    sessionOptions.AddConfigEntry("session.inter_op.allow_spinning", "0");
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef _WIN32
    juce::String jucePath(modelPath);
    auto wPath = jucePath.toWideCharPointer();
    session_ = std::make_unique<Ort::Session>(env, wPath, sessionOptions);
#else
    session_ = std::make_unique<Ort::Session>(env, modelPath.c_str(), sessionOptions);
#endif

    AppLogger::info("[DmlVocoder] ONNX session created (DML backend)");
}

void DmlVocoder::initializeIOBinding() {
    if (!session_ || ioBindingInitialized_) return;
    ioBinding_ = std::make_unique<Ort::IoBinding>(*session_);
    ioBindingInitialized_ = true;
}

std::vector<float> DmlVocoder::runSession(
    VocoderScratchBuffers& scratch,
    size_t numFrames)
{
    if (!ioBindingInitialized_)
        initializeIOBinding();

    ioBinding_->ClearBoundInputs();
    ioBinding_->ClearBoundOutputs();

    for (size_t i = 0; i < scratch.inputTensors.size(); ++i)
        ioBinding_->BindInput(scratch.inputNamesC[i], scratch.inputTensors[i]);

    const std::string outputName = outputNames_.empty() ? "audio" : outputNames_[0];
    const size_t expectedAudioLength = numFrames * 512;

    if (!preallocatedOutput_ || preallocatedFrames_ != numFrames) {
        outputBuffer_.assign(expectedAudioLength, 0.0f);
        std::vector<int64_t> outputShape = {1, static_cast<int64_t>(expectedAudioLength)};

        preallocatedOutput_ = std::make_unique<Ort::Value>(
            Ort::Value::CreateTensor<float>(
                cpuMemoryInfo_,
                outputBuffer_.data(),
                outputBuffer_.size(),
                outputShape.data(),
                outputShape.size()));
        preallocatedFrames_ = numFrames;
    }

    ioBinding_->BindOutput(outputName.c_str(), *preallocatedOutput_);

    session_->Run(Ort::RunOptions{nullptr}, *ioBinding_);
    ioBinding_->SynchronizeOutputs();

    return outputBuffer_;
}

} // namespace OpenTune

#endif // _WIN32
