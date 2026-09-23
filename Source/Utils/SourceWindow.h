#pragma once
#include <juce_core/juce_core.h>
#include <cstdint>
#include <cmath>
#include <optional>
#include "TimeCoordinate.h"

namespace OpenTune {

// Source-absolute window: 一个 Content 从某 Source 的哪一段提炼而来。
// 这是 lineage 事实的物理表达。禁止用于任何 content-local 或 timeline 坐标。
// 已提交窗口的两个端点必须位于其 source sample 网格上。
// 
// Per ARA2 spec: AudioSource.persistentID is the stable identifier for archive/restore.
// For ARA domain: use sourcePersistentId (string)
// For Capture/Standalone domains: use sourceId (numeric)
struct SourceWindow {
    uint64_t     sourceId{0};                 // Capture/Standalone: numeric ID
    juce::String sourcePersistentId;          // ARA: AudioSource persistentID
    double       sourceStartSeconds{0.0};     // [start, end) source-absolute
    double       sourceEndSeconds{0.0};

    bool isValid() const noexcept {
        return (sourceId != 0 || sourcePersistentId.isNotEmpty()) 
            && sourceEndSeconds > sourceStartSeconds;
    }
    double durationSeconds() const noexcept {
        return sourceEndSeconds - sourceStartSeconds;
    }
};

// 无状态对齐：把窗口两端投影到所属 source 的 sample 网格（各端点独立 nearest），
// 再以 sample / sourceSampleRate 回写 seconds。只规范化时间端点，不改变 source identity。
// 输入端点几何无效（非有限或 end <= start）、sourceSampleRate 无效，或对齐后端点
// 不再构成有效窗口时返回 nullopt；未带 identity 的导入前窗口是合法输入。
inline std::optional<SourceWindow> alignSourceWindowToSampleGrid(const SourceWindow& window,
                                                                 double sourceSampleRate)
{
    if (!std::isfinite(sourceSampleRate) || sourceSampleRate <= 0.0
        || !std::isfinite(window.sourceStartSeconds)
        || !std::isfinite(window.sourceEndSeconds)
        || window.sourceEndSeconds <= window.sourceStartSeconds)
        return std::nullopt;

    SourceWindow aligned = window;
    aligned.sourceStartSeconds = TimeCoordinate::samplesToSeconds(
        TimeCoordinate::secondsToSamplesNearest(aligned.sourceStartSeconds, sourceSampleRate),
        sourceSampleRate);
    aligned.sourceEndSeconds = TimeCoordinate::samplesToSeconds(
        TimeCoordinate::secondsToSamplesNearest(aligned.sourceEndSeconds, sourceSampleRate),
        sourceSampleRate);

    if (aligned.sourceEndSeconds <= aligned.sourceStartSeconds)
        return std::nullopt;

    return aligned;
}

} // namespace OpenTune
