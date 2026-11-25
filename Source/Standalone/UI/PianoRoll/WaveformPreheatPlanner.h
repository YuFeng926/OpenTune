#pragma once

#include "../../../Utils/ContentTimelineProjection.h"
#include "../../../Content/EditableContentSnapshot.h"
#include "PianoRollTimeMap.h"
#include "PianoRollRenderer.h"
#include "../ViewMapper.h"
#include "../WaveformMipmap.h"
#include "../../../Utils/TimeCoordinate.h"
#include <cstdint>
#include <algorithm>

namespace OpenTune {

/// 单个 placement 的预热计划：与 drawWaveform 相同的整数列裁剪与 xToSource
/// 投影。预热源区间与渲染器逐列实际查询区间一致——placement 时间界先取整
/// 到屏幕列（渲染器 timeToX 同款 llround），损伤扩边 2px 后裁回内容视口；
/// 层级数按每列实际查询消费 ceil(end)-floor(start) 的峰值取值，
/// 置债窗口用首末列的查询端点。
struct WaveformPreheatPlan
{
    int64_t startSample = 0;
    int64_t endSampleExclusive = 0;
    int levelCount = 0;
    bool valid = false;
};

inline WaveformPreheatPlan planWaveformPreheat(
    const ContentTimelineProjection& projection,
    const TimeGridSnapshot& timeGrid,
    double sampleRate,
    double visibleStartSeconds,
    double pixelsPerSecond,
    int contentStartX,
    int viewportWidth)
{
    if (!(sampleRate > 0.0) || !(pixelsPerSecond > 0.0) || viewportWidth <= 0)
        return {};

    ViewMapper coords;
    coords.visibleStartSeconds = visibleStartSeconds;
    coords.pixelsPerSecond = pixelsPerSecond;
    coords.contentStartX = contentStartX;
    coords.contentWidth = viewportWidth;
    const PianoRollTimeMap map(projection, timeGrid, coords);

    // 列界与渲染器 drawWaveform 同源（单一实现）：placement 整数列界硬裁进
    // 内容视口，无外扩（预热建模全量栅格，rasterBounds 为空）。
    const int segStartX = coords.timeToX(projection.timelineStartSeconds);
    const int segEndX = coords.timeToX(projection.timelineEndSeconds());
    const auto [drawStartX, drawEndX] = PianoRollRenderer::waveformColumnBounds(
        true, {}, contentStartX + viewportWidth, contentStartX, segStartX, segEndX);
    if (drawEndX <= drawStartX)
        return {};

    WaveformPreheatPlan plan;
    for (int x = drawStartX; x < drawEndX; ++x)
    {
        const double sourceStart = map.xToSource(x);
        const double sourceEnd = map.xToSource(x + 1);
        const int64_t queryStart = TimeCoordinate::secondsToSamplesFloor(sourceStart, sampleRate);
        const int64_t queryEnd = TimeCoordinate::secondsToSamplesCeil(sourceEnd, sampleRate);
        // 退化列（TimeGrid 压缩到零跨度或钳制后区间为空）：渲染器查询为空，
        // 无样本消费，不进入端点与层级统计。
        if (queryEnd <= queryStart)
            continue;
        if (!plan.valid)
            plan.startSample = queryStart;
        plan.endSampleExclusive = queryEnd;
        // 每列实际查询消费 ceil(end)-floor(start)（渲染器逐像素同款换算），
        // 层级取列峰值：127.5 样本/列的首列实际查询 [0,128) 消费 128。
        int levelCount = 0;
        while (levelCount < WaveformMipmap::kNumLevels
               && WaveformMipmap::kSamplesPerPeak[levelCount] <= queryEnd - queryStart)
            ++levelCount;
        plan.levelCount = std::max(plan.levelCount, levelCount);
        plan.valid = true;
    }
    if (!plan.valid)
        return {};
    return plan;
}

} // namespace OpenTune
