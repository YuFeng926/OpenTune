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
        size_t numFrames) override;

private:
    void initializeSession(const std::string& modelPath,
                           Ort::Env& env,
                           int adapterIndex);
    void initializeIOBinding();
    std::vector<float> copyBoundOutputToCpu(Ort::Value deviceOutput,
                                            size_t expectedAudioLength);

    Ort::Env* env_ = nullptr;
    std::unique_ptr<Ort::IoBinding> ioBinding_;
    std::vector<Ort::ConstMemoryInfo> outputMemoryInfos_;
    bool ioBindingInitialized_ = false;
};

} // namespace OpenTune
