#include "ModelFactory.h"
#include "RMVPEExtractor.h"
#include "FCPEExtractor.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/CpuBudgetManager.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"
#include "../Utils/Error.h"
#include <juce_core/juce_core.h>
#include <iomanip>
#ifdef _WIN32
#include <dml_provider_factory.h>
#endif

namespace OpenTune {

namespace {

bool shouldEnableOrtProfilingInDebug()
{
#if JUCE_DEBUG
    const auto envValue = juce::SystemStats::getEnvironmentVariable("OPENTUNE_ORT_PROFILE", {});
    if (envValue.isEmpty()) {
        return false;
    }

    const juce::String normalized = envValue.trim().toLowerCase();
    return normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes";
#else
    return false;
#endif
}

void logOnnxSessionCpuConfig(const CpuBudgetManager::BudgetConfig& budget)
{
    AppLogger::info("[ModelFactory] ONNX session CPU config: totalBudget=" + juce::String(budget.totalBudget)
              + " onnxIntra=" + juce::String(budget.onnxIntra)
              + " onnxInter=" + juce::String(budget.onnxInter)
              + " sequential=" + juce::String(budget.onnxSequential ? 1 : 0)
              + " allowSpinning=" + juce::String(budget.allowSpinning ? 1 : 0));
}

}

// ==============================================================================
// F0 Extractor Creation
// ==============================================================================

ModelFactory::F0ExtractorResult ModelFactory::createF0Extractor(
    F0ModelType type,
    const std::string& modelDir,
    Ort::Env& env,
    std::shared_ptr<ResamplingManager> resampler)
{
    std::string modelPath = getModelPath(type, modelDir);

    if (!isModelAvailable(type, modelDir)) {
        return F0ExtractorResult::failure(ErrorCode::ModelNotFound,
            "F0 model file: " + modelPath);
    }

    try {
        bool gpuMode = false;
        auto session = loadF0Session(modelPath, env, type, gpuMode);
        if (!session) {
            return F0ExtractorResult::failure(ErrorCode::SessionCreationFailed,
                "Failed to create ONNX session for: " + modelPath);
        }

        const juce::String backendStr =
#if defined(_WIN32)
            gpuMode ? "DirectML" : "CPU";
#elif defined(__APPLE__)
            gpuMode ? "CoreML" : "CPU";
#else
            "CPU";
#endif
        AppLogger::info("[ModelFactory] Loaded F0 model (" + backendStr + "): " + juce::String(modelPath));

        switch (type) {
            case F0ModelType::RMVPE:
                return F0ExtractorResult::success(
                    std::make_unique<RMVPEExtractor>(std::move(session), resampler));
            case F0ModelType::FCPE:
                return F0ExtractorResult::success(
                    std::make_unique<FCPEExtractor>(std::move(session), resampler));
        }

        return F0ExtractorResult::failure(ErrorCode::InvalidModelType,
            "Unknown F0 model type");

    } catch (...) {
        return F0ExtractorResult::failure(ErrorCode::ModelLoadFailed,
            "Unknown error loading F0 model");
    }
}

// ==============================================================================
// Model Path Resolution
// ==============================================================================

std::string ModelFactory::getModelPath(F0ModelType type, const std::string& modelDir) {
    switch (type) {
        case F0ModelType::RMVPE:
            return modelDir + "/rmvpe.onnx";
        case F0ModelType::FCPE:
            return modelDir + "/fcpe.onnx";
    }
    return "";
}

// ==============================================================================
// Model Availability Checking
// ==============================================================================

bool ModelFactory::isModelAvailable(F0ModelType type, const std::string& modelDir) {
    std::string path = getModelPath(type, modelDir);
    juce::File file(path);
    return file.existsAsFile();
}

// ==============================================================================
// Model Discovery
// ==============================================================================

std::vector<F0ModelInfo> ModelFactory::getAvailableF0Models(const std::string& modelDir) {
    std::vector<F0ModelInfo> models;

    // RMVPE disabled - mark as unavailable
    F0ModelInfo rmvpe;
    rmvpe.type = F0ModelType::RMVPE;
    rmvpe.name = "rmvpe";
    rmvpe.displayName = "RMVPE (Robust)";
    rmvpe.modelSizeBytes = 361 * 1024 * 1024;
    rmvpe.isAvailable = false;  // Disabled - FCPE is the default model
    // models.push_back(rmvpe);  // Don't include in available models list

    F0ModelInfo fcpe;
    fcpe.type = F0ModelType::FCPE;
    fcpe.name = "fcpe";
    fcpe.displayName = "FCPE (Fast)";
    fcpe.modelSizeBytes = 43 * 1024 * 1024;
    fcpe.isAvailable = isModelAvailable(F0ModelType::FCPE, modelDir);
    models.push_back(fcpe);

    return models;
}

// ==============================================================================
// F0 Session Options
// ==============================================================================

Ort::SessionOptions ModelFactory::createF0SessionOptions(
    F0ModelType type,
    bool& outGpuMode,
    bool forceCpu) {
    Ort::SessionOptions sessionOptions;

    sessionOptions.DisableMemPattern();
    sessionOptions.DisableCpuMemArena();

    bool gpuMode = false;

#if defined(__APPLE__)
    if (!forceCpu) {
        try {
        std::unordered_map<std::string, std::string> coremlOptions;
        coremlOptions["ModelFormat"] = "MLProgram";
        coremlOptions["MLComputeUnits"] = "CPUAndGPU";
        sessionOptions.AppendExecutionProvider("CoreML", coremlOptions);
        gpuMode = true;
        AppLogger::info("[ModelFactory] F0 session: CoreML EP added (macOS, MLProgram+CPUAndGPU)");
        } catch (...) {
            AppLogger::warn("[ModelFactory] Failed to add CoreML EP for F0 (unknown error)");
            AppLogger::info("[ModelFactory] F0 session: falling back to CPU");
        }
    }
#endif

#if defined(_WIN32)
    if (!forceCpu
        && type == F0ModelType::FCPE
        && AccelerationDetector::getInstance().getSelectedBackend()
            == AccelerationDetector::AccelBackend::DirectML) {
        const auto& api = Ort::GetApi();
        const OrtDmlApi* dmlApi = nullptr;
        OrtStatus* status = api.GetExecutionProviderApi("DML", ORT_API_VERSION,
                                                        reinterpret_cast<const void**>(&dmlApi));
        if (status == nullptr && dmlApi != nullptr) {
            int adapterIndex = AccelerationDetector::getInstance().getDirectMLDeviceId();
            OrtStatus* dmlStatus = dmlApi->SessionOptionsAppendExecutionProvider_DML(
                sessionOptions, adapterIndex);
            if (dmlStatus == nullptr) {
                gpuMode = true;
                AppLogger::info("[ModelFactory] F0 session: DML EP added (adapter " + juce::String(adapterIndex) + ")");
            } else {
                api.ReleaseStatus(dmlStatus);
                AppLogger::warn("[ModelFactory] DML EP append failed for F0, falling back to CPU");
            }
        } else {
            if (status) {
                AppLogger::warn("[ModelFactory] DML EP API unavailable: "
                    + juce::String(api.GetErrorMessage(status)));
                api.ReleaseStatus(status);
            } else {
                AppLogger::warn("[ModelFactory] DML EP API pointer is null, falling back to CPU");
            }
        }
    }
#endif

    const auto budget = CpuBudgetManager::buildConfig(gpuMode);
    sessionOptions.SetIntraOpNumThreads(budget.onnxIntra);
    sessionOptions.SetInterOpNumThreads(budget.onnxInter);
    sessionOptions.SetExecutionMode(budget.onnxSequential ? ExecutionMode::ORT_SEQUENTIAL : ExecutionMode::ORT_PARALLEL);
    sessionOptions.AddConfigEntry("session.intra_op.allow_spinning", budget.allowSpinning ? "1" : "0");
    sessionOptions.AddConfigEntry("session.inter_op.allow_spinning", budget.allowSpinning ? "1" : "0");

    logOnnxSessionCpuConfig(budget);
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    if (!gpuMode) {
        AppLogger::info("[ModelFactory] F0 session: CPU-only mode");
    }
    outGpuMode = gpuMode;
    return sessionOptions;
}

// ==============================================================================
// Session Loading
// ==============================================================================

std::unique_ptr<Ort::Session> ModelFactory::loadF0Session(
    const std::string& modelPath,
    Ort::Env& env,
    F0ModelType type,
    bool& outGpuMode)
{
    try {
        auto sessionOptions = createF0SessionOptions(type, outGpuMode);

        if (shouldEnableOrtProfilingInDebug()) {
#ifdef _WIN32
            sessionOptions.EnableProfiling(L"opentune_f0_profile");
#else
            sessionOptions.EnableProfiling("opentune_f0_profile");
#endif
            AppLogger::info("[ModelFactory] ORT profiling enabled for F0");
        }

#ifdef _WIN32
        juce::File modelFile(modelPath);
        std::wstring wModelPath = modelFile.getFullPathName().toWideCharPointer();
        return std::make_unique<Ort::Session>(env, wModelPath.c_str(), sessionOptions);
#else
        return std::make_unique<Ort::Session>(env, modelPath.c_str(), sessionOptions);
#endif

    } catch (const Ort::Exception& e) {
        AppLogger::warn("[ModelFactory] F0 session creation failed: " + juce::String(e.what()) + "; retrying CPU");
        outGpuMode = false;
        try {
            auto cpuOptions = createF0SessionOptions(type, outGpuMode, true);
#ifdef _WIN32
            juce::File modelFile(modelPath);
            std::wstring wModelPath = modelFile.getFullPathName().toWideCharPointer();
            return std::make_unique<Ort::Session>(env, wModelPath.c_str(), cpuOptions);
#else
            return std::make_unique<Ort::Session>(env, modelPath.c_str(), cpuOptions);
#endif
        } catch (...) {
            AppLogger::error("[ModelFactory] Failed to load F0 session (CPU fallback): " + juce::String(modelPath));
            return nullptr;
        }
    }
}

} // namespace OpenTune
