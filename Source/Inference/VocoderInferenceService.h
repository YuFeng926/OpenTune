#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include "VocoderInterface.h"
#include "../Utils/Error.h"

namespace OpenTune {

class VocoderInferenceService {
public:
    VocoderInferenceService();
    ~VocoderInferenceService();

    bool initialize(const std::string& modelDir);

    void shutdown();

    Result<std::vector<float>> synthesizeAudioWithEnergy(
        const std::vector<float>& f0,
        const std::vector<float>& energy,
        const float* mel,
        size_t melSize);

    virtual Result<std::vector<float>> doSynthesizeAudioWithEnergy(
        const std::vector<float>& f0,
        const std::vector<float>& energy,
        const float* mel,
        size_t melSize);

    int getVocoderHopSize() const;

    int getMelBins() const;

    bool isInitialized() const;

private:
    std::mutex runMutex_;
    class Impl;
    std::unique_ptr<Impl> pImpl_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocoderInferenceService)
};

} // namespace OpenTune
