/**
 * 工程数据模型（ProjectModel）
 *
 * 定义 OpenTune 工程快照层的数据结构。工程快照是可持久化的用户意图 + 编辑真相 +
 * 媒体索引的完整表达，不包含运行态缓存、worker 状态或 AppPreferences 级偏好。
 *
 * 设计原则：
 *   - 只保存用户意图与编辑真相，不保存 RenderCache、推理状态、动画状态
 *   - 媒体引用使用工程目录下的相对路径
 *   - 内容身份统一使用 ContentKey；placement/source 自身 id 仍是 domain-local uint64_t
 *   - 所有时间统一用 Seconds，所有版本/修订统一用 revision
 */

#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <cstdint>
#include <vector>

#include "../DSP/ChromaKeyDetector.h"
#include "../Utils/Note.h"
#include "../Utils/OutputGainEnvelope.h"
#include "../Utils/SourceWindow.h"
#include "../Content/ContentKey.h"

namespace OpenTune {

// ============================================================================
// 工程头信息
// ============================================================================

struct ProjectHeader {
    int projectFormatVersion{3};
    juce::String appVersion;
    juce::String projectName{"Untitled"};
    juce::String projectId;
    juce::String createdAt;
    juce::String lastSavedAt;
};

// ============================================================================
// 工程级设置（写入 .otproj 的视图状态，用于恢复工作位置）
// ============================================================================

struct ProjectSettings {
    double bpm{120.0};
    double sampleRate{44100.0};
    int timeSignatureNumerator{4};
    int timeSignatureDenominator{4};
    int selectedTrackId{0};
    uint64_t selectedPlacementId{0};
    int activeTrackId{0};
    double scrollX{0.0};
    double scrollY{0.0};
    double zoomLevel{1.0};
    double verticalZoom{1.0};
    double timelineOriginSeconds{0.0};
};

// ============================================================================
// 媒体池条目（一个 Source 的持久化描述）
// ============================================================================

struct ProjectSourceEntry {
    uint64_t sourceId{0};
    juce::String displayName;
    juce::String originalImportPath;         // 用户导入时的原始路径
    juce::String relativeMediaPath;           // 工程目录下的相对媒体路径
    double sampleRate{0.0};
    int numChannels{0};
    int64_t lengthSamples{0};
    double lengthSeconds{0.0};
    juce::String contentHash;                 // 文件内容哈希，用于去重
    int64_t fileSizeBytes{0};
};

// ============================================================================
// Content 持久化条目
// ============================================================================

struct ProjectContentEntry {
    ContentKey contentKey;
    uint64_t sourceId{0};
    bool retired{false};
    uint64_t renderRevision{0};
    ContentKey lineageParentContentKey;

    // Source window (provenance)
    SourceWindow sourceWindow;

    // Detected key
    DetectedKey detectedKey;

    // Notes (user-edited)
    std::vector<Note> notes;

    // B 层：Sibilant Balance 结果（content-local 绝对时间，dB）
    SibilantGainEnvelope sibilantGainEnvelope;

    // Corrected segments (F0 corrections)
    struct SegmentEntry {
        int startFrame{0};
        int endFrame{0};
        uint8_t source{0};              // PitchCorrectionSegment::Source 枚举值
        float retuneSpeed{-1.0f};
        float vibratoDepth{-1.0f};
        float vibratoRate{-1.0f};
        std::vector<float> f0Data;
    };
    std::vector<SegmentEntry> correctionSegments;

    // TimeGrid (v7 vocal-time-stretch)
    struct TimeGridEntry {
        uint64_t revision{0};
        struct HandleEntry {
            uint64_t id{0};
            uint8_t kind{0};             // HandleKind 枚举值
            double sourceSeconds{0.0};
            double outputSeconds{0.0};
            uint8_t confidence{0};       // Confidence 枚举值 (0=Default, 1=High)
        };
        std::vector<HandleEntry> handles;
    };
    TimeGridEntry timeGrid;

    // Original F0 extraction state
    uint8_t originalF0State{0};         // OriginalF0State 枚举值

    // Pitch shift settings (整体移调)
    struct PitchShiftEntry {
        int semitone{0};
        int cents{0};
    };
    PitchShiftEntry pitchShiftSettings;

    // Silent gaps (用于 render chunk boundaries)
    struct SilentGapEntry {
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
        float minLevel_dB{0.0f};
    };
    std::vector<SilentGapEntry> silentGaps;

    // Reference features (用于对齐参考)
    struct ReferenceFeatureEntry {
        int analysisRevision{0};
        uint8_t status{0};              // ReferenceFeatureStatus 枚举值
        uint8_t producer{0};            // ReferenceFeatureProducer 枚举值
        int64_t inputFingerprint{0};
        double sourceDurationSeconds{0.0};
        juce::String errorMessage;

        // Pitch features (notes)
        std::vector<Note> pitchNotes;

        // Timing features (anchors)
        struct TimingAnchorEntry {
            uint64_t anchorId{0};
            double sourceSeconds{0.0};
            float strength{0.0f};
            uint8_t kind{0};            // ReferenceTimingAnchorKind 枚举值
            float confidence{0.0f};
        };
        std::vector<TimingAnchorEntry> timingAnchors;
    };
    ReferenceFeatureEntry referenceFeatures;

};

// ============================================================================
// Reference binding（clip 间的参考关系）
// ============================================================================

struct ProjectReferenceBinding {
    uint64_t targetPlacementId{0};
    uint64_t referencePlacementId{0};
};

// ============================================================================
// Track 与 Placement 持久化条目
// ============================================================================

struct ProjectPlacementEntry {
    uint64_t placementId{0};
    ContentKey contentKey;
    uint64_t mappingRevision{0};
    double timelineStartSeconds{0.0};
    double timelineDurationSeconds{0.0};
    float clipGain{1.0f};
    double fadeInDurationSeconds{0.0};
    double fadeOutDurationSeconds{0.0};
    double clipInSeconds{0.0};
    juce::String name;
};

struct ProjectTrackEntry {
    int trackId{0};
    juce::String name;
    float gain{1.0f};
    bool mute{false};
    bool solo{false};
    juce::Colour colour;
    std::vector<ProjectPlacementEntry> placements;
};

// ============================================================================
// 完整工程快照
// ============================================================================

struct ProjectSnapshot {
    ProjectHeader header;
    ProjectSettings settings;
    std::vector<ProjectSourceEntry> sources;
    std::vector<ProjectContentEntry> contents;
    std::vector<ProjectTrackEntry> tracks;
    std::vector<ProjectReferenceBinding> referenceBindings;

    /** 生成唯一的工程 ID（时间戳 + 随机数） */
    static juce::String generateProjectId();

    /** 生成创建/保存时间戳字符串 */
    static juce::String generateTimestamp();
};

} // namespace OpenTune
