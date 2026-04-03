#include "VocoderFactory.h"
#include "PCNSFHifiGANVocoder.h"
#ifdef _WIN32
#include "DmlVocoder.h"
#endif
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"
#include <unordered_map>

namespace OpenTune {

VocoderCreationResult VocoderFactory::create(
    const std::string& modelPath,
    Ort::Env& env)
{
#ifdef _WIN32
    auto& gpu = AccelerationDetector::getInstance();

    if (gpu.getSelectedBackend() == AccelerationDetector::AccelBackend::DirectML) {
        const auto& gpuInfo = gpu.getSelectedGpu();
        const int deviceId = gpu.getDirectMLDeviceId();

        AppLogger::info("[VocoderFactory] DML backend selected by GPU detector");
        AppLogger::info("[VocoderFactory]   GPU: " + juce::String(gpuInfo.name));
        AppLogger::info("[VocoderFactory]   adapterIndex=" + juce::String(static_cast<int>(gpuInfo.adapterIndex)));

        AppLogger::info("[VocoderFactory] Creating DML vocoder...");

        DmlConfig config;
        config.deviceId = deviceId;
        config.performancePreference = 1; // HighPerformance

        try {
            auto vocoder = std::make_unique<DmlVocoder>(modelPath, env, config);

            AppLogger::info("[VocoderFactory] DML vocoder created successfully");

            return VocoderCreationResult::success(std::move(vocoder), VocoderBackend::DML);

        } catch (const std::exception& e) {
            AppLogger::error("[VocoderFactory] DML vocoder creation FAILED: "
                + juce::String(e.what()));
            AppLogger::warn("[VocoderFactory] GPU DML initialization failed, switching to CPU vocoder");
        }
    }
#endif
    
    try {
        Ort::SessionOptions sessionOptions;
        
        sessionOptions.DisableMemPattern();
        sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        VocoderBackend selectedBackend = VocoderBackend::CPU;

#if defined(__APPLE__)
        // macOS: attempt CoreML acceleration for vocoder via Neural Engine
        try {
            std::unordered_map<std::string, std::string> coremlOptions;
            coremlOptions["ModelFormat"] = "MLProgram";
            coremlOptions["MLComputeUnits"] = "CPUAndGPU";
            sessionOptions.AppendExecutionProvider("CoreML", coremlOptions);
            selectedBackend = VocoderBackend::CoreML;
            AppLogger::info("[VocoderFactory] Vocoder session: CoreML EP added (macOS)");
        } catch (const Ort::Exception& e) {
            AppLogger::warn("[VocoderFactory] Failed to add CoreML EP for vocoder: " + juce::String(e.what()));
            AppLogger::info("[VocoderFactory] Vocoder session: falling back to CPU");
        } catch (...) {
            AppLogger::warn("[VocoderFactory] Failed to add CoreML EP for vocoder (unknown error)");
            AppLogger::info("[VocoderFactory] Vocoder session: falling back to CPU");
        }
#endif

        if (selectedBackend == VocoderBackend::CPU) {
            AppLogger::info("[VocoderFactory] Creating CPU vocoder...");
        } else {
            AppLogger::info("[VocoderFactory] Creating CoreML vocoder...");
        }

#ifdef _WIN32
        juce::String jucePath(modelPath);
        auto wPath = jucePath.toWideCharPointer();
        auto session = std::make_unique<Ort::Session>(env, wPath, sessionOptions);
#else
        auto session = std::make_unique<Ort::Session>(env, modelPath.c_str(), sessionOptions);
#endif
        
        auto vocoder = std::make_unique<PCNSFHifiGANVocoder>(std::move(session));
        
        const juce::String backendStr = (selectedBackend == VocoderBackend::CoreML) ? "CoreML" : "CPU";
        AppLogger::info("[VocoderFactory] " + backendStr + " vocoder created successfully");
        
        return VocoderCreationResult::success(std::move(vocoder), selectedBackend);
        
    } catch (const std::exception& e) {
        return VocoderCreationResult::failure(VocoderBackend::CPU, 
            "Vocoder creation failed: " + std::string(e.what()));
    }
}

} // namespace OpenTune
