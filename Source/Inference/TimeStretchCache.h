/**
 * TimeStretchCache - clip-wide single-entry cache for Stage 2 time-stretch output.
 *
 * Unlike PitchCache (chunk-wise), TimeStretchCache stores one entry per content
 * key, holding the entire clip's time-stretched PCM. Stage 2 depends on Stage 1
 * audio and TimeGrid, and Stage 1 audio depends on both pitch curve and
 * pitch-shift settings, so cache identity is the explicit tuple:
 * (pitchRevision, pitchShiftRevision, timeGridRevision).
 *
 * Hit conditions:
 *   - ContentKey matches
 *   - pitchRevision matches the current pitch-curve revision
 *   - pitchShiftRevision matches the current pitch-shift settings revision
 *   - timeGridRevision matches the current TimeGrid revision
 *
 * processBlock reads slices via sliceForOutputRange(...).
 * Spec: openspec/changes/vocal-time-stretch/specs/two-stage-render-pipeline/spec.md
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
     * One entry per content key.
     */
    struct Entry {
        std::vector<float> audio;        // Stage 2 output, full clip, full-band PCM
        uint64_t pitchRevision = 0;
        uint64_t pitchShiftRevision = 0;
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
     * Store the full-clip stretched PCM for content at a specific
     * (pitchRevision, pitchShiftRevision, timeGridRevision) tuple with
     * build-generation token for stale-output rejection. Replaces any prior entry.
     */
    void store(ContentKey key,
               std::vector<float> audio,
               uint64_t pitchRevision,
               uint64_t pitchShiftRevision,
               uint64_t timeGridRevision,
               double sampleRate,
               uint32_t buildGeneration);

    /**
     * Capture the current invalidation generation for this content key.
     * Caller (worker) holds this and passes it to store(); if the generation
     * has changed by the time store() runs, the build output is discarded.
     */
    uint32_t beginBuild(ContentKey key) const;

    /**
     * Check if entry for `key` matches the current Stage 2 dependency tuple.
     */
    bool hit(ContentKey key,
             uint64_t pitchRevision,
             uint64_t pitchShiftRevision,
             uint64_t timeGridRevision) const;

    /**
     * Read a slice of the cached output by output time range.
     *
     * @param key                      Content identity
     * @param outputStartSeconds       Start of slice in output time
     * @param destination              Destination buffer
     * @param destinationStartSample   Where to write in destination
     * @param numSamples               Sample count to read
     * @param targetSampleRate         If different from cached SR, linear-interp resample
     * @return                         Number of samples written; 0 if cache miss/range out of bounds
     */
    int sliceForOutputRange(ContentKey key,
                            uint64_t pitchRevision,
                            uint64_t pitchShiftRevision,
                            uint64_t timeGridRevision,
                            double outputStartSeconds,
                            juce::AudioBuffer<float>& destination,
                            int destinationStartSample,
                            int numSamples,
                            int targetSampleRate) const;

    /**
     * Mark entry as stale.
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

    // Atomic snapshot for lock-free readers. Published atomically after each
    // write to entries_ so that sliceForOutputRange never needs a lock.
    mutable std::shared_ptr<const std::map<ContentKey, std::shared_ptr<Entry>>> readerMap_;

    // Retired snapshots for writer-side delayed destruction.
    // Swept when use_count()==1 so free/malloc never hits audio thread.
    mutable std::vector<std::shared_ptr<const std::map<ContentKey, std::shared_ptr<Entry>>>> retiredSnapshots_;

    // Per-content invalidation generation counter.
    // Bumped by invalidate(), checked by store() to reject stale worker output.
    mutable std::map<ContentKey, uint32_t> invalidationGen_;
};

} // namespace OpenTune
