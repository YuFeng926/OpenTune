#pragma once
#include "../Utils/SourceWindow.h"
#include "../Utils/Note.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include "../Utils/ContentAnalysisState.h"
#include "../Utils/SilentGapDetector.h"
#include "../DSP/ChromaKeyDetector.h"
#include "../DSP/ReferenceFeatures.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

struct EditableContentSnapshot
{
    SourceWindow sourceWindow;

    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double audioSampleRate{0.0};  // 与 audioBuffer 绑定
    uint64_t audioRevision{0};

    // ARA2: AudioSource 只读元数据（从 AudioModification.cachedSourceShape_ 委托）
    double sourceSampleRate{0.0};
    int sourceChannelCount{0};
    int64_t sourceSampleCount{0};

    std::vector<Note> notes;
    std::shared_ptr<PitchCurve> pitchCurve;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;
    PitchShiftSettings pitchShiftSettings;

    OriginalF0State originalF0State{OriginalF0State::NotRequested};
    DetectedKey detectedKey;
    std::vector<SilentGap> silentGaps;
    ReferenceFeatureSet referenceFeatures;

    uint64_t notesRevision{0};
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};
    uint64_t contentRevision{0};

    template <typename Sink>
    void forEachEffectiveF0Span(int startFrame, int endFrame, Sink&& sink) const
    {
        if (pitchCurve == nullptr)
            return;

        const auto curveSnapshot = pitchCurve->getSnapshot();
        const auto& originalF0 = curveSnapshot->getOriginalF0();
        const float gapGain = static_cast<float>(pitchShiftSettings.getPitchRatio());

        curveSnapshot->forEachCorrectionF0Span(
            startFrame,
            endFrame,
            [&](int spanStartFrame, const float* values, int count)
            {
                const bool isCorrection = values != nullptr;
                sink(spanStartFrame,
                     isCorrection ? values : originalF0.data() + spanStartFrame,
                     count,
                     isCorrection ? 1.0f : gapGain);
            });
    }
};

} // namespace OpenTune
