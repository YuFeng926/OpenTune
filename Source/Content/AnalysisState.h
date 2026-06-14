#pragma once
#include "../Utils/ContentAnalysisState.h"
#include "../Utils/PitchCurve.h"
#include "../DSP/ChromaKeyDetector.h"
#include "../DSP/ReferenceFeatures.h"
#include "../Utils/SilentGapDetector.h"
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

enum class AnalysisLifecycle : uint8_t
{
    Idle,
    Requested,
    InProgress,
    Ready,
    Failed
};

// 分析字段属于内容根，但不是编辑命令真相
struct AnalysisState
{
    OriginalF0State originalF0State{OriginalF0State::NotRequested};
    std::shared_ptr<PitchCurve> pitchCurve;
    DetectedKey detectedKey;
    std::vector<SilentGap> silentGaps;
    ReferenceFeatureSet referenceFeatures;
    AnalysisLifecycle f0Lifecycle{AnalysisLifecycle::Idle};
    AnalysisLifecycle pitchLifecycle{AnalysisLifecycle::Idle};
    uint64_t analysisRevision{0};
};

} // namespace OpenTune
