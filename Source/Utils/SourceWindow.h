#pragma once
#include <cstdint>

namespace OpenTune {

// Source-absolute window: 一个 Materialization 从某 Source 的哪一段提炼而来。
// 这是 lineage 事实的物理表达。禁止用于任何 materialization-local 或 timeline 坐标。
struct SourceWindow {
    uint64_t sourceId{0};
    double   sourceStartSeconds{0.0};   // [start, end) source-absolute
    double   sourceEndSeconds{0.0};

    bool isValid() const noexcept {
        return sourceId != 0 && sourceEndSeconds > sourceStartSeconds;
    }
    double durationSeconds() const noexcept {
        return sourceEndSeconds - sourceStartSeconds;
    }
};

} // namespace OpenTune
