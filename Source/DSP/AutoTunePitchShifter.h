#pragma once

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace OpenTune {

/// Lightweight per-sample pitch shifter based on adaptive resampling rate
/// with cycle insertion/deletion (the reference flow method, patent expired 2018).
///
/// O(1) per sample, no FFT, fixed memory, deterministic latency (~5 samples).
///
/// Usage range: corrections < 100 cents. For larger shifts, use NSF-HiFiGAN vocoder.
class AutoTunePitchShifter {
public:
    explicit AutoTunePitchShifter(double sampleRate);
    ~AutoTunePitchShifter();

    /// Reset internal state (call between non-contiguous audio segments).
    void reset();

    /// Process a chunk of audio with per-frame F0 guidance.
    /// Drop-in replacement for the legacy cycle-resampling pitch shifter.
    /// @param input         Source audio (mono, sampleRate)
    /// @param numSamples    Number of input samples
    /// @param originalF0    Detected F0 per frame (Hz, f0FrameRate fps)
    /// @param correctedF0   Target F0 per frame (Hz, f0FrameRate fps)
    /// @param numF0Frames   Number of F0 frames
    /// @param f0FrameRate   F0 frame rate (typically 100.0)
    /// @return Pitch-shifted audio (same length as input)
    std::vector<float> shiftChunk(
        const float* input, int numSamples,
        const float* originalF0, const float* correctedF0,
        int numF0Frames, double f0FrameRate);

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

    // Crossfade state
    static constexpr int kCrossfadeSamples = 32;  // ~0.7ms @ 44.1kHz, smooth cycle boundaries
    bool inCrossfade_ = false;
    int crossfadeRemaining_ = 0;
    double crossfadeFromAddr_ = 0.0;

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
