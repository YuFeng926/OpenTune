#pragma once

#include "OnnxVocoderBase.h"

namespace OpenTune {

class PCNSFHifiGANVocoder : public OnnxVocoderBase {
public:
    explicit PCNSFHifiGANVocoder(std::unique_ptr<Ort::Session> session);
    ~PCNSFHifiGANVocoder() override;

protected:
    std::vector<float> runSession(
        VocoderScratchBuffers& scratch,
        size_t numFrames) override;
};

} // namespace OpenTune
