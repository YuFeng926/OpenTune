#pragma once

#include "../Utils/SilentGapDetector.h"
#include <vector>
#include <cstdint>

namespace OpenTune {

/**
 * RenderChunkPlanner — 根据 silent gaps 规划 render chunk boundary。
 * 
 * 纯函数工具类，不持有状态。
 * 
 * Phase 0: 提取自 RenderChunkPlanner::buildChunkBoundariesFromSilentGaps
 *          (content owner.cpp:820-880)
 */
class RenderChunkPlanner
{
public:
    struct ChunkRange
    {
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
    };

    /**
     * 根据 silent gaps 和 hop size 构建 chunk boundaries。
     * 
     * @param sampleCount 音频总样本数
     * @param silentGaps 静音段列表
     * @param hopSize 渲染 hop size（用于对齐边界）
     * @return 按升序排列的 chunk boundary 位置（sample index）
     */
    static std::vector<int64_t> buildChunkBoundariesFromSilentGaps(
        int64_t sampleCount,
        const std::vector<SilentGap>& silentGaps,
        int hopSize);

    static std::vector<ChunkRange> selectChunksIntersectingRange(
        int64_t contentSampleCount,
        const std::vector<SilentGap>& silentGaps,
        int64_t requestStartSample,
        int64_t requestEndSampleExclusive,
        int hopSize);
};

} // namespace OpenTune
