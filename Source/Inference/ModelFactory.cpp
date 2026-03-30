#include "ModelFactory.h"
#include "RMVPEExtractor.h"
#include "HifiGANVocoder.h"
#include "PCNSFHifiGANVocoder.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/CpuBudgetManager.h"
#include "../Utils/GpuDetector.h"
#include "../Utils/AppLogger.h"
#include <juce_core/juce_core.h>
#include <cstdlib>
#include <iomanip>
#include <thread>
#include <unordered_map>

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
// F0 Extractor Creation
// ==============================================================================

std::unique_ptr<IF0Extractor> ModelFactory::createF0Extractor(
    F0ModelType type,
    const std::string& modelDir,
    Ort::Env& env,
    std::shared_ptr<ResamplingManager> resampler,
    ModelFactory::InferenceProvider provider)
{
    std::string modelPath = getModelPath(type, modelDir);

    if (!isModelAvailable(type, modelDir)) {
        AppLogger::error("[ModelFactory] F0 model not found: " + juce::String(modelPath));
        return nullptr;
    }

    try {
        auto session = loadONNXSession(modelPath, env, provider);
        if (!session) {
            return nullptr;
        }

        AppLogger::info("[ModelFactory] Loaded F0 model: " + juce::String(modelPath));

        switch (type) {
            case F0ModelType::RMVPE:
                return std::make_unique<RMVPEExtractor>(std::move(session), resampler);
        }

    } catch (const Ort::Exception& e) {
        AppLogger::error("[ModelFactory] ONNX error loading F0 model: " + juce::String(e.what()));
        return nullptr;
    } catch (const std::exception& e) {
        AppLogger::error("[ModelFactory] Error loading F0 model: " + juce::String(e.what()));
        return nullptr;
    }

    return nullptr;
}

// ==============================================================================
// Vocoder Creation
// ==============================================================================

