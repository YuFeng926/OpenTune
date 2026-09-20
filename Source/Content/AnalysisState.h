#pragma once
#include "../Utils/ContentAnalysisState.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/DetectedKey.h"
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

    bool setOriginalF0State(OriginalF0State state) noexcept
    {
        AnalysisLifecycle lifecycle = AnalysisLifecycle::Idle;
        switch (state)
        {
            case OriginalF0State::NotRequested: lifecycle = AnalysisLifecycle::Idle; break;
            case OriginalF0State::Extracting: lifecycle = AnalysisLifecycle::InProgress; break;
            case OriginalF0State::Ready: lifecycle = AnalysisLifecycle::Ready; break;
            case OriginalF0State::Failed: lifecycle = AnalysisLifecycle::Failed; break;
        }

        if (originalF0State == state && f0Lifecycle == lifecycle)
            return false;

        originalF0State = state;
        f0Lifecycle = lifecycle;
        return true;
    }
};

} // namespace OpenTune
