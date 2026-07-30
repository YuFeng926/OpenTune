/**
 * SoundTouchStretcher — Wrapper around soundtouch::SoundTouch (WSOLA time-stretch).
 *
 * Replaces the archived phase-vocoder stretcher (2026-05-19).  WSOLA is a
 * time-domain algorithm: no STFT analysis-resynthesis, no phase reconstruction —
 * physically eliminates the chorus/phasing artifacts inherent to phase vocoder.
 *
 * Used for OpenTune Stage 2 time-stretch:
 *   - per-content persistent instance (lazy-constructed by content owner)
 *   - re-built fully on every TimeGrid OR PitchCurve revision change
 *   - ratios always positive; pitch unchanged (Stage 1 vocoder handles pitch)
 *   - endpoints invariant → ∫ ratio dt = totalSourceSeconds (output length == input length)
 *
 * Lifecycle:
 *   1. beginRebuild(schedule)  — clear() + cache schedule + reset counters
 *   2. push(chunk, n, isLast)  — drives setTempo() per-segment + putSamples(); on
 *                                isLast=true also calls SoundTouch::flush()
 *   3. pull(out, max)          — drains receiveSamples(); call until available()==0
 *
 * Spec: openspec/changes/swap-time-stretch-to-soundtouch/specs/soundtouch-time-stretcher/spec.md
 *
 * Compared to the archived phase-vocoder stretcher:
 *   - single-pass push() replaces RB's two-pass study() + process()
 *   - TempoSchedule replaces KeyFrameMap (SoundTouch has no setKeyFrameMap; we
 *     drive setTempo() at segment boundaries instead)
 *   - endpoint ±N sample drift (WSOLA discrete hops) handled by caller via
 *     truncate-or-zero-pad against expected length
 */
#pragma once

#include "../Utils/TimeGrid.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// Forward-declare to keep header lean.
namespace soundtouch { class SoundTouch; }

namespace OpenTune {

class SoundTouchStretcher {
public:
    /**
     * Tempo schedule derived from a TimeGridSnapshot.  Each anchor stores the
     * (source_seconds, output_seconds) pair from the corresponding handle; the
     * inter-anchor segment has constant ratio (output_seconds_delta /
     * source_seconds_delta).
     *
     * SoundTouch tempo semantics (RateTransposer.cpp):
     *   tempo > 1.0  → faster / shorter output
     *   tempo < 1.0  → slower / longer output
     *
     * Therefore for our user-facing "ratio" = output_dur / source_dur:
     *   SoundTouch tempo = 1.0 / ratio
     */
    struct TempoSchedule {
        struct Anchor {
            double sourceSeconds;
            double outputSeconds;
        };
        std::vector<Anchor> anchors;       // ascending; endpoints (Clip Start/End) included
        double totalSourceSeconds = 0.0;
        double totalOutputSeconds = 0.0;    // == totalSourceSeconds (端点不变量)
        bool   identity = false;            // every segment ratio == 1.0

        /// Locate the segment index containing sourceSeconds (0-based; clamped to last).
        size_t findSegmentIndex(double sourceSeconds) const noexcept;

        /// Return SoundTouch-tempo at the given source time (= 1.0 / ratio of segment).
        double soundTouchTempoAt(double sourceSeconds) const noexcept;
    };

    /**
     * Construct with vocal-friendly default settings (see spec §"WSOLA 引擎配置"):
     *   sampleRate / channels
     *   rate=pitch=tempo=1.0
     *   USE_AA_FILTER=1 / AA_FILTER_LENGTH=64 / USE_QUICKSEEK=0
     *   SEEKWINDOW_MS=25 / OVERLAP_MS=8
     */
    SoundTouchStretcher(double sampleRate, int channels);
    ~SoundTouchStretcher();

    SoundTouchStretcher(const SoundTouchStretcher&) = delete;
    SoundTouchStretcher& operator=(const SoundTouchStretcher&) = delete;

    /**
     * Convert a TimeGridSnapshot to a TempoSchedule for SoundTouch consumption.
     *
     * Each handle's (source_seconds, output_seconds) becomes an anchor.  The schedule
     * has totalSourceSeconds = anchors.back().sourceSeconds and totalOutputSeconds =
     * anchors.back().outputSeconds.  Endpoints invariant guarantees the two are equal.
     */
    TempoSchedule buildTempoScheduleFromTimeGrid(const TimeGridSnapshot& grid) const;

    /**
     * Begin a new full-rebuild cycle.  Internally:
     *   1. SoundTouch::clear()
     *   2. cache the schedule
     *   3. reset push/pull counters
     *
     * After this returns, the instance is in Pushing phase; caller must call
     * push(...) until isLast=true, then drain via pull().
     */
    void beginRebuild(const TempoSchedule& schedule);

    /**
     * Stream input into SoundTouch.  Internally:
     *   - if input chunk straddles a segment boundary, splits into multiple
     *     setTempo() + putSamples() pairs (each sub-push has constant tempo)
     *   - on isLast=true, calls SoundTouch::flush() to release tail samples
     *
     * Output is collected internally; drain via pull() / available().  Caller
     * may interleave push and pull (no need to wait for full input).
     *
     * @param input         Interleaved (or mono) PCM block of length numSamples
     *                      (per spec: OpenTune is mono, channels=1)
     * @param numSamples    Frame count (NOT sample count for stereo; SoundTouch
     *                      uses frame counts in putSamples).
     * @param isLast        true on the final block of the entire clip
     */
    void push(const float* input, size_t numSamples, bool isLast);

    /**
     * Drain output frames from SoundTouch's internal FIFO.
     *
     * @param output        Output buffer (per-channel interleaved)
     * @param maxSamples    Buffer capacity in frames
     * @return              Number of frames actually written
     */
    size_t pull(float* output, size_t maxSamples);

    /// Number of output frames currently available in SoundTouch's FIFO.
    size_t available() const;

    /**
     * Phase enum for state-machine assertions.
     * Idle: ready for beginRebuild()
     * Pushing: between beginRebuild() and push(...,isLast=true)
     * Drained: after isLast=true and pull() drained available
     */
    enum class Phase : uint8_t { Idle, Pushing, Drained };

    Phase phase() const noexcept { return phase_; }

    /// Expected output frames for the active schedule (only meaningful after
    /// beginRebuild).  Equals totalSourceSeconds × sampleRate (endpoints invariant).
    size_t expectedOutputSamples() const noexcept { return expectedOutputSamples_; }

    /// Returns a packed integer encoding the SoundTouch settings used by the
    /// wrapper at construction.  Layout (bit 0 LSB):
    ///   bits 0-7:   SETTING_USE_AA_FILTER  (0 or 1)
    ///   bits 8-15:  SETTING_USE_QUICKSEEK  (0 or 1)
    ///   bits 16-23: SETTING_SEEKWINDOW_MS  (clamped 0-255)
    ///   bits 24-31: SETTING_OVERLAP_MS     (clamped 0-255)
    /// Exposed for unit tests; not for runtime callers.
    static uint32_t getSoundTouchSettings() noexcept;

private:
    void setTempoIfChanged(double newTempo);

    std::unique_ptr<soundtouch::SoundTouch> impl_;
    double sampleRate_;
    int    channels_;
    Phase  phase_ = Phase::Idle;
    TempoSchedule activeSchedule_;
    size_t totalPushedSamples_ = 0;
    double currentTempo_ = 1.0;
    size_t expectedOutputSamples_ = 0;
};

} // namespace OpenTune
