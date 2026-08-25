#pragma once

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace OpenTune {

/// Lightweight per-sample pitch shifter based on adaptive resampling rate
/// with cycle insertion/deletion (the reference flow method, patent expired 2018).
///
/// Uses autocorrelation for precise cycle boundary detection at jump points,
/// eliminating crossfade artifacts. O(N) per jump, O(1) per sample otherwise.
/// Zero output latency: the internal read pointer's inherent group delay is
/// cancelled by consuming kLookaheadSamples of future input supplied by the caller.
class AutoTunePitchShifter {
public:
    explicit AutoTunePitchShifter(double sampleRate);
    ~AutoTunePitchShifter();

    /// Reset internal state (call between non-contiguous audio segments).
    void reset();

    /// Future samples the shifter consumes before producing output; cancels
    /// the read pointer's inherent group delay so output[i] aligns with input[i].
    /// Callers must supply this many trailing clip samples via shiftChunk().
    static constexpr int kLookaheadSamples = 5;

    /// Process a chunk of audio with per-frame F0 guidance.
    /// @param input         Source audio to publish (mono, sampleRate)
    /// @param numSamples    Number of publish samples
    /// @param originalF0    Detected F0 per frame (Hz, f0FrameRate fps)
    /// @param correctedF0   Target F0 per frame (Hz, f0FrameRate fps)
    /// @param numF0Frames   Number of F0 frames
    /// @param f0FrameRate   F0 frame rate (typically 100.0)
    /// @param firstSampleFramePhase F0 frame phase of the first publish sample
    /// @param lookahead     Samples following input in the source clip
    ///                      (kLookaheadSamples expected; shorter tails are
    ///                      zero-padded, only affects clip-final chunks)
    /// @param numLookaheadSamples Available lookahead samples (<= kLookaheadSamples)
    /// @return Pitch-shifted audio, strictly sample-aligned with input
    std::vector<float> shiftChunk(
        const float* input, int numSamples,
        const float* originalF0, const float* correctedF0,
        int numF0Frames, double f0FrameRate,
        double firstSampleFramePhase,
        const float* lookahead = nullptr,
        int numLookaheadSamples = 0);

private:
    double sampleRate_;

    // Circular input buffer
    std::vector<float> buffer_;
    int bufferSize_ = 0;
    int writePos_ = 0;

    // Core state machine (the reference flow Claims 5-10)
    double outputAddr_ = 0.0;    // fractional read pointer
    double inputAddr_ = 0.0;     // write pointer (total samples fed)
    double resampleRate_ = 1.0;  // current playback rate (smoothed)

    // Autocorrelation cycle boundary detection
    static constexpr double kAutocorrSearchRatio = 0.10;  // ±10% of estimated period
    static constexpr int kAutocorrSearchMinMargin = 8;    // minimum margin for high freq
    static constexpr int kAutocorrMinOverlap = 16;        // minimum overlap for correlation
    int findCycleBoundary(double approxPeriod) const;

    // Configuration
    static constexpr int kMaxPeriodSamples = 1024;  // ~43 Hz @ 44.1kHz
    static constexpr int kMinPeriodSamples = 20;    // ~2205 Hz @ 44.1kHz
    static constexpr double kDecayPerSample = 0.995; // retune speed smoothing

    // Helpers
    float readInterpolated(double addr) const;
    void feedSample(float sample);
    float processSample(double currentPeriod, double targetResampleRate);
};

} // namespace OpenTune
