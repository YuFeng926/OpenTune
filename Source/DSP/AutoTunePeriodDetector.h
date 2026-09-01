#pragma once

#include <vector>

namespace OpenTune {

/// Chunk-local render-rate period detector with a coarse-then-confirm flow:
/// coarse LPF/downsampling acquisition, full-rate fundamental confirmation,
/// and an eight-lag E/H tracking window.
class AutoTunePeriodDetector {
public:
    /// One detection result per input sample (hop-held).
    struct DetectedPeriod {
        float periodSamples = 0.0f; ///< Refined period in full-rate samples.
        bool valid = false;          ///< false => caller must stay neutral.
        /// True only when this sample completed a tracking-window evaluation
        /// (acquisition initialization or the every-5-samples tracking
        /// update). Hop-held repeats report false so consumers refresh the
        /// smoothed resample rate only on real detector update events.
        bool trackingUpdated = false;
    };

    /// Analyzes prefix (samples immediately preceding input[0]) + input and
    /// returns exactly numInputSamples results, one per input sample.
    /// @param fcpeF0Hint  Optional per-frame FCPE F0 array (Hz, >0 = voiced).
    ///                    Used as octave prior for coarse acquisition when
    ///                    available.  May be nullptr when no AI F0 is present.
    /// @param f0HintFrameRate  Frame rate of fcpeF0Hint (e.g. 100.0).
    static std::vector<DetectedPeriod> analyze(
        const float* lookbehind, int numLookbehindSamples,
        const float* input, int numInputSamples,
        double sampleRate,
        const float* fcpeF0Hint = nullptr,
        int numF0HintFrames = 0,
        double f0HintFrameRate = 0.0);

    /// Coarse-stage decimation factor.
    static constexpr int kDecimFactor = 8;
    /// Coarse lag bounds at the decimated rate.
    static constexpr int kMinDecimLag = 2;
    static constexpr int kMaxDecimLag = 110;

    /// Full-rate lag bounds for the 44.1 kHz reference sample grid.
    static constexpr int kMinFullLag = kMinDecimLag * kDecimFactor;
    static constexpr int kMaxFullLag = kMaxDecimLag * kDecimFactor;

    /// Fixed causal anti-alias FIR used before 8:1 decimation.
    static constexpr int kCoarseFilterTaps = 63;
    /// Number of full-rate samples retained for coarse acquisition (default).
    static constexpr int kCoarseWindowSamples = 2048;
    /// Larger coarse window for cross-validation (low-frequency robustness).
    static constexpr int kCoarseWindowSamplesLarge = 4096;
    /// History required by the causal FIR and coarse window.
    static constexpr int kRequiredLookbehindSamples =
        kCoarseWindowSamplesLarge + kCoarseFilterTaps - 1;
};

} // namespace OpenTune
