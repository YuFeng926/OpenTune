#pragma once

#include <vector>

#include "AutoTunePeriodDetector.h"

namespace OpenTune {

/// Per-sample pitch correction following the reference flow correction mode.
///
/// The measured cycle period source is either the supplied originalF0 track
/// or, when a detector shadow source is provided, per-sample detected periods
/// from AutoTunePeriodDetector. In shadow mode, post-processed originalF0 is
/// used only as the project's voiced/unvoiced gate; zero-F0 frames run neutral.
/// The desired pitch always comes from correctedF0 (effectiveF0). The output
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
        int numLookbehindSamples = 0,
        // Optional per-sample detected period shadow source, parallel to
        // input. Non-null replaces originalF0 as the measured period source.
        // Unvoiced gate samples run sample-exact neutral passthrough; voiced
        // detector failures keep rate 1 on the continuous resampled path.
        // The smoothed resample rate is refreshed only on trackingUpdated
        // events from the detector.
        const AutoTunePeriodDetector::DetectedPeriod* detectorPeriods = nullptr,
        int numDetectorSamples = 0);

private:
    double sampleRate_;

    std::vector<float> buffer_;
    int bufferSize_ = 0;
    int writePos_ = 0;

    double outputAddr_ = 0.0;
    double inputAddr_ = 0.0;
    double resampleRate_ = 1.0;

    static constexpr double kDecayPerTrackingUpdate = 0.995;

    bool isReadable(double addr) const;
    float readInterpolated(double addr) const;
    void feedSample(float sample);
    /// updateResampleRate == false keeps the currently held smoothed rate
    /// (detector shadow hop-held samples between tracking update events).
    float processSample(double cyclePeriod, double targetResampleRate,
                        bool updateResampleRate = true);
};

} // namespace OpenTune
