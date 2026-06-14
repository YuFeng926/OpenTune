#pragma once
#include <juce_core/juce_core.h>
#include <cstdint>

namespace OpenTune {

// Source-absolute window: 一个 Content 从某 Source 的哪一段提炼而来。
// 这是 lineage 事实的物理表达。禁止用于任何 content-local 或 timeline 坐标。
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

} // namespace OpenTune
