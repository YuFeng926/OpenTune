#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <vector>
#include <functional>
#include "../Utils/Error.h"

namespace Ort { struct Env; }

namespace OpenTune {

class VocoderInferenceService;
class VocoderRenderScheduler;

class VocoderDomain {
public:
    struct Job {
        uint64_t chunkKey{0};
        std::vector<float> f0;
        std::vector<float> mel;
        std::function<void(bool, const juce::String&, const std::vector<float>&)> onComplete;
    };

    VocoderDomain(std::shared_ptr<Ort::Env> env);
    ~VocoderDomain();

    bool initialize(const std::string& modelDir);
    void shutdown();
    void submit(Job job);
    int getVocoderHopSize() const;
    int getMelBins() const;

private:
    std::unique_ptr<VocoderInferenceService> inferenceService_;
    std::unique_ptr<VocoderRenderScheduler> scheduler_;  // declared after inferenceService_ — C++ destroys in reverse order

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocoderDomain)
};

} // namespace OpenTune
