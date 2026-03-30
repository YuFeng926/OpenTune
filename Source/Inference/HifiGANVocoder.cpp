#include "HifiGANVocoder.h"
#include "../Utils/AppLogger.h"

namespace OpenTune {

HifiGANVocoder::HifiGANVocoder(std::unique_ptr<Ort::Session> session)
    : session_(std::move(session))
{
    memoryInfo_ = std::make_unique<Ort::MemoryInfo>(
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)
    );
}

HifiGANVocoder::~HifiGANVocoder() = default;

std::vector<float> HifiGANVocoder::synthesize(
    const std::vector<float>& f0,
    const float* mel)
{
    if (!session_) {
        AppLogger::error("[HifiGANVocoder] Not initialized");
        return {};
    }

    try {
        // HifiGAN expects: mel [1, 128, frames] and f0 [1, frames]
        size_t numFrames = f0.size();

        AppLogger::debug("[HifiGANVocoder] Processing " + juce::String((int)numFrames) + " frames");

        // If mel is not provided, create a zero mel spectrogram
        std::vector<float> melData;
        if (mel == nullptr) {
            melData.resize(128 * numFrames, 0.0f);
            mel = melData.data();
        }

        // Prepare mel tensor [1, 128, frames]
        std::vector<int64_t> melShape = {1, 128, static_cast<int64_t>(numFrames)};
        auto melTensor = Ort::Value::CreateTensor<float>(
            *memoryInfo_,
            const_cast<float*>(mel),
            128 * numFrames,
            melShape.data(),
            melShape.size()
        );

        // Prepare f0 tensor [1, frames]
        std::vector<int64_t> f0Shape = {1, static_cast<int64_t>(numFrames)};
        auto f0Tensor = Ort::Value::CreateTensor<float>(
            *memoryInfo_,
            const_cast<float*>(f0.data()),
            numFrames,
            f0Shape.data(),
            f0Shape.size()
        );

        // Run inference
        const char* inputNames[] = {"mel", "f0"};
        const char* outputNames[] = {"audio"};
        std::vector<Ort::Value> inputTensors;
        inputTensors.push_back(std::move(melTensor));
        inputTensors.push_back(std::move(f0Tensor));

        auto outputTensors = session_->Run(
            Ort::RunOptions{nullptr},
            inputNames, inputTensors.data(), 2,
            outputNames, 1
        );

        // Extract audio output
        float* audioData = outputTensors[0].GetTensorMutableData<float>();
        auto audioShape = outputTensors[0].GetTensorTypeAndShapeInfo().GetShape();
        
        // 防止 audioShape 为空时的下溢访问
        if (audioShape.empty()) {
            AppLogger::error("[HifiGANVocoder] Error: audioShape is empty");
            return std::vector<float>(f0.size() * 512, 0.0f);
        }
        
        size_t audioLength = audioShape[audioShape.size() - 1];

        std::vector<float> audio(audioData, audioData + audioLength);

        AppLogger::debug("[HifiGANVocoder] Generated " + juce::String((int)audioLength)
                  + " samples (" + juce::String(audioLength / 44100.0f, 2) + "s)");

        return audio;

    } catch (const Ort::Exception& e) {
        AppLogger::error("[HifiGANVocoder] ONNX error: " + juce::String(e.what()));
        return std::vector<float>(f0.size() * 512, 0.0f);
    } catch (const std::exception& e) {
        AppLogger::error("[HifiGANVocoder] Error: " + juce::String(e.what()));
        return std::vector<float>(f0.size() * 512, 0.0f);
    }
}

} // namespace OpenTune
