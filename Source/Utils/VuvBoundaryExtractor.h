#pragma once

#include <vector>

namespace OpenTune {

/**
 * VuvBoundaryExtractor - Derives VUV segments from OriginalF0
 *
 * Extracts voiced/unvoiced segments from F0 curve (f0>0 = voiced, f0==0 = unvoiced),
 * merges short unvoiced gaps, and produces known_durations for GAME alignment.
 */
class VuvBoundaryExtractor {
public:
    struct Segment {
        double start;    // seconds
        double end;      // seconds
        bool voiced;
    };

    /**
     * Extract VUV segments from OriginalF0, merging short gaps.
     * @param f0 F0 values (0 = unvoiced)
     * @param hopSize RMVPE hop size in samples (typically 160)
     * @param sampleRate F0 extraction sample rate (typically 16000)
     * @param minGapMs Minimum unvoiced gap duration; shorter gaps merged into voiced (default 50ms)
     * @return VUV segments covering [0, totalDuration]
     */
    static std::vector<Segment> extractSegments(
        const std::vector<float>& f0, int hopSize, int sampleRate, double minGapMs = 50.0);

    /**
     * Convert VUV segments to known_durations array for GAME.
     * Each element is the duration of one segment in seconds.
     * Sum must equal totalDuration.
     * @param segments VUV segments from extractSegments()
     * @param totalDuration Total audio duration in seconds (used to clamp last segment)
     * @return Consecutive durations in seconds
     */
    static std::vector<float> segmentsToDurations(
        const std::vector<Segment>& segments, double totalDuration);
};

} // namespace OpenTune
