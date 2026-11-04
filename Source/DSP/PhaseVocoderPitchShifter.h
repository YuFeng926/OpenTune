#pragma once

#include <vector>

namespace OpenTune {

// Offline mono pitch shifting shared by cache rendering and note audition.
// Audio and both F0 tracks start at content time zero. The caller supplies an
// exact output range; STFT context is read from the same immutable full source.
// Independent range requests reconstruct phase from the preceding neutral
// analysis frame on one content-wide grid, so job order never owns DSP state.
class PhaseVocoderPitchShifter {
public:
    static constexpr int kFftOrder = 11;
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr int kHopSize = 256;

    explicit PhaseVocoderPitchShifter(double sampleRate);

    // Returns exactly outputSampleCount samples aligned to outputStartSample.
    // Ratios follow correctedF0 / originalF0. Zero source/target F0 is neutral;
    // positive adjacent controls are interpolated in log-frequency space.
    // A long uninterrupted correction requires phase-history replay from its
    // preceding neutral frame; no mutable cross-job cache is involved.
    std::vector<float> render(
        const float* source, int sourceSampleCount,
        int outputStartSample, int outputSampleCount,
        const float* originalF0, const float* correctedF0,
        int numF0Frames, double f0FrameRate) const;

private:
    double sampleRate_;
};

} // namespace OpenTune
