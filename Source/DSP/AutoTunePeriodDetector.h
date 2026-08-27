#pragma once

#include <vector>

namespace OpenTune {

/// Chunk-local render-rate period detector following the the reference flow flow:
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
    static std::vector<DetectedPeriod> analyze(
        const float* lookbehind, int numLookbehindSamples,
        const float* input, int numInputSamples,
        double sampleRate);

    /// the reference flow coarse-stage decimation factor.
    static constexpr int kDecimFactor = 8;
    /// Coarse lag bounds at the decimated rate.
    static constexpr int kMinDecimLag = 2;
    static constexpr int kMaxDecimLag = 110;

    /// Full-rate lag bounds for the reference 44.1 kHz sample grid.
    static constexpr int kMinFullLag = kMinDecimLag * kDecimFactor;
    static constexpr int kMaxFullLag = kMaxDecimLag * kDecimFactor;

    /// Fixed causal anti-alias FIR used before 8:1 decimation.
    static constexpr int kCoarseFilterTaps = 63;
    /// Number of full-rate samples retained for coarse acquisition.
    static constexpr int kCoarseWindowSamples = 2048;
    /// History required by the causal FIR and coarse window.
    static constexpr int kRequiredLookbehindSamples =
        kCoarseWindowSamples + kCoarseFilterTaps - 1;
};

} // namespace OpenTune
