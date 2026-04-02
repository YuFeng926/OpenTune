#include "ResamplingManager.h"
#include "CDSPResampler.h"
#include "Utils/TimeCoordinate.h"
#include <cmath>
#include <algorithm>
#include <limits>

namespace OpenTune {

ResamplingManager::ResamplingManager() {
}

ResamplingManager::~ResamplingManager() {
    clearCache();
}

std::vector<float> ResamplingManager::downsampleForInference(
    const float* input, size_t inputLength,
    int inputSR, int targetSR
) {
    return resample(input, inputLength, inputSR, targetSR);
}

std::vector<float> ResamplingManager::upsampleForHost(
    const float* input, size_t inputLength,
    int inputSR, int targetSR
) {
    return resample(input, inputLength, inputSR, targetSR);
}

std::vector<float> ResamplingManager::resampleToTargetLength(
    const std::vector<float>& input, size_t targetLength
) {
    if (input.empty() || targetLength == 0) {
        return {};
    }
    
    if (input.size() == targetLength) {
        return input;
    }

    double srcRate = static_cast<double>(input.size());
    double dstRate = static_cast<double>(targetLength);
    
    r8b::CDSPResampler24 resampler(srcRate, dstRate, static_cast<int>(input.size()));
    
    std::vector<float> output(targetLength);
    
    resampler.oneshot(input.data(), static_cast<int>(input.size()), output.data(), static_cast<int>(targetLength));
    
    return output;
}

std::vector<float> ResamplingManager::resample(
    const std::vector<float>& input,
    double srcSampleRate,
    double dstSampleRate
) {
    if (input.empty()) {
        return {};
    }
    
    if (std::abs(srcSampleRate - dstSampleRate) < 0.01) {
        return input;
    }

    const double durationSeconds = TimeCoordinate::samplesToSeconds(static_cast<int64_t>(input.size()), srcSampleRate);
    const int64_t outputLength64 = TimeCoordinate::secondsToSamples(durationSeconds, dstSampleRate);
    if (outputLength64 <= 0) {
        return {};
    }
    const int outputLength = static_cast<int>(std::min<int64_t>(outputLength64, static_cast<int64_t>(std::numeric_limits<int>::max())));

    r8b::CDSPResampler24 resampler(srcSampleRate, dstSampleRate, static_cast<int>(input.size()));
    
    std::vector<float> output(outputLength);
    resampler.oneshot(input.data(), static_cast<int>(input.size()), output.data(), outputLength);
    
    return output;
}

int ResamplingManager::getLatencySamples(int inputSR, int targetSR) const {
    // r8brain CDSPResampler24 的 oneshot 模式内部处理延迟补偿，返回 0
    juce::ignoreUnused(inputSR, targetSR);
    return 0;
}

void ResamplingManager::clearCache() {
}

std::vector<float> ResamplingManager::resample(
    const float* input, size_t inputLength,
    int inputSR, int targetSR
) {
    if (inputSR == targetSR || inputLength == 0) {
        return std::vector<float>(input, input + inputLength);
    }

    r8b::CDSPResampler24 resampler(
        static_cast<double>(inputSR), 
        static_cast<double>(targetSR), 
        static_cast<int>(inputLength)
    );

    const double durationSeconds = TimeCoordinate::samplesToSeconds(static_cast<int64_t>(inputLength), static_cast<double>(inputSR));
    const int64_t outputLength64 = TimeCoordinate::secondsToSamples(durationSeconds, static_cast<double>(targetSR));
    int outputLength = static_cast<int>(std::min<int64_t>(outputLength64, static_cast<int64_t>(std::numeric_limits<int>::max())));
    
    if (outputLength <= 0) outputLength = 1;

    std::vector<float> output(outputLength);

    resampler.oneshot(input, static_cast<int>(inputLength), output.data(), outputLength);

    return output;
}

} // namespace OpenTune
