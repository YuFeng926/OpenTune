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
};

} // namespace OpenTune
