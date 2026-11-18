#pragma once
#include "../Utils/ContentAnalysisState.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/DetectedKey.h"
#include "../DSP/ReferenceFeatures.h"
#include "../Utils/SilentGapDetector.h"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace OpenTune {

// 分析字段属于内容根，但不是编辑命令真相
struct AnalysisState
{
    OriginalF0State originalF0State{OriginalF0State::NotRequested};
    std::string originalF0FailureReason;
    std::shared_ptr<PitchCurve> pitchCurve;
    DetectedKey detectedKey;
    std::vector<SilentGap> silentGaps;
    ReferenceFeatureSet referenceFeatures;
    uint64_t analysisRevision{0};

    bool setOriginalF0State(OriginalF0State state) noexcept
    {
        if (state != OriginalF0State::Failed)
            originalF0FailureReason.clear();
        if (originalF0State == state)
            return false;

        originalF0State = state;
        return true;
    }
};

} // namespace OpenTune
