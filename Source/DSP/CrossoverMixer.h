#pragma once

#include <juce_dsp/juce_dsp.h>

namespace OpenTune {

/**
 * LR4 (4th-order Linkwitz-Riley, 24 dB/oct) crossover mixer.
 *
 * Splits two input signals (dry + rendered) through a fixed 14 kHz crossover:
 * output = LPF(rendered) + HPF(dry)
 *
 * When rendered == dry (no correction), LPF(dry) + HPF(dry) ≈ dry
 * due to LR4 magnitude-flat summation.
 */
class CrossoverMixer
{
public:
    CrossoverMixer() { prepare(44100.0, 512, 2); }

    void prepare(double sampleRate, int maxBlockSize, int numChannels = 2);

    float processSample(int channel, float dry, float rendered);

private:
    static constexpr float kCrossoverFrequencyHz = 14000.0f;

    juce::dsp::LinkwitzRileyFilter<float> dryFilter_;
    juce::dsp::LinkwitzRileyFilter<float> renderedFilter_;
};

} // namespace OpenTune
