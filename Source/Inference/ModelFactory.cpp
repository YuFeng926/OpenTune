#include "ModelFactory.h"
#include "RMVPEExtractor.h"
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
        AccelerationDetector::AccelBackend backend = AccelerationDetector::AccelBackend::CPU;
        auto session = loadF0Session(modelPath, env, backend);
        if (!session) {
            return F0ExtractorResult::failure(ErrorCode::SessionCreationFailed,
                "Failed to create ONNX session for: " + modelPath);
        }

        juce::String backendStr;
        switch (backend) {
            case AccelerationDetector::AccelBackend::DirectML: backendStr = "DirectML"; break;
            case AccelerationDetector::AccelBackend::CoreML:   backendStr = "CoreML"; break;
            default:                                           backendStr = "CPU"; break;
        }
        AppLogger::info("[ModelFactory] Loaded F0 model (" + backendStr + "): " + juce::String(modelPath));

        switch (type) {
            case F0ModelType::RMVPE:
                return F0ExtractorResult::success(
                    std::make_unique<RMVPEExtractor>(std::move(session), resampler));
        }

        return F0ExtractorResult::failure(ErrorCode::InvalidModelType,
            "Unknown F0 model type");

    } catch (const Ort::Exception& e) {
        return F0ExtractorResult::failure(ErrorCode::ModelLoadFailed,
            "ONNX error loading F0 model: " + std::string(e.what()));
    } catch (const std::exception& e) {
        return F0ExtractorResult::failure(ErrorCode::ModelLoadFailed,
            "Error loading F0 model: " + std::string(e.what()));
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

    F0ModelInfo rmvpe;
    rmvpe.type = F0ModelType::RMVPE;
    rmvpe.name = "rmvpe";
    rmvpe.displayName = "RMVPE (Robust)";
    rmvpe.modelSizeBytes = 361 * 1024 * 1024;
    rmvpe.isAvailable = isModelAvailable(F0ModelType::RMVPE, modelDir);
    models.push_back(rmvpe);

    return models;
}

// ==============================================================================
// F0 Session Options
// ==============================================================================

Ort::SessionOptions ModelFactory::createF0SessionOptions(AccelerationDetector::AccelBackend& outBackend) {
    Ort::SessionOptions sessionOptions;

    // F0 inference uses variable input shapes; disable ORT arenas that retain large buffers.
    sessionOptions.DisableMemPattern();
    sessionOptions.DisableCpuMemArena();

    outBackend = AccelerationDetector::AccelBackend::CPU;

#if defined(__APPLE__)
    try {
        std::unordered_map<std::string, std::string> coremlOptions;
        coremlOptions["ModelFormat"] = "MLProgram";
        coremlOptions["MLComputeUnits"] = "CPUAndGPU";
        sessionOptions.AppendExecutionProvider("CoreML", coremlOptions);
        outBackend = AccelerationDetector::AccelBackend::CoreML;
        AppLogger::info("[ModelFactory] F0 session: CoreML EP added (macOS, MLProgram+CPUAndGPU)");
    } catch (const Ort::Exception& e) {
        AppLogger::warn("[ModelFactory] Failed to add CoreML EP for F0: " + juce::String(e.what()));
        AppLogger::info("[ModelFactory] F0 session: falling back to CPU");
    } catch (const std::exception& e) {
        AppLogger::warn("[ModelFactory] Failed to add CoreML EP for F0: " + juce::String(e.what()));
        AppLogger::info("[ModelFactory] F0 session: falling back to CPU");
    } catch (...) {
        AppLogger::warn("[ModelFactory] Failed to add CoreML EP for F0 (unknown error)");
        AppLogger::info("[ModelFactory] F0 session: falling back to CPU");
    }
#elif defined(_WIN32)
    auto& gpu = AccelerationDetector::getInstance();
    if (gpu.getSelectedBackend() == AccelerationDetector::AccelBackend::DirectML) {
        try {
            const OrtDmlApi* dmlApi = nullptr;
            OrtStatus* probeStatus = Ort::GetApi().GetExecutionProviderApi(
                "DML", ORT_API_VERSION,
                reinterpret_cast<const void**>(&dmlApi));
            if (probeStatus != nullptr) {
                Ort::GetApi().ReleaseStatus(probeStatus);
                AppLogger::warn("[ModelFactory] Failed to get DML API for F0");
            } else {
                int adapterIndex = gpu.getDirectMLDeviceId();
                OrtStatus* dmlStatus = dmlApi->SessionOptionsAppendExecutionProvider_DML(
                    sessionOptions, adapterIndex);
                if (dmlStatus != nullptr) {
                    Ort::GetApi().ReleaseStatus(dmlStatus);
                    AppLogger::warn("[ModelFactory] Failed to append DML EP for F0");
                } else {
                    outBackend = AccelerationDetector::AccelBackend::DirectML;
                    AppLogger::info("[ModelFactory] F0 session: DML EP added (Windows, adapterIndex="
                        + juce::String(adapterIndex) + ")");
                }
            }
        } catch (const Ort::Exception& e) {
            AppLogger::warn("[ModelFactory] Failed to add DML EP for F0: " + juce::String(e.what()));
        }
    }
#endif

    const bool isGpuAccelerated = outBackend != AccelerationDetector::AccelBackend::CPU;
    const auto budget = CpuBudgetManager::buildConfig(isGpuAccelerated);
    sessionOptions.SetIntraOpNumThreads(budget.onnxIntra);
    sessionOptions.SetInterOpNumThreads(budget.onnxInter);
    sessionOptions.SetExecutionMode(budget.onnxSequential ? ExecutionMode::ORT_SEQUENTIAL : ExecutionMode::ORT_PARALLEL);
    sessionOptions.AddConfigEntry("session.intra_op.allow_spinning", budget.allowSpinning ? "1" : "0");
    sessionOptions.AddConfigEntry("session.inter_op.allow_spinning", budget.allowSpinning ? "1" : "0");

    logOnnxSessionCpuConfig(budget);
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    if (outBackend == AccelerationDetector::AccelBackend::CPU) {
        AppLogger::info("[ModelFactory] F0 session: CPU-only mode");
    }
    return sessionOptions;
}

// ==============================================================================
// Session Loading
// ==============================================================================

std::unique_ptr<Ort::Session> ModelFactory::loadF0Session(
    const std::string& modelPath,
    Ort::Env& env,
    AccelerationDetector::AccelBackend& outBackend)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            auto sessionOptions = createF0SessionOptions(outBackend);

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
            if (attempt == 0 && outBackend != AccelerationDetector::AccelBackend::CPU) {
                juce::String backendName;
                switch (outBackend) {
                    case AccelerationDetector::AccelBackend::DirectML: backendName = "DirectML"; break;
                    case AccelerationDetector::AccelBackend::CoreML:   backendName = "CoreML"; break;
                    default:                                           backendName = "unknown"; break;
                }
                AppLogger::warn("[ModelFactory] F0 session creation failed on " + backendName
                    + ", retrying on CPU: " + juce::String(e.what()));
                outBackend = AccelerationDetector::AccelBackend::CPU;
                continue;
            }
            AppLogger::error("[ModelFactory] Failed to load F0 session: " + juce::String(e.what()));
            return nullptr;
        }
    }
    return nullptr;
}

} // namespace OpenTune
