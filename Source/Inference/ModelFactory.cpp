#include "ModelFactory.h"
#include "FCPEExtractor.h"
#include "OnnxRuntimeProviderCompat.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/CpuBudgetManager.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"
#include "../Utils/Error.h"
#include <functional>
#include "F0InferenceService.h"
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
    const F0ModelVersion& model,
    Ort::Env& env,
    std::shared_ptr<ResamplingManager> resampler,
    AccelerationDetector::BackendSelection backend)
{
    const auto modelPath = model.file.getFullPathName().toStdString();
    if (!model.file.existsAsFile()) {
        return F0ExtractorResult::failure(ErrorCode::ModelNotFound,
            "F0 model file: " + modelPath);
    }

    try {
        // Every session for this extraction, including the CPU retry, uses these same weights.
        juce::MemoryBlock weights;
        if (!model.file.loadFileAsData(weights)) {
            return F0ExtractorResult::failure(ErrorCode::ModelLoadFailed,
                "Could not read F0 model: " + modelPath);
        }
        if (weights.getSize() != static_cast<size_t>(model.bytes)
            || model.file.getSize() != model.bytes || model.file.getLastModificationTime() != model.writeTime) {
            return F0ExtractorResult::failure(ErrorCode::ModelLoadFailed,
                "F0 model version changed: " + modelPath);
        }

        bool gpuMode = false;
        auto session = loadF0Session(model, weights, env, type, gpuMode, backend);
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
            case F0ModelType::FCPE: {
                // One-shot CPU retry support for runtime DirectML Run failure
                // (Windows only). Factory builds a pure-CPU session without
                // recursing into GPU fallback (forceCpu = true), and preserves
                // the existing CPU fallback on initial creation failure.
                std::function<std::unique_ptr<Ort::Session>()> cpuSessionFactory;
#if defined(_WIN32)
                if (gpuMode) {
                    cpuSessionFactory = [weights = std::move(weights), &env, type]() {
                        bool cpuGpuMode = false;
                        try {
                            auto cpuOptions = createF0SessionOptions(type, cpuGpuMode, {});
                            return std::make_unique<Ort::Session>(env, weights.getData(), weights.getSize(), cpuOptions);
                        } catch (const std::exception& e) {
                            AppLogger::error("[ModelFactory] CPU session factory failed: "
                                + juce::String(e.what()));
                            return std::unique_ptr<Ort::Session>(nullptr);
                        } catch (...) {
                            AppLogger::error("[ModelFactory] CPU session factory failed with unknown exception");
                            return std::unique_ptr<Ort::Session>(nullptr);
                        }
                    };
                }
#endif
                return F0ExtractorResult::success(
                    std::make_unique<FCPEExtractor>(std::move(session), resampler,
                                                    std::move(cpuSessionFactory), gpuMode));
            }
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
        case F0ModelType::FCPE:
            return modelDir + "/fcpe.onnx";
    }
    return "";
}

F0ModelVersion ModelFactory::resolveF0ModelVersion(F0ModelType type, const std::string& modelDir)
{
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(getModelPath(type, modelDir));
    return {file, file.getSize(), file.getLastModificationTime()};
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
    AccelerationDetector::BackendSelection backendSelection) {
    Ort::SessionOptions sessionOptions;

    sessionOptions.DisableMemPattern();
    sessionOptions.DisableCpuMemArena();

    bool gpuMode = false;

#if defined(__APPLE__)
    if (backendSelection.backend == AccelerationDetector::AccelBackend::CoreML) {
        try {
        appendCoreMlExecutionProvider(sessionOptions);
        gpuMode = true;
        AppLogger::info("[ModelFactory] F0 session: CoreML EP added ("
            + juce::String(coreMlProviderDescription()) + ", computeUnits=ALL)");
        } catch (const std::exception& e) {
            AppLogger::warn(juce::String("[ModelFactory] Failed to add CoreML EP for F0: ") + e.what());
            AppLogger::info("[ModelFactory] F0 session: falling back to CPU");
            AccelerationDetector::getInstance().overrideBackend(AccelerationDetector::AccelBackend::CPU);
        }
    }
#endif

#if defined(_WIN32)
    if (type == F0ModelType::FCPE
        && backendSelection.backend == AccelerationDetector::AccelBackend::DirectML) {
        const auto& api = Ort::GetApi();
        const OrtDmlApi* dmlApi = nullptr;
        OrtStatus* status = api.GetExecutionProviderApi("DML", ORT_API_VERSION,
                                                        reinterpret_cast<const void**>(&dmlApi));
        if (status == nullptr && dmlApi != nullptr) {
            const int adapterIndex = backendSelection.dmlAdapterIndex;
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
    const F0ModelVersion& model,
    const juce::MemoryBlock& weights,
    Ort::Env& env,
    F0ModelType type,
    bool& outGpuMode,
    AccelerationDetector::BackendSelection backend)
{
    const auto& modelPath = model.file.getFullPathName();
    try {
        auto sessionOptions = createF0SessionOptions(type, outGpuMode, backend);

        if (shouldEnableOrtProfilingInDebug()) {
#ifdef _WIN32
            sessionOptions.EnableProfiling(L"opentune_f0_profile");
#else
            sessionOptions.EnableProfiling("opentune_f0_profile");
#endif
            AppLogger::info("[ModelFactory] ORT profiling enabled for F0");
        }

        return std::make_unique<Ort::Session>(env, weights.getData(), weights.getSize(), sessionOptions);
    } catch (const std::exception& e) {
        AppLogger::error("[ModelFactory] F0 session creation failed: " + juce::String(e.what())
            + "; model: " + modelPath);
    } catch (...) {
        AppLogger::error("[ModelFactory] F0 session creation failed with an unknown exception; model: " + modelPath);
    }

    if (!outGpuMode)
        return nullptr;

    AppLogger::warn("[ModelFactory] F0 GPU session creation failed; retrying CPU once");
    AccelerationDetector::getInstance().overrideBackend(AccelerationDetector::AccelBackend::CPU);
    try {
        auto cpuOptions = createF0SessionOptions(type, outGpuMode, {});
        return std::make_unique<Ort::Session>(env, weights.getData(), weights.getSize(), cpuOptions);
    } catch (...) {
        AppLogger::error("[ModelFactory] Failed to load F0 session (CPU fallback): " + modelPath);
        return nullptr;
    }
}

} // namespace OpenTune
