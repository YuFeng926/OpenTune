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
/// shiftChunk only serves the lightweight contract: every requested resample
/// rate must stay within ±75 cents. It validates the complete input
/// before rendering; if any sample would request a rate outside the contract
/// it returns an empty vector and renders nothing. The repair entry point
/// resolves detector octave ambiguity before reporting a validation failure.
class AutoTunePitchShifter {
public:
    explicit AutoTunePitchShifter(double sampleRate);

    static constexpr int kLookaheadSamples = 5;

    /// @return numSamples resampled samples, or an empty vector (with no
    ///         audio rendered) when any requested resample rate falls outside
    ///         the ±75 cent lightweight contract. numSamples == 0 also
    ///         returns an empty vector. Callers use
    ///         shiftChunkWithOctaveRepair to resolve a wrong detector octave
    ///         before treating the request as a failure.
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
    /// The corrected F0 target is fixed. Contiguous samples whose requested
    /// rate leaves the ±75 cent contract are retried with the detector period
    /// scaled by 1/2 or 2; the candidate that keeps the target and the
    /// adjacent target residual continuous is published. A failed repair
    /// returns success == false; the caller reports the AutoTune validation
    /// failure and does not fall back to another renderer.
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
