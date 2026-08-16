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

// 规范化保护范围：clamp 到 [0, sampleCount]、排序、合并重叠/相接，得到互不
// 重叠的保护区序列，供分割点一次性过滤使用。只保留样本边界，不接触 EqSettings。
std::vector<RenderChunkPlanner::ProtectedRange> normalizeProtectedRanges(
    const std::vector<RenderChunkPlanner::ProtectedRange>& ranges,
    int64_t sampleCount)
{
    std::vector<RenderChunkPlanner::ProtectedRange> normalized;
    normalized.reserve(ranges.size());

    for (const auto& range : ranges)
    {
        const int64_t startSample = juce::jlimit<int64_t>(0, sampleCount, range.startSample);
        const int64_t endSampleExclusive = juce::jlimit<int64_t>(0, sampleCount, range.endSampleExclusive);
        if (endSampleExclusive <= startSample)
            continue;
        normalized.push_back({startSample, endSampleExclusive});
    }

    std::sort(normalized.begin(), normalized.end(),
        [](const auto& a, const auto& b) { return a.startSample < b.startSample; });

    std::vector<RenderChunkPlanner::ProtectedRange> merged;
    merged.reserve(normalized.size());
    for (const auto& range : normalized)
    {
        if (merged.empty() || range.startSample >= merged.back().endSampleExclusive)
            merged.push_back(range);
        else
            merged.back().endSampleExclusive =
                std::max(merged.back().endSampleExclusive, range.endSampleExclusive);
    }
    return merged;
}

// 分割点是否落入任一保护区内部（边界点安全：Note 完整落在一侧 chunk）。
// ranges 为 normalizeProtectedRanges 输出（升序、互不重叠）。
bool insideProtectedRange(int64_t sample,
                          const std::vector<RenderChunkPlanner::ProtectedRange>& ranges)
{
    for (const auto& range : ranges)
    {
        if (sample >= range.endSampleExclusive)
            continue;
        if (sample <= range.startSample)
            break;  // 后续 range 起点更大，不可能再命中
        return true;
    }
    return false;
}

// 把落在保护区内部的分割点推进到保护区结束后的最近 hop-aligned 边界。
// 长 active-EQ Note 允许块超过既有最大时长，绝不切断 Note。
int64_t advancePastProtectedRanges(int64_t splitSample,
                                   int hopSize,
                                   const std::vector<RenderChunkPlanner::ProtectedRange>& ranges)
{
    for (const auto& range : ranges)
    {
        if (splitSample >= range.endSampleExclusive)
            continue;
        if (splitSample <= range.startSample)
            break;
        splitSample = ((range.endSampleExclusive + hopSize - 1) / hopSize) * hopSize;
    }
    return splitSample;
}

} // namespace

std::vector<int64_t> RenderChunkPlanner::buildChunkBoundariesFromSilentGaps(
    int64_t sampleCount,
    const std::vector<SilentGap>& silentGaps,
    int hopSize,
    const std::vector<ProtectedRange>& protectedRanges)
{
    std::vector<int64_t> boundaries;
    if (sampleCount <= 0 || hopSize <= 0) {
        return boundaries;
    }

    const auto ranges = normalizeProtectedRanges(protectedRanges, sampleCount);

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

        // silent-gap 分割不得落入 active-EQ Note 保护范围内部（不切断 Note）
        if (insideProtectedRange(splitSample, ranges)) {
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

            // 最大块分割不得落入 active-EQ Note 保护范围内部：推进到 Note 结束
            // 后最近的 hop-aligned 边界；长 Note 允许块超过最大时长，绝不切断。
            splitSample = advancePastProtectedRanges(splitSample, hopSize, ranges);
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
    const std::vector<ProtectedRange>& protectedRanges,
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

    const auto chunkBoundaries = buildChunkBoundariesFromSilentGaps(
        contentSampleCount, silentGaps, hopSize, protectedRanges);
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
