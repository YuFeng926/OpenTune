#pragma once

#include <juce_dsp/juce_dsp.h>

namespace OpenTune {

/**
 * LR4 (4th-order Linkwitz-Riley, 24 dB/oct) crossover mixer.
 *
 * Splits two input signals (dry + rendered) through a fixed 16 kHz crossover:
 *   output = LPF(rendered) + HPF(dry)
 *
 * When rendered == dry (no correction), LPF(dry) + HPF(dry) ≈ dry
 * due to LR4 magnitude-flat summation.
 */
class CrossoverMixer
{
public:
    CrossoverMixer() { prepare(44100.0, 512, 2); }

    /** Prepare filters for a given sample rate, block size, and channel count. */
    void prepare(double sampleRate, int maxBlockSize, int numChannels = 2);

    /** Reset all internal filter states to zero. */
    void reset();

    /**
     * Process a single sample pair through the crossover.
     * @param channel  Channel index (for multi-channel filter state)
     * @param dry      Original audio sample
     * @param rendered Vocoder-rendered audio sample (pass dry if no render available)
     * @return LPF(rendered) + HPF(dry)
     */
    float processSample(int channel, float dry, float rendered);

private:
    static constexpr float kCrossoverFrequencyHz = 14000.0f;

    juce::dsp::LinkwitzRileyFilter<float> dryFilter_;
    juce::dsp::LinkwitzRileyFilter<float> renderedFilter_;
};

} // namespace OpenTune