std::unique_ptr<IVocoder> ModelFactory::createVocoder(
    VocoderType type,
    const std::string& modelDir,
    Ort::Env& env)
{
    std::string modelPath = getModelPath(type, modelDir);

    if (!isModelAvailable(type, modelDir)) {
        AppLogger::error("[ModelFactory] Vocoder model not found: " + juce::String(modelPath));
        return nullptr;
    }

    try {
        auto session = loadONNXSession(modelPath, env, ModelFactory::InferenceProvider::Auto);
        if (!session) {
            return nullptr;
        }

        AppLogger::info("[ModelFactory] Loaded vocoder: " + juce::String(modelPath));

        switch (type) {
            case VocoderType::HifiGAN:
                return std::make_unique<HifiGANVocoder>(std::move(session));
            case VocoderType::PC_NSF_HifiGAN:
                return std::make_unique<PCNSFHifiGANVocoder>(std::move(session));
            case VocoderType::FishGan:
                return nullptr;
        }

    } catch (const Ort::Exception& e) {
        AppLogger::error("[ModelFactory] ONNX error loading vocoder: " + juce::String(e.what()));
        return nullptr;
    } catch (const std::exception& e) {
        AppLogger::error("[ModelFactory] Error loading vocoder: " + juce::String(e.what()));
        return nullptr;
    }

    return nullptr;
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

std::string ModelFactory::getModelPath(VocoderType type, const std::string& modelDir) {
    switch (type) {
        case VocoderType::HifiGAN:
            return modelDir + "/hifigan.onnx";
        case VocoderType::PC_NSF_HifiGAN:
            return modelDir + "/hifigan.onnx";
        case VocoderType::FishGan:
            return modelDir + "/fishgan.onnx";
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

bool ModelFactory::isModelAvailable(VocoderType type, const std::string& modelDir) {
    std::string path = getModelPath(type, modelDir);
    juce::File file(path);
    return file.existsAsFile();
}

// ==============================================================================
// Model Discovery
// ==============================================================================

std::vector<F0ModelInfo> ModelFactory::getAvailableF0Models(const std::string& modelDir) {
    std::vector<F0ModelInfo> models;

    // Check RMVPE
    F0ModelInfo rmvpe;
    rmvpe.type = F0ModelType::RMVPE;
    rmvpe.name = "rmvpe";
    rmvpe.displayName = "RMVPE (Robust)";
    rmvpe.modelSizeBytes = 361 * 1024 * 1024;
    rmvpe.isAvailable = isModelAvailable(F0ModelType::RMVPE, modelDir);
    models.push_back(rmvpe);

    return models;
}

std::vector<VocoderInfo> ModelFactory::getAvailableVocoders(const std::string& modelDir) {
    std::vector<VocoderInfo> vocoders;

    // Check HifiGAN
    VocoderInfo hifigan;
    hifigan.type = VocoderType::HifiGAN;
    hifigan.name = "hifigan";
    hifigan.displayName = "HifiGAN Neural Synthesis";
    hifigan.modelSizeBytes = 14 * 1024 * 1024;
    hifigan.isAvailable = isModelAvailable(VocoderType::HifiGAN, modelDir);
    vocoders.push_back(hifigan);

    VocoderInfo pcNsf;
    pcNsf.type = VocoderType::PC_NSF_HifiGAN;
    pcNsf.name = "pc_nsf_hifigan";
    pcNsf.displayName = "PC-NSF-HifiGAN (Pitch Controllable)";
    pcNsf.modelSizeBytes = 50 * 1024 * 1024;
    pcNsf.isAvailable = isModelAvailable(VocoderType::PC_NSF_HifiGAN, modelDir);
    vocoders.push_back(pcNsf);

    VocoderInfo fishGan;
    fishGan.type = VocoderType::FishGan;
    fishGan.name = "fishgan";
    fishGan.displayName = "FishGan";
    fishGan.modelSizeBytes = 0;
    fishGan.isAvailable = isModelAvailable(VocoderType::FishGan, modelDir);
    vocoders.push_back(fishGan);

    return vocoders;
}

void ModelFactory::configureCpuBudgetHints(bool gpuMode)
{
    getGpuModeHint().store(gpuMode, std::memory_order_relaxed);
}

void ModelFactory::resetState()
{
    getGpuModeHint().store(false, std::memory_order_relaxed);
}

// ==============================================================================
// Private Helper Methods
// ==============================================================================

Ort::SessionOptions ModelFactory::createSessionOptions(ModelFactory::InferenceProvider provider) {
    Ort::SessionOptions sessionOptions;

    auto& gpu = GpuDetector::getInstance();

    if (provider == ModelFactory::InferenceProvider::CPUOnly) {
        AppLogger::info("[ModelFactory] Using CPU execution (forced)");
    }

    bool dmlSuccessfullyAdded = false;
    if (provider == ModelFactory::InferenceProvider::Auto) {
        switch (gpu.getSelectedBackend()) {
            case GpuDetector::GpuBackend::DirectML: {
                try {
                    std::unordered_map<std::string, std::string> dmlOptions;
                    dmlOptions["device_id"] = std::to_string(gpu.getDirectMLDeviceId());
                    
                    size_t gpuMemLimit = gpu.getRecommendedGpuMemoryLimit();
                    if (gpuMemLimit > 0) {
                        dmlOptions["gpu_mem_limit"] = std::to_string(gpuMemLimit);
                        float limitGB = static_cast<float>(gpuMemLimit) / (1024.0f * 1024.0f * 1024.0f);
                        AppLogger::info("[ModelFactory] GPU memory limit: " 
                                  + juce::String(limitGB, 2) + " GB (60% of VRAM)");
                    }
                    
                    sessionOptions.AppendExecutionProvider("DML", dmlOptions);
                    dmlSuccessfullyAdded = true;
                    sessionOptions.DisableMemPattern();  // DML requires memory pattern disabled

                    AppLogger::info("[ModelFactory] Using DirectML Execution Provider");
                    AppLogger::info("[ModelFactory] GPU: " + juce::String(gpu.getSelectedGpu().name));
                } catch (const Ort::Exception& e) {
                    dmlSuccessfullyAdded = false;
                    // Enhanced diagnostics when e.what() is empty
                    juce::String errorMsg = e.what();
                    if (errorMsg.isEmpty()) {
                        if (!gpu.isDmlProviderDllAvailable()) {
                            errorMsg = "DML provider DLL not found";
                        } else if (gpu.getSelectedGpu().name.empty()) {
                            errorMsg = "No GPU device available";
                        } else {
                            errorMsg = "DML initialization failed (check GPU driver compatibility)";
                        }
                    }
                    AppLogger::error("[ModelFactory] Failed to add DirectML EP: " + errorMsg);
                    AppLogger::info("[ModelFactory] Falling back to CPU execution");
                }
                break;
            }
            case GpuDetector::GpuBackend::CPU:
            default:
                AppLogger::info("[ModelFactory] Using CPU execution");
                break;
        }
    }

    const bool gpuMode = provider == ModelFactory::InferenceProvider::Auto
        && (dmlSuccessfullyAdded || getGpuModeHint().load(std::memory_order_relaxed));
    const auto budget = CpuBudgetManager::buildConfig(gpuMode);
    sessionOptions.SetIntraOpNumThreads(budget.onnxIntra);
    sessionOptions.SetInterOpNumThreads(budget.onnxInter);
    sessionOptions.SetExecutionMode(budget.onnxSequential ? ExecutionMode::ORT_SEQUENTIAL : ExecutionMode::ORT_PARALLEL);

    try {
        sessionOptions.AddConfigEntry("session.intra_op.allow_spinning", budget.allowSpinning ? "1" : "0");
        sessionOptions.AddConfigEntry("session.inter_op.allow_spinning", budget.allowSpinning ? "1" : "0");
    } catch (const Ort::Exception& e) {
        AppLogger::warn("[ModelFactory] Failed to set spinning config: " + juce::String(e.what()));
    } catch (...) {
        AppLogger::warn("[ModelFactory] Failed to set spinning config (unknown error)");
    }

    logOnnxSessionCpuConfig(budget);
    
    try {
        sessionOptions.AddConfigEntry("session.use_arena", "1");
    } catch (const Ort::Exception& e) {
        AppLogger::warn("[ModelFactory] Failed to set arena config: " + juce::String(e.what()));
    } catch (...) {
        AppLogger::warn("[ModelFactory] Failed to set arena config (unknown error)");
    }
    
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    
    return sessionOptions;
}

std::unique_ptr<Ort::Session> ModelFactory::loadONNXSession(
    const std::string& modelPath,
    Ort::Env& env,
    ModelFactory::InferenceProvider provider)
{
    try {
        auto sessionOptions = createSessionOptions(provider);

        if (shouldEnableOrtProfilingInDebug()) {
#ifdef _WIN32
            sessionOptions.EnableProfiling(L"opentune_ort_profile");
#else
            sessionOptions.EnableProfiling("opentune_ort_profile");
#endif
            AppLogger::info("[ModelFactory] ORT profiling enabled via OPENTUNE_ORT_PROFILE");
        }

        #ifdef _WIN32
        // Windows requires wide string path
        juce::File modelFile(modelPath);
        std::wstring wModelPath = modelFile.getFullPathName().toWideCharPointer();
        auto session = std::make_unique<Ort::Session>(env, wModelPath.c_str(), sessionOptions);
        #else
        auto session = std::make_unique<Ort::Session>(env, modelPath.c_str(), sessionOptions);
        #endif

        // 记录 Execution Provider 相关信息（不做强验证）
        logExecutionProviderInfo(session.get(), provider);

        return session;

    } catch (const Ort::Exception& e) {
        AppLogger::error("[ModelFactory] Failed to load ONNX session: " + juce::String(e.what()));
        return nullptr;
    }
}

void ModelFactory::logExecutionProviderInfo(Ort::Session* session, ModelFactory::InferenceProvider provider) {
    if (!session) return;
    
    try {
        auto& gpu = GpuDetector::getInstance();

        if (provider == ModelFactory::InferenceProvider::CPUOnly) {
            AppLogger::info("[ModelFactory] Session created successfully with backend: CPU (forced)");
            return;
        }
        
        if (gpu.getSelectedBackend() == GpuDetector::GpuBackend::DirectML) {
            if (!gpu.isDmlProviderDllAvailable()) {
                AppLogger::warn("[ModelFactory] DML Provider DLL not found, session may fall back to CPU!");
            } else {
                AppLogger::info("[ModelFactory] DML Provider DLL available: " + juce::String(gpu.getDmlProviderDllPath()));
            }
            
            try {
                Ort::AllocatorWithDefaultOptions allocator;
                size_t inputCount = session->GetInputCount();
                size_t outputCount = session->GetOutputCount();
                
                AppLogger::info("[ModelFactory] Model loaded: " + juce::String((int)inputCount) + " inputs, " 
                          + juce::String((int)outputCount) + " outputs");
                
                if (inputCount > 0) {
                    auto inputName = session->GetInputNameAllocated(0, allocator);
                    auto inputTypeInfo = session->GetInputTypeInfo(0);
                    auto inputShape = inputTypeInfo.GetTensorTypeAndShapeInfo().GetShape();
                    
                    juce::String shapeText;
                    for (size_t i = 0; i < inputShape.size(); ++i) {
                        if (i > 0) shapeText << ", ";
                        shapeText << juce::String(inputShape[i]);
                    }
                    AppLogger::info("[ModelFactory] Input[0]: " + juce::String(inputName.get()) + " shape: [" + shapeText + "]");
                }
            } catch (const Ort::Exception& e) {
                AppLogger::error("[ModelFactory] Could not get model info: " + juce::String(e.what()));
            }
        }
        
        AppLogger::info("[ModelFactory] Session created successfully with backend: " + juce::String(gpu.getBackendName()));
        
    } catch (const Ort::Exception& e) {
        AppLogger::error("[ModelFactory] Error while logging execution provider info: " + juce::String(e.what()));
    } catch (...) {
        AppLogger::error("[ModelFactory] Unknown error while logging execution provider info");
    }
}

} // namespace OpenTune
