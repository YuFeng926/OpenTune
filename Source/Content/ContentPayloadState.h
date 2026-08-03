#pragma once
#include "../Utils/SourceWindow.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include "../Utils/Note.h"
#include "../Utils/OutputGainEnvelope.h"
#include "../Utils/SilentGapDetector.h"
#include "../Utils/ContentAnalysisState.h"   // OriginalF0State
#include "../DSP/ChromaKeyDetector.h"         // DetectedKey
#include "../DSP/ReferenceFeatures.h"          // ReferenceFeatureSet
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

enum class ContentLifecycle
{
    Empty,
    Ready,
    Retired
};

/// Standalone 域完整 payload — 所有数据由一个 ContentPayloadState 表达。
/// 不含 render cache、worker、stretcher、playback publisher 所有权。
struct ContentPayloadState
{
    SourceWindow sourceWindow;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double sampleRate{44100.0};

    // ── Analysis state ──────────────────────────────────────
    std::shared_ptr<PitchCurve> pitchCurve;
    OriginalF0State originalF0State{OriginalF0State::NotRequested};
    DetectedKey detectedKey;
    std::vector<SilentGap> silentGaps;
    ReferenceFeatureSet referenceFeatures;

    // ── Editable state ──────────────────────────────────────
    std::vector<Note> notes;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;
    PitchShiftSettings pitchShiftSettings;
    SibilantGainEnvelope sibilantGainEnvelope;

    // ── Revisions ───────────────────────────────────────────
    uint64_t notesRevision{0};
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};
    uint64_t outputGainRevision{0};
    uint64_t contentRevision{0};
    uint64_t audioRevision{0};

    ContentLifecycle lifecycle{ContentLifecycle::Ready};
};

} // namespace OpenTune
