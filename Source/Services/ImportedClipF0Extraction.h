#pragma once

#include "F0ExtractionService.h"
#include "../MaterializationStore.h"
#include "../Inference/F0InferenceService.h"
#include "../Utils/TimeCoordinate.h"

#include <cmath>
#include <vector>

namespace OpenTune {

inline bool extractOriginalF0ForImportedClip(F0InferenceService& f0Service,
                                             const MaterializationStore::MaterializationSnapshot& snap,
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

    // F0Alignment: audio duration
    constexpr double internalSampleRate = TimeCoordinate::kRenderSampleRate;
    out.audioDurationSeconds = static_cast<double>(numSamples) / internalSampleRate;

    // F0Alignment: first audible sample (first sample where average abs across channels > 1.0e-4)
    {
        int firstAudibleSample = -1;
        for (int i = 0; i < numSamples; ++i) {
            double channelSum = 0.0;
            for (int ch = 0; ch < numChannels; ++ch) {
                channelSum += std::abs(snap.audioBuffer->getSample(ch, i));
            }
            const double avg = channelSum / static_cast<double>(numChannels);
            if (avg > 1.0e-4) {
                firstAudibleSample = i;
                break;
            }
        }
        out.firstAudibleTimeSeconds = (firstAudibleSample >= 0)
            ? static_cast<double>(firstAudibleSample) / internalSampleRate
            : -1.0;
    }

    std::vector<float> monoAudio(static_cast<size_t>(numSamples), 0.0f);
    const float invChannels = 1.0f / static_cast<float>(numChannels);
    for (int ch = 0; ch < numChannels; ++ch) {
        const float* src = snap.audioBuffer->getReadPointer(ch);
        for (int i = 0; i < numSamples; ++i) {
            monoAudio[static_cast<size_t>(i)] += src[i] * invChannels;
        }
    }

    // Materialization audio is stored in the shared runtime's fixed local sample-rate domain.
    const int hopSize = f0Service.getF0HopSize();
    const int f0SampleRate = f0Service.getF0SampleRate();

    // F0Alignment: expected inference frame count
    out.expectedInferenceFrameCount = static_cast<int>(std::ceil(out.audioDurationSeconds
        * static_cast<double>(f0SampleRate) / static_cast<double>(juce::jmax(1, hopSize))));

    auto extraction = f0Service.extractF0(monoAudio.data(), monoAudio.size(),
                                          static_cast<int>(internalSampleRate));
    if (!extraction.ok() || extraction.value().empty()) {
        errorMessage = "f0_empty_or_unvoiced";
        return false;
    }

    out.f0 = extraction.value();
    out.energy.resize(out.f0.size(), 0.0f);
    for (size_t i = 0; i < out.f0.size(); ++i) {
        if (std::isfinite(out.f0[i]) && out.f0[i] > 0.0f)
            out.energy[i] = 1.0f;
    }

    out.hopSize = hopSize;
    out.f0SampleRate = f0SampleRate;
    out.modelName = "RMVPE";

    // F0Alignment: first voiced frame/time
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

    int voicedFrames = 0;
    for (float value : out.f0) {
        if (std::isfinite(value) && value > 0.0f)
            ++voicedFrames;
    }

    if (voicedFrames == 0) {
        errorMessage = "f0_empty_or_unvoiced";
        return false;
    }

    return true;
}

} // namespace OpenTune
