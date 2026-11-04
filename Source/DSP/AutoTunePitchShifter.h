#pragma once

#include <vector>

#include "AutoTunePeriodDetector.h"

namespace OpenTune {

/// Per-sample pitch correction with floating-point cycle resampling.
///
/// The measured cycle period source is either the supplied originalF0 track
/// or, when a detector shadow source is provided, per-sample detected periods
/// from AutoTunePeriodDetector. In shadow mode the detector's valid flag
/// determines voiced/unvoiced — the resampler never hard-resets, preserving
/// address continuity across all transitions. The desired pitch always comes
/// from correctedF0 (effectiveF0); a missing target remains neutral rather
/// than becoming an RMVPE-derived UV decision. The output pointer is resampled
/// with a floating-point rate and one floating-point cycle period is inserted
/// or removed when the pointer crosses the input pointer. Five samples of
/// lookahead are consumed so output time is aligned with the published input
/// window.
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
        const float* lookbehind = nullptr,
        int numLookbehindSamples = 0,
        const AutoTunePeriodDetector::DetectedPeriod* detectorPeriods = nullptr,
        int numDetectorSamples = 0,
        bool snapMode = false);

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
                        bool updateResampleRate = true,
                        bool snapMode = false);
};

} // namespace OpenTune
