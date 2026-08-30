#include "VocoderFactory.h"
#include "PCNSFHifiGANVocoder.h"
#ifdef _WIN32
#include "DmlVocoder.h"
#endif
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"
#include <optional>
#include <unordered_map>

namespace OpenTune {

namespace {

// 权重旁 <同名>.yaml sidecar：读取 mel_fmax。fmax 不在 ONNX schema 内
// （见 VocoderInterface 注释），训练时 mel 滤波器组参数只能随权重走配置文件。
std::optional<float> loadSidecarMelFMax(const std::string& modelPath)
{
    const juce::File yamlFile = juce::File(modelPath).withFileExtension("yaml");
    if (!yamlFile.existsAsFile())
        return std::nullopt;

    const auto lines = juce::StringArray::fromLines(yamlFile.loadFileAsString());
    for (const auto& line : lines) {
        const auto trimmed = line.trim();
        if (!trimmed.startsWith("mel_fmax"))
            continue;
        const float value = trimmed.fromFirstOccurrenceOf(":", false, false).trim().getFloatValue();
        if (value > 0.0f)
            return value;
    }
    return std::nullopt;
}

void applySidecarConfig(const std::string& modelPath, VocoderInterface& vocoder)
{
    const auto fMax = loadSidecarMelFMax(modelPath);
    if (fMax.has_value()) {
        vocoder.setMelFMax(*fMax);
        AppLogger::info("[VocoderFactory] Sidecar mel_fmax=" + juce::String(*fMax)
            + " (" + juce::File(modelPath).getFileName() + ".yaml)");
    }
}

} // namespace

VocoderCreationResult VocoderFactory::create(
    const std::string& modelPath,
    Ort::Env& env)
{
#ifdef _WIN32
    auto& gpu = AccelerationDetector::getInstance();

    if (gpu.getSelectedBackend() == AccelerationDetector::AccelBackend::DirectML) {
        const auto& gpuInfo = gpu.getSelectedGpu();
        const int adapterIndex = gpu.getDirectMLDeviceId();

        AppLogger::info("[VocoderFactory] DML backend selected by GPU detector");
        AppLogger::info("[VocoderFactory]   GPU: " + juce::String(gpuInfo.name));
        AppLogger::info("[VocoderFactory]   adapterIndex=" + juce::String(static_cast<int>(gpuInfo.adapterIndex)));

        AppLogger::info("[VocoderFactory] Creating DML vocoder...");

        try {
            auto vocoder = std::make_unique<DmlVocoder>(modelPath, env, adapterIndex);

            AppLogger::info("[VocoderFactory] DML vocoder created successfully");

            applySidecarConfig(modelPath, *vocoder);

            const float fmax = vocoder->getFMax();
            const float nyquist = static_cast<float>(vocoder->getSampleRate()) * 0.5f;
            if (fmax > nyquist) {
                return VocoderCreationResult::failure(VocoderBackend::DML,
                    "Vocoder config violates Nyquist: fmax=" + std::to_string(fmax)
                    + " > Nyquist=" + std::to_string(nyquist)
                    + " (sampleRate=" + std::to_string(vocoder->getSampleRate()) + ")");
            }
            AppLogger::info("[VocoderFactory] Vocoder config OK: fmax=" + juce::String(fmax)
                + ", sampleRate=" + juce::String(vocoder->getSampleRate())
                + " (Nyquist=" + juce::String(nyquist) + ")");

            return VocoderCreationResult::success(std::move(vocoder), VocoderBackend::DML);

        } catch (const std::exception& e) {
            AppLogger::error("[VocoderFactory] DML vocoder creation FAILED: "
                + juce::String(e.what()));
            AppLogger::warn("[VocoderFactory] GPU DML initialization failed, switching to CPU vocoder");
            gpu.overrideBackend(AccelerationDetector::AccelBackend::CPU);
        }
    }
#endif
    
    try {
        Ort::SessionOptions sessionOptions;
        
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

        applySidecarConfig(modelPath, *vocoder);

        const float fmax = vocoder->getFMax();
        const float nyquist = static_cast<float>(vocoder->getSampleRate()) * 0.5f;
        if (fmax > nyquist) {
            return VocoderCreationResult::failure(selectedBackend,
                "Vocoder config violates Nyquist: fmax=" + std::to_string(fmax)
                + " > Nyquist=" + std::to_string(nyquist)
                + " (sampleRate=" + std::to_string(vocoder->getSampleRate()) + ")");
        }
        AppLogger::info("[VocoderFactory] Vocoder config OK: fmax=" + juce::String(fmax)
            + ", sampleRate=" + juce::String(vocoder->getSampleRate())
            + " (Nyquist=" + juce::String(nyquist) + ")");

        return VocoderCreationResult::success(std::move(vocoder), selectedBackend);
        
    } catch (const std::exception& e) {
        return VocoderCreationResult::failure(VocoderBackend::CPU, 
            "Vocoder creation failed: " + std::string(e.what()));
    }
}

} // namespace OpenTune
