#pragma once
#include "AnalysisState.h"
#include "../Utils/SourceWindow.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include "../Utils/Note.h"
#include "../Utils/AutomationLane.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

/// 内容域统一状态 — Standalone / Capture / ARA 三个 owner 共用同一结构。
/// 不含 render cache、worker、stretcher、playback publisher 所有权，
/// 也不含域生命周期（ARA birth state、Capture retired/session、Standalone retired records）。
struct ContentState
{
    SourceWindow sourceWindow;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double sampleRate{0.0};

    // ── Analysis state ──────────────────────────────────────
    AnalysisState analysis;

    // ── Editable state ──────────────────────────────────────
    std::vector<Note> notes;
    // 始终为非空 identity 或非恒等 snapshot；identity 不用 nullptr 表达。
    // 默认/空状态为 bootstrap identity，真实 source/audio 到达时由 owner 覆盖。
    std::shared_ptr<const TimeGridSnapshot> timeGrid{TimeGridSnapshot::bootstrapIdentity()};
    PitchShiftSettings pitchShiftSettings;
    AutomationLane volumeEnvelope;

    // ── Revisions ───────────────────────────────────────────
    uint64_t notesRevision{0};
    bool noteTopologyInitialized{false};  // 内容是否经历过至少一次音符拓扑提交（含合法空结果）
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    // 运行时 cache identity：新 owner/content 从 1 开始，0 只表示"读取不到内容"。
    uint64_t contentRevision{1};
    uint64_t audioRevision{0};
};

} // namespace OpenTune
