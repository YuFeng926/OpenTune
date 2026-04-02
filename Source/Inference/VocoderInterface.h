#pragma once

#include <vector>
#include <string>
#include <cstddef>

namespace OpenTune {

struct VocoderInfo {
    std::string name;
    std::string backend;
    int hopSize = 512;
    int sampleRate = 44100;
    int melBins = 128;
};

class VocoderInterface {
public:
    virtual ~VocoderInterface() = default;

    virtual std::vector<float> synthesize(
        const std::vector<float>& f0,
        const float* mel,
        size_t melSize
    ) = 0;

    virtual VocoderInfo getInfo() const = 0;
    virtual size_t estimateMemoryUsage(size_t frames) const = 0;

    virtual int getHopSize() const { return 512; }
    virtual int getSampleRate() const { return 44100; }
    virtual int getMelBins() const { return 128; }
    virtual std::string getName() const = 0;
    virtual bool requiresMel() const { return true; }
};

} // namespace OpenTune
