#pragma once

#include "../Utils/SilentGapDetector.h"
#include <vector>
#include <cstdint>

namespace OpenTune {

/**
 * RenderChunkPlanner — 根据 silent gaps 规划 render chunk boundary。
 * 
 * 纯函数工具类，不持有状态。
 */
class RenderChunkPlanner
{
public:
    // 唯一渲染 hop：planner 边界对齐、ContentRenderService 计划与 Runtime freeze
    // 共用。独立于 GAME/F0 提取 frame hop 与 Vocoder 内部 hop，是 Stage1 渲染的
    // 固定常数，无第二来源。
    static constexpr int kRenderHopSize = 512;

    struct ChunkRange
    {
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
    };

    // 纯样本域保护范围：active-EQ Note 的秒域边界经 TimeCoordinate floor/ceil
    // 转换后的覆盖区间。任何 silent-gap 分割与最大块分割都不得落入其内部，
    // 保证 Note 完整进 EQ；长 active-EQ Note 允许块超过既有最大时长。
    struct ProtectedRange
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
     * @param protectedRanges active-EQ Note 保护范围；分割不得落入其内部。
     *        默认空：调用方无 Note 保护语义（如 GAME note 生成）。
     * @return 按升序排列的 chunk boundary 位置（sample index）
     */
    static std::vector<int64_t> buildChunkBoundariesFromSilentGaps(
        int64_t sampleCount,
        const std::vector<SilentGap>& silentGaps,
        int hopSize,
        const std::vector<ProtectedRange>& protectedRanges = {});

    static std::vector<ChunkRange> selectChunksIntersectingRange(
        int64_t contentSampleCount,
        const std::vector<SilentGap>& silentGaps,
        const std::vector<ProtectedRange>& protectedRanges,
        int64_t requestStartSample,
        int64_t requestEndSampleExclusive,
        int hopSize);
};

} // namespace OpenTune
