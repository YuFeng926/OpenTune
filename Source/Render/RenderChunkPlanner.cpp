#include "RenderChunkPlanner.h"
#include "../Utils/TimeCoordinate.h"
#include <algorithm>

namespace OpenTune {

namespace {

constexpr double kMaxRenderChunkDurationSeconds = 15.0;
constexpr int64_t kMaxRenderChunkSamples = static_cast<int64_t>(
    kMaxRenderChunkDurationSeconds * TimeCoordinate::kRenderSampleRate);

bool findPreferredHopAlignedBoundarySample(const SilentGap& gap,
                                           int hopSize,
                                           int64_t& outSample)
{
    outSample = 0;
    if (!gap.isValid() || hopSize <= 0) {
        return false;
    }

    const int64_t firstAlignedSample = ((gap.startSample + hopSize - 1) / hopSize) * hopSize;
    const int64_t lastAlignedSample = ((gap.endSampleExclusive - 1) / hopSize) * hopSize;
    if (firstAlignedSample > lastAlignedSample) {
        return false;
    }

    const int64_t midpointSample = gap.midpointSample();
    const int64_t lowerAlignedSample = (midpointSample / hopSize) * hopSize;
    const int64_t upperAlignedSample = lowerAlignedSample + hopSize;

    int64_t preferredSample = lowerAlignedSample;
    if (upperAlignedSample <= lastAlignedSample
        && (midpointSample - lowerAlignedSample) >= (upperAlignedSample - midpointSample)) {
        preferredSample = upperAlignedSample;
    }

    outSample = juce::jlimit(firstAlignedSample, lastAlignedSample, preferredSample);
    return true;
}

} // namespace

std::vector<int64_t> RenderChunkPlanner::buildChunkBoundariesFromSilentGaps(
    int64_t sampleCount,
    const std::vector<SilentGap>& silentGaps,
    int hopSize)
{
    std::vector<int64_t> boundaries;
    if (sampleCount <= 0 || hopSize <= 0) {
        return boundaries;
    }

    std::vector<int64_t> anchorBoundaries;
    anchorBoundaries.reserve(silentGaps.size() + 2);
    anchorBoundaries.push_back(0);

    for (const auto& gap : silentGaps) {
        int64_t splitSample = 0;
        if (!findPreferredHopAlignedBoundarySample(gap, hopSize, splitSample)) {
            continue;
        }

        if (splitSample <= anchorBoundaries.back() || splitSample >= sampleCount) {
            continue;
        }

        anchorBoundaries.push_back(splitSample);
    }

    if (anchorBoundaries.back() != sampleCount) {
        anchorBoundaries.push_back(sampleCount);
    }

    std::sort(anchorBoundaries.begin(), anchorBoundaries.end());
    anchorBoundaries.erase(std::unique(anchorBoundaries.begin(), anchorBoundaries.end()), anchorBoundaries.end());

    boundaries.reserve(anchorBoundaries.size() + static_cast<size_t>(sampleCount / kMaxRenderChunkSamples) + 1);
    boundaries.push_back(anchorBoundaries.front());

    for (size_t i = 0; i + 1 < anchorBoundaries.size(); ++i) {
        const int64_t anchorStart = anchorBoundaries[i];
        const int64_t anchorEnd = anchorBoundaries[i + 1];

        int64_t chunkStart = anchorStart;
        while ((anchorEnd - chunkStart) > kMaxRenderChunkSamples) {
            int64_t splitSample = ((chunkStart + kMaxRenderChunkSamples) / hopSize) * hopSize;
            if (splitSample <= chunkStart) {
                splitSample = ((chunkStart / hopSize) + 1) * static_cast<int64_t>(hopSize);
            }

            if (splitSample >= anchorEnd) {
                break;
            }

            boundaries.push_back(splitSample);
            chunkStart = splitSample;
        }

        if (boundaries.back() != anchorEnd) {
            boundaries.push_back(anchorEnd);
        }
    }

    return boundaries;
}

std::vector<RenderChunkPlanner::ChunkRange> RenderChunkPlanner::selectChunksIntersectingRange(
    int64_t contentSampleCount,
    const std::vector<SilentGap>& silentGaps,
    int64_t requestStartSample,
    int64_t requestEndSampleExclusive,
    int hopSize)
{
    std::vector<ChunkRange> chunks;
    if (contentSampleCount <= 0 || requestEndSampleExclusive <= requestStartSample || hopSize <= 0) {
        return chunks;
    }

    const int64_t clampedRequestStart = juce::jlimit<int64_t>(0, contentSampleCount, requestStartSample);
    const int64_t clampedRequestEnd = juce::jlimit<int64_t>(0, contentSampleCount, requestEndSampleExclusive);
    if (clampedRequestEnd <= clampedRequestStart) {
        return chunks;
    }

    const auto chunkBoundaries = buildChunkBoundariesFromSilentGaps(contentSampleCount, silentGaps, hopSize);
    if (chunkBoundaries.size() < 2) {
        chunks.push_back({clampedRequestStart, clampedRequestEnd});
        return chunks;
    }

    chunks.reserve(chunkBoundaries.size() - 1);
    for (size_t i = 0; i + 1 < chunkBoundaries.size(); ++i) {
        const int64_t chunkStartSample = chunkBoundaries[i];
        const int64_t chunkEndSampleExclusive = chunkBoundaries[i + 1];
        const int64_t overlapStart = std::max(clampedRequestStart, chunkStartSample);
        const int64_t overlapEnd = std::min(clampedRequestEnd, chunkEndSampleExclusive);
        if (overlapEnd <= overlapStart) {
            continue;
        }

        chunks.push_back({chunkStartSample, chunkEndSampleExclusive});
    }

    return chunks;
}

} // namespace OpenTune
