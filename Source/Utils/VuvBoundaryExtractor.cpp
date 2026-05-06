#include "VuvBoundaryExtractor.h"
#include <algorithm>
#include <cmath>

namespace OpenTune {

std::vector<VuvBoundaryExtractor::Segment> VuvBoundaryExtractor::extractSegments(
    const std::vector<float>& f0, int hopSize, int sampleRate, double minGapMs)
{
    if (f0.empty() || hopSize <= 0 || sampleRate <= 0)
        return {};

    const double frameDuration = static_cast<double>(hopSize) / static_cast<double>(sampleRate);
    const double totalDuration = static_cast<double>(f0.size()) * frameDuration;

    // Step 1: Build raw segments
    std::vector<Segment> raw;
    bool currentVoiced = (f0[0] > 0.0f);
    double segStart = 0.0;

    for (size_t i = 1; i < f0.size(); ++i) {
        bool frameVoiced = (f0[i] > 0.0f);
        if (frameVoiced != currentVoiced) {
            double boundary = static_cast<double>(i) * frameDuration;
            raw.push_back({segStart, boundary, currentVoiced});
            segStart = boundary;
            currentVoiced = frameVoiced;
        }
    }
    raw.push_back({segStart, totalDuration, currentVoiced});

    if (raw.empty())
        return {};

    // Step 2: Two-pass merge of short unvoiced gaps.
    // Strategy: a short unvoiced gap (< minGapMs) is absorbed into an adjacent voiced segment.
    // Priority: merge into left voiced neighbor; if none, merge into right voiced neighbor.
    const double minGapSec = minGapMs / 1000.0;

    // Mark short unvoiced segments for absorption
    std::vector<bool> absorb(raw.size(), false);
    for (size_t i = 0; i < raw.size(); ++i) {
        if (!raw[i].voiced && (raw[i].end - raw[i].start) < minGapSec) {
            // Check left neighbor
            bool leftVoiced = (i > 0 && raw[i - 1].voiced);
            // Check right neighbor
            bool rightVoiced = (i + 1 < raw.size() && raw[i + 1].voiced);
            if (leftVoiced || rightVoiced) {
                absorb[i] = true;
            }
        }
    }

    // Build merged list
    std::vector<Segment> merged;
    merged.reserve(raw.size());

    for (size_t i = 0; i < raw.size(); ++i) {
        if (absorb[i]) {
            // Absorbed into adjacent voiced: extend previous or next
            if (!merged.empty() && merged.back().voiced) {
                merged.back().end = raw[i].end;
            } else {
                // No left voiced — will be absorbed into right when right is processed
                // Push temporarily; next voiced will merge it
                merged.push_back({raw[i].start, raw[i].end, true}); // mark as voiced
            }
        } else if (raw[i].voiced && !merged.empty() && merged.back().voiced) {
            // Consecutive voiced (after gap absorption) — extend
            merged.back().end = raw[i].end;
        } else {
            merged.push_back(raw[i]);
        }
    }

    return merged;
}

std::vector<float> VuvBoundaryExtractor::segmentsToDurations(
    const std::vector<Segment>& segments, double totalDuration)
{
    if (segments.empty())
        return {static_cast<float>(std::max(totalDuration, 1e-6))};

    std::vector<float> durations;
    durations.reserve(segments.size());

    constexpr float kMinDuration = 1e-4f; // minimum segment duration to avoid zero/negative

    for (size_t i = 0; i < segments.size(); ++i) {
        float dur = static_cast<float>(segments[i].end - segments[i].start);
        durations.push_back(std::max(dur, kMinDuration));
    }

    // Normalize sum to equal totalDuration exactly
    double sum = 0.0;
    for (auto d : durations) sum += d;
    if (sum > 0.0 && std::abs(sum - totalDuration) > 1e-6) {
        const double scale = totalDuration / sum;
        for (auto& d : durations) {
            d = std::max(static_cast<float>(d * scale), kMinDuration);
        }
    }

    return durations;
}

} // namespace OpenTune
