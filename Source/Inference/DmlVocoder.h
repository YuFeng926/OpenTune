#pragma once

#include "OnnxVocoderBase.h"
#ifdef _WIN32
#include <dml_provider_factory.h>
#endif

namespace OpenTune {

class DmlVocoder : public OnnxVocoderBase {
public:
    explicit DmlVocoder(const std::string& modelPath,
                        Ort::Env& env,
                        int adapterIndex);
    ~DmlVocoder() override;

protected:
    std::vector<float> runSession(
        VocoderScratchBuffers& scratch,
        size_t numFrames,
        Ort::RunOptions& runOptions) override;

private:
    void initializeSession(const std::string& modelPath,
                           Ort::Env& env,
                           int adapterIndex);
    void initializeIOBinding();

    Ort::MemoryInfo cpuMemoryInfo_;
    std::unique_ptr<Ort::IoBinding> ioBinding_;
    std::unique_ptr<Ort::Value> preallocatedOutput_;
    std::vector<float> outputBuffer_;
    size_t preallocatedFrames_ = 0;
    bool ioBindingInitialized_ = false;
};

} // namespace OpenTune
