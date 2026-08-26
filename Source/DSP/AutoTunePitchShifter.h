#pragma once

#include <vector>

namespace OpenTune {

/// Per-sample pitch correction following the reference flow correction mode.
///
/// The supplied original F0 is the measured cycle period source. The output
/// pointer is resampled with a floating-point rate and one floating-point
/// cycle period is inserted or removed when the pointer crosses the input
/// pointer. Five samples of lookahead are consumed so output time is aligned
/// with the published input window.
class AutoTunePitchShifter {
public:
    explicit AutoTunePitchShifter(double sampleRate);

    static constexpr int kLookaheadSamples = 5;

    std::vector<float> shiftChunk(
        const float* input, int numSamples,
        const float* originalF0, const float* correctedF0,
        int numF0Frames, double f0FrameRate,
        double firstSampleFramePhase,
        const float* lookahead = nullptr,
        int numLookaheadSamples = 0,
        // Samples immediately preceding input[0]; needed for cycle jumps.
        const float* lookbehind = nullptr,
        int numLookbehindSamples = 0);

private:
    double sampleRate_;

    std::vector<float> buffer_;
    int bufferSize_ = 0;
    int writePos_ = 0;

    double outputAddr_ = 0.0;
    double inputAddr_ = 0.0;
    double resampleRate_ = 1.0;

    static constexpr double kDecayPerSample = 0.995;

    bool isReadable(double addr) const;
    float readInterpolated(double addr) const;
    void feedSample(float sample);
    float processSample(double cyclePeriod, double targetResampleRate);
};

} // namespace OpenTune
