#include "VocoderFactory.h"
#include "PCNSFHifiGANVocoder.h"
#include "DmlVocoder.h"
#include "../Utils/GpuDetector.h"
#include "../Utils/AppLogger.h"

namespace OpenTune {

VocoderCreationResult VocoderFactory::create(
    const std::string& modelPath,
    Ort::Env& env)
{
    auto& gpu = GpuDetector::getInstance();

    if (gpu.getSelectedBackend() == GpuDetector::GpuBackend::DirectML) {
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
    
    AppLogger::info("[VocoderFactory] Creating CPU vocoder...");
    
    try {
        Ort::SessionOptions sessionOptions;
        
        sessionOptions.DisableMemPattern();
        sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        
#ifdef _WIN32
        std::wstring wPath(modelPath.begin(), modelPath.end());
        auto session = std::make_unique<Ort::Session>(env, wPath.c_str(), sessionOptions);
#else
        auto session = std::make_unique<Ort::Session>(env, modelPath.c_str(), sessionOptions);
#endif
        
        auto vocoder = std::make_unique<PCNSFHifiGANVocoder>(std::move(session));
        
        AppLogger::info("[VocoderFactory] CPU vocoder created successfully");
        
        return VocoderCreationResult::success(std::move(vocoder), VocoderBackend::CPU);
        
    } catch (const std::exception& e) {
        return VocoderCreationResult::failure(VocoderBackend::CPU, 
            "CPU vocoder creation failed: " + std::string(e.what()));
    }
}

} // namespace OpenTune
