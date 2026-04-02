#include "ModelFactory.h"
#include "RMVPEExtractor.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/CpuBudgetManager.h"
#include "../Utils/AppLogger.h"
#include "../Utils/Error.h"
#include <juce_core/juce_core.h>
#include <cstdlib>
#include <iomanip>

namespace OpenTune {

namespace {

bool shouldEnableOrtProfilingInDebug()
{
#if JUCE_DEBUG
    const char* envValue = std::getenv("OPENTUNE_ORT_PROFILE");
    if (envValue == nullptr) {
        return false;
    }

    const juce::String normalized = juce::String(envValue).trim().toLowerCase();
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
// F0 Extractor Creation (CPU-only)
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
        auto session = loadF0Session(modelPath, env);
        if (!session) {
            return F0ExtractorResult::failure(ErrorCode::SessionCreationFailed,
                "Failed to create ONNX session for: " + modelPath);
        }

        AppLogger::info("[ModelFactory] Loaded F0 model (CPU-only): " + juce::String(modelPath));

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
// F0 Session Options (CPU-only)
// ==============================================================================

Ort::SessionOptions ModelFactory::createF0CpuSessionOptions() {
    Ort::SessionOptions sessionOptions;
    
    const auto budget = CpuBudgetManager::buildConfig(false);
    sessionOptions.SetIntraOpNumThreads(budget.onnxIntra);
    sessionOptions.SetInterOpNumThreads(budget.onnxInter);
    sessionOptions.SetExecutionMode(budget.onnxSequential ? ExecutionMode::ORT_SEQUENTIAL : ExecutionMode::ORT_PARALLEL);
    sessionOptions.AddConfigEntry("session.intra_op.allow_spinning", budget.allowSpinning ? "1" : "0");
    sessionOptions.AddConfigEntry("session.inter_op.allow_spinning", budget.allowSpinning ? "1" : "0");
    
    logOnnxSessionCpuConfig(budget);
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    
    AppLogger::info("[ModelFactory] F0 session: CPU-only mode");
    return sessionOptions;
}

// ==============================================================================
// Session Loading
// ==============================================================================

std::unique_ptr<Ort::Session> ModelFactory::loadF0Session(
    const std::string& modelPath,
    Ort::Env& env)
{
    try {
        auto sessionOptions = createF0CpuSessionOptions();

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
        AppLogger::error("[ModelFactory] Failed to load F0 session: " + juce::String(e.what()));
        return nullptr;
    }
}

} // namespace OpenTune
