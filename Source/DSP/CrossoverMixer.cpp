#include "CrossoverMixer.h"

namespace OpenTune {

void CrossoverMixer::prepare(double sampleRate, int maxBlockSize, int numChannels)
{
    juce::dsp::ProcessSpec spec{};
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(maxBlockSize);
    spec.numChannels = static_cast<juce::uint32>(numChannels);

    dryFilter_.setCutoffFrequency(kCrossoverFrequencyHz);
    dryFilter_.prepare(spec);
    dryFilter_.reset();

    renderedFilter_.setCutoffFrequency(kCrossoverFrequencyHz);
    renderedFilter_.prepare(spec);
    renderedFilter_.reset();
}

float CrossoverMixer::processSample(int channel, float dry, float rendered)
{
    float dryLow, dryHigh;
    dryFilter_.processSample(channel, dry, dryLow, dryHigh);

    float renderedLow, renderedHigh;
    renderedFilter_.processSample(channel, rendered, renderedLow, renderedHigh);

    return renderedLow + dryHigh;
}

} // namespace OpenTune
