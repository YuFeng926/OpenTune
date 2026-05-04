#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <string>
#include <vector>
#include "VocoderInterface.h"
#include "../Utils/Error.h"

namespace Ort { struct Env; }

namespace OpenTune {

/** Thread-safety: synthesize() is only called by VocoderRenderScheduler's
 *  single worker thread. initialize/shutdown must not race with synthesize. */
class VocoderInferenceService {
public:
    VocoderInferenceService(std::shared_ptr<Ort::Env> env);
    ~VocoderInferenceService();

    bool initialize(const std::string& modelDir);
    void shutdown();

    Result<std::vector<float>> synthesize(
        const std::vector<float>& f0,
        const float* mel,
        size_t melSize);

    int getVocoderHopSize() const;
    int getMelBins() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocoderInferenceService)
};

} // namespace OpenTune
