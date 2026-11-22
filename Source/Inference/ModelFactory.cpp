#include "ModelFactory.h"
#include "FCPEExtractor.h"
#include "OnnxRuntimeProviderCompat.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/CpuBudgetManager.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"
#include "../Utils/Error.h"
#include <functional>
#include <cstring>
#include <map>
#include <mutex>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
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
    const std::string& modelPath,
    Ort::Env& env,
    std::shared_ptr<ResamplingManager> resampler,
    AccelerationDetector::BackendSelection backend)
{
    if (!juce::File(modelPath).existsAsFile()) {
        return F0ExtractorResult::failure(ErrorCode::ModelNotFound,
            "F0 model file: " + modelPath);
    }

    try {
        bool gpuMode = false;
        auto session = loadF0Session(modelPath, env, type, gpuMode, backend);
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
                    cpuSessionFactory = [modelPath, &env, type]() {
                        bool cpuGpuMode = false;
                        try {
                            auto cpuOptions = createF0SessionOptions(type, cpuGpuMode, {});
#ifdef _WIN32
                            juce::File f(modelPath);
                            std::wstring wPath = f.getFullPathName().toWideCharPointer();
                            return std::make_unique<Ort::Session>(env, wPath.c_str(), cpuOptions);
#else
                            return std::make_unique<Ort::Session>(env, modelPath.c_str(), cpuOptions);
#endif
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

std::optional<std::array<uint8_t, 32>> ModelFactory::getModelWeightDigest(const std::string& modelPath)
{
    struct CachedDigest
    {
        int64_t size{0};
        juce::Time modified;
        std::array<uint8_t, 32> digest{};
    };
    static std::mutex mutex;
    static std::map<std::string, CachedDigest> cachedByPath;

    const juce::File file(modelPath);
    if (!file.existsAsFile())
        return std::nullopt;

    const auto fullPath = file.getFullPathName().toStdString();
    const auto size = file.getSize();
    const auto modified = file.getLastModificationTime();
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto found = cachedByPath.find(fullPath);
        if (found != cachedByPath.end() && found->second.size == size && found->second.modified == modified)
            return found->second.digest;
    }

    auto input = file.createInputStream();
    if (input == nullptr || !input->openedOk())
        return std::nullopt;
    const juce::SHA256 hash(*input);
    const auto rawDigest = hash.getRawData();
    std::array<uint8_t, 32> digest{};
    std::memcpy(digest.data(), rawDigest.getData(), digest.size());

    if (file.getSize() != size || file.getLastModificationTime() != modified)
        return std::nullopt;

    {
        const std::lock_guard<std::mutex> lock(mutex);
        cachedByPath[fullPath] = CachedDigest{size, modified, digest};
    }
    return digest;
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
    const std::string& modelPath,
    Ort::Env& env,
    F0ModelType type,
    bool& outGpuMode,
    AccelerationDetector::BackendSelection backend)
{
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

#ifdef _WIN32
        juce::File modelFile(modelPath);
        std::wstring wModelPath = modelFile.getFullPathName().toWideCharPointer();
        return std::make_unique<Ort::Session>(env, wModelPath.c_str(), sessionOptions);
#else
        return std::make_unique<Ort::Session>(env, modelPath.c_str(), sessionOptions);
#endif

    } catch (const std::exception& e) {
        // 仅在「本来要建 GPU session 且非 forceCpu」时允许一次性 CPU 回退。
        // forceCpu 或纯 CPU 首次建会话失败时直接报错，避免：
        //  - 纯 CPU 路径触发第二次完全相同的建会话调用；
        //  - 回退路径改写 AccelerationDetector 全局 backend 状态。
        if (!outGpuMode) {
            AppLogger::error("[ModelFactory] F0 session creation failed (no CPU fallback): "
                + juce::String(e.what()) + "; model: " + juce::String(modelPath));
            outGpuMode = false;
            return nullptr;
        }
        AppLogger::warn("[ModelFactory] F0 GPU session creation failed: "
            + juce::String(e.what()) + "; retrying CPU once");
        AccelerationDetector::getInstance().overrideBackend(AccelerationDetector::AccelBackend::CPU);
        outGpuMode = false;
        try {
            auto cpuOptions = createF0SessionOptions(type, outGpuMode, {});
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
    } catch (...) {
        if (!outGpuMode) {
            AppLogger::error("[ModelFactory] F0 session creation failed (no CPU fallback): "
                + juce::String(modelPath));
            outGpuMode = false;
            return nullptr;
        }
        AppLogger::warn("[ModelFactory] F0 GPU session creation failed with an unknown exception; retrying CPU once");
        AccelerationDetector::getInstance().overrideBackend(AccelerationDetector::AccelBackend::CPU);
        outGpuMode = false;
        try {
            auto cpuOptions = createF0SessionOptions(type, outGpuMode, {});
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
