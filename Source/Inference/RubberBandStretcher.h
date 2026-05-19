/**
 * RubberBandStretcher — Wrapper around RubberBand::RubberBandStretcher (Offline mode).
 *
 * Offline mode is mandatory for v7 because:
 *   - setKeyFrameMap is forbidden in RealTime mode (RB API hard constraint)
 *   - chunkRenderWorker is a background thread, not realtime audio
 *
 * Lifecycle (per-materialization persistent instance):
 *   - Constructed once when the materialization is committed
 *   - Destroyed when the materialization is destroyed
 *   - On every TimeGrid OR PitchCurve revision change → rebuildForRevisions(...)
 *     fully resets (reset → setTimeRatio → setKeyFrameMap → study + process pass)
 *
 * Strict API order (RB v3+ hard constraints):
 *   1. reset()                 — clears study state + keyframe map
 *   2. setTimeRatio(...)       — must precede setKeyFrameMap
 *   3. setKeyFrameMap(...)     — must precede first process()
 *   4. study(chunks, isLast)   — feed entire input first
 *   5. process(chunks, isLast) — generate output, retrieve via available()
 *
 * State machine prevents call-order violations.
 *
 * Spec: openspec/changes/vocal-time-stretch/specs/rubberband-time-stretcher/spec.md
 * Design: research/p0_time_stretch/DESIGN.md v7 §5.5
 */
#pragma once

#include "../Utils/TimeGrid.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

// Forward-declare RB types to keep this header lean.
namespace RubberBand { class RubberBandStretcher; }

namespace OpenTune {

class RubberBandStretcher {
public:
    /**
     * Construct with Offline mode + R3 + formant + transients-crisp + pitch-high-quality.
     *
     * @param sampleRate    Audio sample rate (typically 44100)
     * @param channels      Channel count (1 for OpenTune mono materialization)
     */
    RubberBandStretcher(double sampleRate, int channels);
    ~RubberBandStretcher();

    RubberBandStretcher(const RubberBandStretcher&) = delete;
    RubberBandStretcher& operator=(const RubberBandStretcher&) = delete;

    /**
     * Convert a TimeGridSnapshot to RB's keyframe map format.
     *
     * Each handle's (source_seconds, output_seconds) is converted to
     * (round(source_seconds * sampleRate), round(output_seconds * sampleRate))
     * sample-frame indices. The map is monotonic-increasing in both keys and
     * values (TimeGrid invariant guarantees this).
     */
    std::map<size_t, size_t> buildKeyframesFromTimeGrid(const TimeGridSnapshot& grid) const;

    /**
     * Begin a new full-rebuild cycle.
     *
     * Strict call order enforced:
     *   1. reset()
     *   2. setTimeRatio(timeRatio)         (1.0 for v7 locked endpoints)
     *   3. setKeyFrameMap(keyframes)
     *
     * After this returns, the instance is in Studying phase: caller must call
     * study(...) until isLast=true, then process(...) until isLast=true.
     *
     * @param timeRatio   Total ratio = total_out / total_src. With locked endpoints == 1.0.
     * @param keyframes   Output of buildKeyframesFromTimeGrid (or any custom mapping).
     */
    void beginRebuild(double timeRatio, const std::map<size_t, size_t>& keyframes);

    /**
     * Pass 1 — Study. Provide the entire input audio in chunks (in source order).
     * Set isLast=true on the final chunk to signal end of study pass.
     *
     * After isLast=true, transition to Processing phase. Calling study again
     * (until next beginRebuild) is a no-op.
     *
     * @param input         Mono PCM block of length numSamples
     * @param numSamples    Block sample count
     * @param isLast        true on the final block of the entire clip
     */
    void study(const float* input, size_t numSamples, bool isLast);

    /**
     * Pass 2 — Process. Provide the same input chunks (in source order) again.
     * After each call, retrieve output via available() / retrieve(...).
     *
     * @param input         Mono PCM block of length numSamples
     * @param numSamples    Block sample count
     * @param isLast        true on the final block
     */
    void process(const float* input, size_t numSamples, bool isLast);

    /**
     * Number of output samples currently available.
     */
    size_t available() const;

    /**
     * Retrieve up to maxSamples output frames into output buffer.
     *
     * @param output        Mono PCM buffer
     * @param maxSamples    Buffer capacity
     * @return              Number of frames actually written
     */
    size_t retrieve(float* output, size_t maxSamples);

    /**
     * Phase enum for state-machine assertions.
     * Idle: ready for beginRebuild()
     * Studying: between beginRebuild() and study(...,isLast=true)
     * Processing: between study(isLast=true) and process(isLast=true) + drained retrieve
     */
    enum class Phase : uint8_t { Idle, Studying, Processing };

    Phase phase() const noexcept { return phase_; }

    /// Return the bitmask of RubberBand options used at construction.
    /// Exposed for unit tests to verify vocal-tuned configuration
    /// (tune-rubberband-options-for-vocal). Not intended for runtime callers.
    static int getRubberBandOptions() noexcept;

    /**
     * Total samples expected at output (study pass result).
     * Only meaningful in Processing phase.
     */
    size_t expectedOutputSamples() const noexcept { return expectedOutputSamples_; }

private:
    std::unique_ptr<RubberBand::RubberBandStretcher> impl_;
    double sampleRate_;
    int    channels_;
    Phase  phase_ = Phase::Idle;
    size_t totalInputSamplesStudied_ = 0;
    size_t totalInputSamplesProcessed_ = 0;
    size_t expectedOutputSamples_ = 0;
};

} // namespace OpenTune
