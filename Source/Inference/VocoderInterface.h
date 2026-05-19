#pragma once

#include <vector>
#include <cstddef>

namespace OpenTune {

class VocoderInterface {
public:
    virtual ~VocoderInterface() = default;

    virtual std::vector<float> synthesize(
        const std::vector<float>& f0,
        const float* mel,
        size_t melSize
    ) = 0;

    virtual int getHopSize() const { return 512; }
    virtual int getSampleRate() const { return 44100; }
    virtual int getMelBins() const { return 128; }

    // mel filterbank fmax used during training. Pure virtual: every concrete
    // vocoder MUST declare its own value to keep mel computation consistent
    // with the model. fmax is not encoded in ONNX schema, so this contract
    // is the only safeguard against silent config drift.
    virtual float getFMax() const = 0;
};

} // namespace OpenTune
