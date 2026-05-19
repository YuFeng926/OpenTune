#include "RubberBandStretcher.h"

#include <rubberband/RubberBandStretcher.h>

#include "../Utils/AppLogger.h"

#include <cmath>

namespace OpenTune {

namespace {

using RB = RubberBand::RubberBandStretcher;

// tune-rubberband-options-for-vocal: vocal-tuned options replace P0.0 generic defaults.
// FINAL config (post L5 round-4 user feedback): phase-purity-first, transient sharpness
// sacrificed by user choice. R3 multi-resolution mode (WindowStandard) retained.
//
// Iteration history (see design.md for full reasoning):
//   v0 (P0.0): Crisp  + Compound + WindowStandard         → flanger on vowels (root issue)
//   v1:        Mixed  + Soft     + WindowStandard         → vowel improved, consonant chorus
//   v2:        Mixed  + Compound + WindowStandard         → vowel improved, consonant chorus persists
//   v3:        Smooth + Compound + WindowStandard         → vowel ok, "still some chorus, attack ok"
//   v4 (tried): Smooth + Compound + WindowShort          → no audible improvement vs v3 → reverted
//   v5 (here): Smooth + Compound + WindowStandard        → ⭐ same as v3, retained as final
//
// Note: SmoothingOn/Off and PhaseLaminar/Independent flags have NO EFFECT under the R3
// engine (R2-only per RB header lines 240, 311). PitchHighQuality is "not adjustable in
// offline mode" (line 338). Earlier iterations toggled these flags expecting audible
// effect — the changes were mathematically no-ops; only the Crisp→Mixed→Smooth transient
// flips ever did real work.
//
// v4 explored WindowShort to eliminate suspected R3 multi-resolution fusion phase artifact,
// but L5 verification showed no audible improvement vs v3 — so WindowStandard's multi-
// resolution processing is NOT the root cause of the residual chorus. Restored to
// WindowStandard to retain R3's full quality processing scheme.
//
// Why Smooth + WindowStandard is the chosen final R3 config:
//   - TransientsSmooth: never resets component phases at detector hits → no flanger
//   - WindowStandard: R3 default; multi-resolution processing for highest sound quality
//   - WindowShort showed no benefit (v4 user A/B), so we don't pay the "expense of some
//     sound quality" penalty that WindowShort carries (RB doc line 297)
//
// Residual chorus source (un-fixable within RB option dimension):
//   - All R3+Offline-effective options have been exhaustively explored
//   - PhaseLaminar/Independent + Smoothing + PitchHigh* are all R2-only / no-op under R3+Offline
//   - Likely root cause is in upstream layer: chunkRenderWorker boundary phase or
//     KeyFrameMap segment ratio jumps — out of scope for this change
//
// Trade-off (accepted by user):
//   - Consonant attacks ('p t k', 's sh') lose sharpness (TransientsSmooth path)
//   - Mild residual chorus from upstream / R3 STFT inherent — acceptable per user
constexpr int kRubberBandOptions =
      RB::OptionProcessOffline       // ⭐ Offline mode (default 0; explicit for clarity)
    | RB::OptionEngineFiner          // R3 engine — best quality
    | RB::OptionTransientsSmooth     // ⭐ Never resets component phases — eliminates flanger root cause
    | RB::OptionDetectorCompound     // RB default — irrelevant under Smooth, kept as default
    | RB::OptionFormantPreserved     // ⭐ Vocal formant stability — critical for vocals
    | RB::OptionPitchHighQuality     // No-op in offline (RB header line 338) but kept for forward compat
    | RB::OptionWindowStandard;      // R3 default — full multi-resolution processing for highest quality

} // namespace

RubberBandStretcher::RubberBandStretcher(double sampleRate, int channels)
    : sampleRate_(sampleRate), channels_(channels)
{
    impl_ = std::make_unique<RB>(static_cast<size_t>(sampleRate),
                                  static_cast<size_t>(channels),
                                  kRubberBandOptions,
                                  /*initialTimeRatio=*/1.0,
                                  /*initialPitchScale=*/1.0);

    AppLogger::log("[RubberBandStretcher] Created (sr=" + juce::String(sampleRate)
                   + " ch=" + juce::String(channels)
                   + " options=Offline+EngineFiner+TransientsSmooth+DetectorCompound+FormantPreserved+PitchHighQuality+WindowStandard)");
}

RubberBandStretcher::~RubberBandStretcher() = default;

int RubberBandStretcher::getRubberBandOptions() noexcept
{
    return kRubberBandOptions;
}

std::map<size_t, size_t> RubberBandStretcher::buildKeyframesFromTimeGrid(const TimeGridSnapshot& grid) const
{
    std::map<size_t, size_t> map;
    for (const auto& h : grid.handles()) {
        // Convert seconds to sample-frame indices
        const auto srcSample = static_cast<size_t>(std::llround(h.source_seconds * sampleRate_));
        const auto outSample = static_cast<size_t>(std::llround(h.output_seconds * sampleRate_));
        map[srcSample] = outSample;
    }
    return map;
}

void RubberBandStretcher::beginRebuild(double timeRatio, const std::map<size_t, size_t>& keyframes)
{
    if (impl_ == nullptr) return;

    // Strict order per RB API:
    //   1. reset() — clears keyframe map AND study state
    //   2. setTimeRatio() — must be set before setKeyFrameMap
    //   3. setKeyFrameMap() — must be set before first process()
    impl_->reset();
    impl_->setTimeRatio(timeRatio);
    impl_->setPitchScale(1.0);   // v7 stretch never changes pitch

    if (!keyframes.empty()) {
        impl_->setKeyFrameMap(keyframes);
    }

    phase_ = Phase::Studying;
    totalInputSamplesStudied_ = 0;
    totalInputSamplesProcessed_ = 0;
    expectedOutputSamples_ = 0;
}

void RubberBandStretcher::study(const float* input, size_t numSamples, bool isLast)
{
    if (impl_ == nullptr || phase_ != Phase::Studying) {
        AppLogger::warn("[RubberBandStretcher] study() called outside Studying phase (phase="
                        + juce::String(static_cast<int>(phase_)) + ")");
        return;
    }

    // RB expects de-interleaved channel pointers
    const float* channelData[1] = { input };
    impl_->study(channelData, numSamples, isLast);
    totalInputSamplesStudied_ += numSamples;

    if (isLast) {
        phase_ = Phase::Processing;
        // After study(isLast=true), expected output samples = totalInput * timeRatio.
        // For v7 locked endpoints, ratio is 1.0, so expected = totalInputSamplesStudied_.
        expectedOutputSamples_ = totalInputSamplesStudied_;
    }
}

void RubberBandStretcher::process(const float* input, size_t numSamples, bool isLast)
{
    if (impl_ == nullptr || phase_ != Phase::Processing) {
        AppLogger::warn("[RubberBandStretcher] process() called outside Processing phase (phase="
                        + juce::String(static_cast<int>(phase_)) + ")");
        return;
    }

    const float* channelData[1] = { input };
    impl_->process(channelData, numSamples, isLast);
    totalInputSamplesProcessed_ += numSamples;
}

size_t RubberBandStretcher::available() const
{
    if (impl_ == nullptr) return 0;
    const int avail = impl_->available();
    if (avail < 0) return 0;   // -1 indicates end-of-stream (final retrieve)
    return static_cast<size_t>(avail);
}

size_t RubberBandStretcher::retrieve(float* output, size_t maxSamples)
{
    if (impl_ == nullptr || maxSamples == 0) return 0;

    float* channelData[1] = { output };
    const size_t got = impl_->retrieve(channelData, maxSamples);

    return got;
}

} // namespace OpenTune
