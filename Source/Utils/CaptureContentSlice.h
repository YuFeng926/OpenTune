#pragma once

#include "../Utils/PitchCurve.h"
#include "../Utils/Note.h"
#include "../Utils/F0Timeline.h"
#include "../Content/ContentState.h"

#include <memory>
#include <optional>
#include <vector>

namespace OpenTune {

/// snapshot 版 seconds → frame 区间换算（委托 F0Timeline 的 5 参版本）。
inline std::optional<F0FrameRange> f0FrameRangeForSeconds(
    const PitchCurveSnapshot& snapshot,
    double startSeconds,
    double endSeconds)
{
    return f0FrameRangeForSeconds(snapshot.getHopSize(),
                                  snapshot.getSampleRate(),
                                  static_cast<int>(snapshot.getOriginalF0().size()),
                                  startSeconds,
                                  endSeconds);
}

/// notes 按本地秒区间 [startSeconds, endSeconds) 相交裁剪并平移为 0 起点。
std::vector<Note> sliceNotesToLocalRange(const std::vector<Note>& notes,
                                         double startSeconds,
                                         double endSeconds);

/// frame index 版：调用方已持有精确 frame 区间时使用，避免 seconds 反算取整
/// 造成边界帧同时落入相邻两个半开区间。
std::shared_ptr<PitchCurve> slicePitchCurveToFrameRange(
    const std::shared_ptr<const PitchCurveSnapshot>& pitchCurve,
    int startFrame,
    int endFrameExclusive);

/// seconds 版：按端点秒数求覆盖 frame range 后委托 frame index 版。
std::shared_ptr<PitchCurve> slicePitchCurveToLocalRange(
    const std::shared_ptr<const PitchCurveSnapshot>& pitchCurve,
    double startSeconds,
    double endSeconds);

/// 按 [offsetSamples, offsetSamples+count) 裁剪一份 ContentState：
/// 音频/notes/silentGaps/pitchCurve/volumeEnvelope 同步裁剪并平移为 0 起点；
/// detectedKey/pitchShiftSettings/noteTopologyInitialized/nativeSource 原样保留；
/// sourceWindow 从父权威起点加本次操作秒偏移更新，首尾均为绝对秒；
/// firstCanonicalSample 只由切片绝对起点投影一次，不累加已取整的 canonical 增量；
/// timeGrid 以切片权威时长重建 identity；
/// revision 字段由调用方（owner 的 apply 方法）重新生成。
ContentState sliceContentState(const ContentState& source,
                               int64_t offsetSamples,
                               int64_t count,
                               double sampleRate);

} // namespace OpenTune
