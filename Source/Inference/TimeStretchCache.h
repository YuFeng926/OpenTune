/**
 * TimeStretchCache — clip-wide single-entry cache for Stage 2 time-stretch output.
 *
 * Unlike PitchCache (chunk-wise), TimeStretchCache stores one entry per
 * materialization, holding the entire clip's time-stretched PCM.  The
 * granularity choice is forced by Rubber Band Offline mode: any edit
 * (PitchCurve OR TimeGrid revision change) requires reset → setKeyFrameMap →
 * full study → full process — splitting per-chunk would not save work, so
 * a single entry is simpler and equally efficient.
 *
 * Hit conditions:
 *   - materializationId matches
 *   - pitchRevision matches the current Stage-1 revision
 *   - timeGridRevision matches the current TimeGrid revision
 *
 * processBlock reads slices via sliceForOutputRange(...).
 * Spec: openspec/changes/vocal-time-stretch/specs/two-stage-render-pipeline/spec.md
 *       (§"双级缓存 — PitchCache chunk-wise + TimeStretchCache clip-wide")
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "../Content/ContentKey.h"

namespace OpenTune {

class TimeStretchCache {
public:
    /**
     * One entry per materialization.
     */
    struct Entry {
        std::vector<float> audio;        // Stage 2 output, full clip, full-band PCM
        uint64_t pitchRevision = 0;
        uint64_t timeGridRevision = 0;
        double sampleRate = 44100.0;     // typically TimeCoordinate::kRenderSampleRate
        bool published = false;          // false until first store
    };

    /**
     * Stats snapshot for diagnostics.
     */
    struct Stats {
        int materializationCount = 0;
        size_t totalBytes = 0;
        int publishedCount = 0;
    };

    TimeStretchCache();
    ~TimeStretchCache();

    /**
     * Store the full-clip stretched PCM for a materialization at a specific
     * (pitchRevision, timeGridRevision) tuple with build-generation token
     * for stale-output rejection.  Replaces any prior entry.
     */
    void store(ContentKey key,
               std::vector<float> audio,
               uint64_t pitchRevision,
               uint64_t timeGridRevision,
               double sampleRate,
               uint32_t buildGeneration);

    /**
     * Capture the current invalidation generation for this materialization.
     * Caller (worker) holds this and passes it to store(); if the generation
     * has changed by the time store() runs, the build output is discarded.
     */
    uint32_t beginBuild(ContentKey key) const;

    /**
     * Check if entry for `materializationId` matches the current revisions.
     */
    bool hit(ContentKey key,
             uint64_t pitchRevision,
             uint64_t timeGridRevision) const;

    /**
     * Read a slice of the cached output by output time range.
     *
     * @param materializationId        Materialization
     * @param outputStartSeconds       Start of slice in OUTPUT time
     * @param destination              Destination buffer
     * @param destinationStartSample   Where to write in destination
     * @param numSamples               Sample count to read
     * @param targetSampleRate         If different from cached SR, linear-interp resample
     * @return                         Number of samples written; 0 if cache miss / range out of bounds
     */
    int sliceForOutputRange(ContentKey key,
                            uint64_t pitchRevision,
                            uint64_t timeGridRevision,
                            double outputStartSeconds,
                            juce::AudioBuffer<float>& destination,
                            int destinationStartSample,
                            int numSamples,
                            int targetSampleRate) const;

    /**
     * Mark entry as stale (revisions cleared, audio cleared).
     */
    void invalidate(ContentKey key);

    /**
     * Drop all entries.
     */
    void clear();

    /**
     * Diagnostics.
     */
    Stats getStats() const;

private:
    mutable juce::SpinLock lock_;
    std::map<ContentKey, std::shared_ptr<Entry>> entries_;

    // Atomic snapshot for lock-free readers.  Published atomically after each
    // write to entries_ so that sliceForOutputRange never needs a lock.
    mutable std::shared_ptr<const std::map<ContentKey, std::shared_ptr<Entry>>> readerMap_;

    // Retired snapshots for writer-side delayed destruction.
    // Swept when use_count()==1 — guarantees free/malloc never hits audio thread.
    mutable std::vector<std::shared_ptr<const std::map<ContentKey, std::shared_ptr<Entry>>>> retiredSnapshots_;

    // Per-materialization invalidation generation counter.
    // Bumped by invalidate(), checked by store() to reject stale worker output.
    mutable std::map<ContentKey, uint32_t> invalidationGen_;
};

} // namespace OpenTune
