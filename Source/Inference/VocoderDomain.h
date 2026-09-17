#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <vector>
#include <functional>
#include "../Utils/Error.h"
#include "VocoderRenderScheduler.h"
#include "VocoderInterface.h"

namespace Ort { struct Env; }

namespace OpenTune {

class VocoderDomain {
public:
    using JobResult = VocoderRenderScheduler::JobResult;

    struct Job {
        std::vector<float> f0;
        std::vector<float> uv;  // 1=voiced 显式浊音掩码；空则由 f0>0 推导
        std::vector<float> conditioning;
        std::function<void(JobResult, const juce::String&, const std::vector<float>&)> onComplete;
    };

    VocoderDomain(std::shared_ptr<Ort::Env> env);
    ~VocoderDomain();

    bool initialize(const std::string& modelPath);
    void shutdown();
    bool submit(Job job);
    int getVocoderHopSize() const;
    int getConditioningBins() const;
    VocoderConditioningType getConditioningType() const;
    float getFMax() const;

private:
    std::unique_ptr<VocoderInferenceService> inferenceService_;
    std::unique_ptr<VocoderRenderScheduler> scheduler_;  // declared after inferenceService_ — C++ destroys in reverse order

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocoderDomain)
};

} // namespace OpenTune
