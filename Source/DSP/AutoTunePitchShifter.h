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
/// address continuity across all transitions. The desired pitch comes from
/// correctedF0; a value <= 0 expresses no correction request and keeps the
/// target rate at 1. The output pointer is resampled with a floating-point
/// rate and one floating-point cycle period is inserted or removed when the
/// pointer crosses the input pointer. Five samples of lookahead are consumed
/// so output time is aligned with the published input window.
///
/// shiftChunk only serves the lightweight contract: every resample-rate
/// update must stay within ±75 cents. With a detector shadow only samples
/// with trackingUpdated == true apply a rate update; hop-held samples keep
/// the held rate, so contract validation covers exactly those update points.
/// The input is validated before rendering; a violating request returns an
/// empty vector and renders nothing. The repair entry point resolves detector
/// octave ambiguity before reporting a validation failure.
class AutoTunePitchShifter {
public:
    explicit AutoTunePitchShifter(double sampleRate);

    static constexpr int kLookaheadSamples = 5;

    /// @return numSamples resampled samples, or an empty vector (with no
    ///         audio rendered) when any resample-rate update falls outside
    ///         the ±75 cent lightweight contract. numSamples == 0 also
    ///         returns an empty vector. Callers use
    ///         shiftChunkWithOctaveRepair to resolve a wrong detector octave.
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

    struct OctaveRepairResult {
        std::vector<float> audio;
        bool success = false;
        int octave = 0;
        double targetErrorCents = 0.0;
        double continuityCents = 0.0;
    };

    /// Renders the chunk and repairs a wrong detector octave in one pass.
    /// The corrected F0 target is fixed. The ±75 cent contract bounds every
    /// rate update: a tracking update or forced span-boundary update whose
    /// requested rate leaves the contract keeps the held rate, and a repair
    /// candidate that still requests such a rate is rejected. Contiguous
    /// samples whose requested rate leaves the contract are retried with the
    /// detector period scaled by 1/2 or 2; the candidate that keeps the
    /// target and the adjacent target residual continuous is published.
    /// A failed repair returns success == false.
    OctaveRepairResult shiftChunkWithOctaveRepair(
        const float* input, int numSamples,
        const float* originalF0, const float* correctedF0,
        int numF0Frames, double f0FrameRate,
        double firstSampleFramePhase,
        const float* lookahead, int numLookaheadSamples,
        const float* lookbehind, int numLookbehindSamples,
        const AutoTunePeriodDetector::DetectedPeriod* detectorPeriods,
        int numDetectorSamples,
        bool snapMode) const;

private:
    std::vector<float> shiftChunkInternal(
        const float* input, int numSamples,
        const float* originalF0, const float* correctedF0,
        int numF0Frames, double f0FrameRate,
        double firstSampleFramePhase,
        const float* lookahead, int numLookaheadSamples,
        const float* lookbehind, int numLookbehindSamples,
        const AutoTunePeriodDetector::DetectedPeriod* detectorPeriods,
        int numDetectorSamples,
        bool snapMode,
        bool enforceRateContract);

    double sampleRate_;

    std::vector<float> buffer_;
    int bufferSize_ = 0;
    int writePos_ = 0;

    double outputAddr_ = 0.0;
    double inputAddr_ = 0.0;
    double resampleRate_ = 1.0;
    /// Set when a contract hold would move the output more than ±75 cents
    /// away from the requested target. The render is then discarded.
    bool rateHoldExceeded_ = false;

    static constexpr double kDecayPerTrackingUpdate = 0.995;

    bool isReadable(double addr) const;
    float readInterpolated(double addr) const;
    void feedSample(float sample);
    /// updateResampleRate == false keeps the currently held smoothed rate
    /// (detector shadow hop-held samples between tracking update events).
    /// A tracking update whose target rate leaves the ±75 cent contract also
    /// keeps the held rate; the contract bounds every applied rate.
    float processSample(double cyclePeriod, double targetResampleRate,
                        bool updateResampleRate = true,
                        bool snapMode = false);
};

} // namespace OpenTune
