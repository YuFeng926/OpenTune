#pragma once

#include "TimeGrid.h"
#include <memory>
#include <vector>

namespace OpenTune {

std::shared_ptr<const TimeGridSnapshot> planTimeGridDrag(
    const std::shared_ptr<const TimeGridSnapshot>& original,
    uint64_t draggedHandleId,
    double dragStartOutputSeconds,
    double newOutputSeconds,
    uint64_t selectedHandleId,
    const std::vector<uint64_t>& additionalSelectedIds,
    bool snapDisabled,
    bool& validationRejected);

enum class TimeGridInsertStatus {
    Inserted,
    InvalidTime,
    InvalidGrid,
    OutputSpacing,
    SourceSpacing,
    OutsideRange,
    ValidationRejected
};

struct TimeGridInsertResult {
    TimeGridInsertStatus status{TimeGridInsertStatus::InvalidGrid};
    std::shared_ptr<const TimeGridSnapshot> snapshot;
    uint64_t insertedHandleId{0};
};

TimeGridInsertResult planTimeGridInsert(
    const std::shared_ptr<const TimeGridSnapshot>& original,
    double clickedOutputSeconds);

enum class TimeGridDeleteStatus {
    Deleted,
    NotFound,
    Endpoint,
    ValidationRejected
};

struct TimeGridDeleteResult {
    TimeGridDeleteStatus status{TimeGridDeleteStatus::NotFound};
    std::shared_ptr<const TimeGridSnapshot> snapshot;
};

TimeGridDeleteResult planTimeGridDelete(
    const std::shared_ptr<const TimeGridSnapshot>& original,
    uint64_t selectedHandleId);

/**
 * 输出时间轴上真正因 TimeGrid 编辑而改变的区间。
 *
 * 拖一个 handle 只改变它两侧那个区间的时间映射，其余段的网格逐字节相同。
 * 但烘焙后 Stage 1 的产物依赖整条映射，contentRevision 一旦推进，
 * RenderCache 会把"请求范围"内的所有 chunk 判为过期——所以请求范围必须
 * 收窄到这几次编辑实际覆盖的区间，否则拖一次手柄就重渲整曲。
 *
 * 判定方式：按 handle id 配对两个快照。只有 output_seconds 变化的 handle
 * 才算改动，改动 handle 所夹的相邻区间（前一个 handle 到后一个 handle）
 * 即为受影响范围，两端各自向外扩一个区间以覆盖线性插值段。
 *
 * 返回值已 clamp 到 [0, duration]；两个快照无实质差异时返回 false，
 * 调用方应据此跳过重渲。
 *
 * @param before          编辑前快照
 * @param after           编辑后快照
 * @param outStartSeconds [out] 受影响输出区间起点
 * @param outEndSeconds   [out] 受影响输出区间终点
 */
bool timeGridAffectedOutputRange(
    const std::shared_ptr<const TimeGridSnapshot>& before,
    const std::shared_ptr<const TimeGridSnapshot>& after,
    double& outStartSeconds,
    double& outEndSeconds);

/** Render range on the output timeline, aligned with chunk boundaries. */
struct ContentMutationRenderRange
{
    double startSeconds{0.0};
    double endSeconds{0.0};
};

/** Maps a source-timeline edit range onto the output timeline used by Stage 1
    chunks. Identity grids return the range unchanged; degenerate or invalid
    input falls back to the full track. */
ContentMutationRenderRange mapContentMutationRangeToOutput(
    const std::shared_ptr<const TimeGridSnapshot>& grid,
    double sourceStartSeconds,
    double sourceEndSeconds,
    double durationSeconds);

} // namespace OpenTune
