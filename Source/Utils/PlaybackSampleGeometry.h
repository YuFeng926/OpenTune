#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

/*
 * 播放专用固定映射几何。
 *
 * 时间字段保持权威原值；本文件只产出操作坐标：
 *   D = Q((P - M) * H)
 *   A = Q(P * H)
 *   Z = Q(E * H)
 * host 播放区间为 [A, Z)，所需 prepared 区间为 [A - D, Z - D)。
 * 回调只做半开区间求交与地址平移，不经过秒数回写。
 */
namespace OpenTune::PlaybackSampleGeometry {

/**
 * 播放专用最近点投影 Q。
 *
 * 半样本 tie 选择较大整数（朝向 +infinity），因此对整数 n 满足
 * Q(x + n) == Q(x) + n。实现按整数部分与小数部分拆分，避免
 * floor(x + 0.5) 在大数值与 0.49999999999999994 附近的精度问题。
 * 输入为有限操作坐标，取整结果在 int64 范围内；返回值只作为操作坐标使用。
 */
inline int64_t projectToSample(double value) noexcept
{
    const double whole = std::floor(value);
    const double fraction = value - whole;
    const int64_t base = static_cast<int64_t>(whole);
    return fraction >= 0.5 ? base + 1 : base;
}

/** 半开样本区间 [begin, end)。 */
struct SampleSpan
{
    int64_t begin{0};
    int64_t end{0};

    bool empty() const noexcept { return end <= begin; }
};

/** int64 半开区间求交；无交集时返回空 span。 */
inline SampleSpan intersect(const SampleSpan& lhs, const SampleSpan& rhs) noexcept
{
    return { std::max(lhs.begin, rhs.begin), std::min(lhs.end, rhs.end) };
}

/**
 * 发布时冻结的固定映射。
 *
 * H = playbackRate，P = playbackStartSeconds，E = playbackEndSeconds，
 * M = sourceStartSeconds：
 *   offsetSamples       = D = Q((P - M) * H)
 *   playbackBeginSample = A = Q(P * H)
 *   playbackEndSample   = Z = Q(E * H)
 */
struct Placement
{
    double playbackRate{0.0};        // H
    int64_t offsetSamples{0};        // D
    int64_t playbackBeginSample{0};  // A
    int64_t playbackEndSample{0};    // Z
};

/** 由权威播放起止时间直接建立固定映射；终点 E 直接投影，不由起点与时长重建。 */
inline Placement makePlacement(double playbackRate,
                               double playbackStartSeconds,
                               double playbackEndSeconds,
                               double sourceStartSeconds) noexcept
{
    return { playbackRate,
             projectToSample((playbackStartSeconds - sourceStartSeconds) * playbackRate),
             projectToSample(playbackStartSeconds * playbackRate),
             projectToSample(playbackEndSeconds * playbackRate) };
}

/**
 * 连续切片继承父切片的固定 D；切片自身的权威 P/E 直接投影。
 * 前后片在公共边界投影为同一个整数端点。
 */
inline Placement makeContiguousSlice(const Placement& parent,
                                     double playbackStartSeconds,
                                     double playbackEndSeconds) noexcept
{
    return { parent.playbackRate,
             parent.offsetSamples,
             projectToSample(playbackStartSeconds * parent.playbackRate),
             projectToSample(playbackEndSeconds * parent.playbackRate) };
}

/** host 播放区间 [A, Z)。 */
inline SampleSpan hostPlaybackSpan(const Placement& placement) noexcept
{
    return { placement.playbackBeginSample, placement.playbackEndSample };
}

/** 所需 prepared 区间 [A - D, Z - D)。 */
inline SampleSpan requiredPreparedSpan(const Placement& placement) noexcept
{
    return { placement.playbackBeginSample - placement.offsetSamples,
             placement.playbackEndSample - placement.offsetSamples };
}

/**
 * host 播放样本地址到 prepared 地址的映射。
 *
 * preparedFirstSample 是 prepared buffer 首元素在来源播放率网格上的索引 J0：
 *   sourcePreparedGlobalIndex = hostSampleIndex - D
 *   bufferLocalIndex          = sourcePreparedGlobalIndex - J0
 * 负 local 前缀由调用方通过半开区间求交裁剪，保留有效后缀。
 */
struct PreparedAddress
{
    int64_t preparedGlobalIndex{0};  // hostSampleIndex - D
    int64_t bufferLocalIndex{0};     // preparedGlobalIndex - preparedFirstSample
};

inline PreparedAddress mapHostSampleToPrepared(const Placement& placement,
                                               int64_t hostSampleIndex,
                                               int64_t preparedFirstSample) noexcept
{
    const int64_t preparedGlobalIndex = hostSampleIndex - placement.offsetSamples;
    return { preparedGlobalIndex, preparedGlobalIndex - preparedFirstSample };
}

} // namespace OpenTune::PlaybackSampleGeometry
