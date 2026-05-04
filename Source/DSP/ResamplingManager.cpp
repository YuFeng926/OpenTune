#include "ResamplingManager.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#endif

#include "CDSPResampler.h"
#include "Utils/TimeCoordinate.h"
#include <cmath>
#include <algorithm>
#include <limits>

namespace OpenTune {

ResamplingManager::ResamplingManager() {
}

ResamplingManager::~ResamplingManager() {
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
