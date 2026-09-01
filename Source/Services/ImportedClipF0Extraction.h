#pragma once

#include "F0ExtractionService.h"
#include "../Content/EditableContentSnapshot.h"
#include "../Inference/F0InferenceService.h"
#include "../Utils/TimeCoordinate.h"

#include <cmath>
#include <vector>

namespace OpenTune {

/// Pure energy computation shared by ARA and Standalone paths.
/// Returns energy[f0.size()], voiced-gated RMS per F0 frame using ±10ms center-aligned window.
/// centerSample = round(frameIndex * hopSize / f0SampleRate * sourceSampleRate)
inline std::vector<float> computeFrameEnergy(
    const float* sourcePCM, int numSamples, int sourceSampleRate,
    const std::vector<float>& f0, int f0SampleRate, int hopSize)
{
    std::vector<float> energy(f0.size(), 0.0f);
    if (f0SampleRate <= 0 || hopSize <= 0 || sourceSampleRate <= 0 || numSamples <= 0)
        return energy;

    const int halfRmsWindowSamples = juce::jmax(1,
        static_cast<int>(std::round(static_cast<double>(sourceSampleRate) * 0.010)));
    const double f0SecondsPerFrame = static_cast<double>(hopSize)
        / static_cast<double>(juce::jmax(1, f0SampleRate));

    for (size_t i = 0; i < f0.size(); ++i) {
        if (!std::isfinite(f0[i]) || f0[i] <= 0.0f)
            continue;

        const int centerSample = juce::jlimit(
            0, numSamples - 1,
            static_cast<int>(std::round(static_cast<double>(i)
                * f0SecondsPerFrame * static_cast<double>(sourceSampleRate))));
        const int startSample = juce::jmax(0, centerSample - halfRmsWindowSamples);
        const int endSampleExclusive = juce::jmin(numSamples, centerSample + halfRmsWindowSamples);
        if (endSampleExclusive <= startSample)
            continue;

        double squareSum = 0.0;
        for (int sample = startSample; sample < endSampleExclusive; ++sample) {
            const float value = sourcePCM[sample];
            squareSum += static_cast<double>(value) * static_cast<double>(value);
        }
        const double meanSquare = squareSum / static_cast<double>(endSampleExclusive - startSample);
        energy[i] = juce::jlimit(0.0f, 1.0f, static_cast<float>(std::sqrt(meanSquare)));
    }
    return energy;
}

inline bool extractOriginalF0ForImportedClip(F0InferenceService& f0Service,
                                              const std::shared_ptr<F0RunOwnerState>& runOwnerState,
                                              const EditableContentSnapshot& snap,
                                              F0ExtractionService::Result& out,
                                              std::string& errorMessage)
{
    if (snap.audioBuffer == nullptr) {
        errorMessage = "clip_snapshot_failed";
        return false;
    }

    const int numSamples = snap.audioBuffer->getNumSamples();
    const int numChannels = snap.audioBuffer->getNumChannels();
    if (numSamples <= 0 || numChannels <= 0) {
        errorMessage = "invalid_audio_buffer";
        return false;
    }

    constexpr double internalSampleRate = TimeCoordinate::kRenderSampleRate;
    out.audioDurationSeconds = static_cast<double>(numSamples) / internalSampleRate;

    {
        int firstAudibleSample = -1;
        const float* probe = snap.audioBuffer->getReadPointer(0);
        for (int i = 0; i < numSamples; ++i) {
            if (std::abs(probe[i]) > 1.0e-4f) {
                firstAudibleSample = i;
                break;
            }
        }
        out.firstAudibleTimeSeconds = (firstAudibleSample >= 0)
            ? static_cast<double>(firstAudibleSample) / internalSampleRate
            : -1.0;
    }

    const float* src = snap.audioBuffer->getReadPointer(0);

    const int hopSize = f0Service.getF0HopSize();
    const int f0SampleRate = f0Service.getF0SampleRate();

    out.expectedInferenceFrameCount = static_cast<int>(std::ceil(out.audioDurationSeconds
        * static_cast<double>(f0SampleRate) / static_cast<double>(juce::jmax(1, hopSize))));

    auto extraction = f0Service.extractF0(src, static_cast<size_t>(numSamples),
                                          static_cast<int>(internalSampleRate), runOwnerState);
    if (!extraction.ok() || extraction.value().empty()) {
        errorMessage = "f0_empty_or_unvoiced";
        return false;
    }

    out.f0 = extraction.value();
    out.energy = computeFrameEnergy(src, numSamples,
        static_cast<int>(internalSampleRate), out.f0, f0SampleRate, hopSize);

    out.hopSize = hopSize;
    out.f0SampleRate = f0SampleRate;
    out.modelName = f0Service.getCurrentF0Model() == F0ModelType::FCPE ? "FCPE" : "FCPE";

    {
        out.firstVoicedFrame = -1;
        out.firstVoicedTimeSeconds = -1.0;
        for (size_t i = 0; i < out.f0.size(); ++i) {
            if (std::isfinite(out.f0[i]) && out.f0[i] > 0.0f) {
                out.firstVoicedFrame = static_cast<int>(i);
                const double secondsPerFrame = static_cast<double>(hopSize)
                    / static_cast<double>(juce::jmax(1, f0SampleRate));
                out.firstVoicedTimeSeconds = static_cast<double>(i) * secondsPerFrame;
                break;
            }
        }
    }

    return true;
}

} // namespace OpenTune
