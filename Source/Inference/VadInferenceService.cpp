#include "VadInferenceService.h"
#include "SileroVadExtractor.h"
#include "../Utils/AppLogger.h"

#include <chrono>

namespace OpenTune {

VadInferenceService::VadInferenceService() = default;

VadInferenceService::~VadInferenceService()
{
    shutdown();
}

bool VadInferenceService::initialize(const std::string& modelDir)
{
    std::unique_lock<std::shared_mutex> lock(extractorMutex_);
    modelDir_ = modelDir;
    initializeAttempted_ = true;

    auto candidate = std::make_unique<SileroVadExtractor>();
    const std::string fullPath = modelDir + "/silero_vad.onnx";
    if (candidate->initialize(fullPath)) {
        extractor_ = std::move(candidate);
        lastUsedTime_ = std::chrono::steady_clock::now();
        initializeSucceeded_ = true;
        return true;
    }
    initializeSucceeded_ = false;
    return false;
}

void VadInferenceService::shutdown()
{
    std::unique_lock<std::shared_mutex> lock(extractorMutex_);
    extractor_.reset();
    initializeSucceeded_ = false;
}

void VadInferenceService::releaseIdleModelIfNeeded()
{
    std::unique_lock<std::shared_mutex> lock(extractorMutex_);
    if (extractor_ == nullptr) return;
    const auto now = std::chrono::steady_clock::now();
    const auto idleFor = std::chrono::duration_cast<std::chrono::seconds>(now - lastUsedTime_);
    if (idleFor >= idleTimeoutSeconds_) {
        AppLogger::log("[VadInferenceService] releasing idle Silero VAD model after "
                       + juce::String(static_cast<int>(idleFor.count())) + "s");
        extractor_.reset();
    }
}

bool VadInferenceService::isReady() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(extractorMutex_);
    return extractor_ != nullptr;
}

bool VadInferenceService::ensureLoaded()
{
    // Caller holds unique lock.
    if (extractor_ != nullptr) return true;
    if (!initializeAttempted_ || !initializeSucceeded_) return false;
    if (modelDir_.empty()) return false;

    auto candidate = std::make_unique<SileroVadExtractor>();
    const std::string fullPath = modelDir_ + "/silero_vad.onnx";
    if (candidate->initialize(fullPath)) {
        extractor_ = std::move(candidate);
        lastUsedTime_ = std::chrono::steady_clock::now();
        return true;
    }
    return false;
}

std::vector<float> VadInferenceService::extractVad(const float* audio,
                                                    size_t length,
                                                    int sampleRate,
                                                    std::function<void(float)> progressCallback)
{
    std::unique_lock<std::shared_mutex> lock(extractorMutex_);
    if (!ensureLoaded()) return {};
    auto result = extractor_->extractVad(audio, length, sampleRate, std::move(progressCallback));
    lastUsedTime_ = std::chrono::steady_clock::now();
    return result;
}

} // namespace OpenTune
