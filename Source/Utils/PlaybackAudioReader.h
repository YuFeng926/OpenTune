#pragma once

#include "../MaterializationStore.h"
#include "TimeCoordinate.h"
#include "../Inference/TimeStretchCache.h"
#include "../Inference/RenderCache.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

namespace OpenTune {

/**
 * PlaybackReadRequest - 统一读取请求结构
 *
 * readStartSeconds 必须与 source 中 renderCache / dry buffer 的时间基保持一致。
 */
struct PlaybackReadRequest {
    MaterializationStore::PlaybackReadSource source;
    double readStartSeconds{0.0};
    double targetSampleRate{44100.0};
    int numSamples{0};

    PlaybackReadRequest() = default;
    PlaybackReadRequest(MaterializationStore::PlaybackReadSource src, double start, double rate, int samples)
        : source(src), readStartSeconds(start), targetSampleRate(rate), numSamples(samples) {}
};

/**
 * 统一播放读取 API
 *
 * 二元播放模型：先读当前播放采样率 dry signal，再用当前播放采样率的
 * 已发布 render cache 覆盖同一段目标 buffer。
 */
inline int readPlaybackAudio(const PlaybackReadRequest& request,
                             juce::AudioBuffer<float>& destination,
                             int destinationStartSample)
{
    if (request.numSamples <= 0
        || request.targetSampleRate <= 0.0
        || !request.source.canRead()
        || request.source.audioBuffer == nullptr) {
        return 0;
    }

    const int destinationChannels = destination.getNumChannels();
    const int destinationSamples = destination.getNumSamples();
    if (destinationChannels <= 0
        || destinationSamples <= 0
        || destinationStartSample < 0
        || destinationStartSample >= destinationSamples) {
        return 0;
    }

    const int writableSamples = juce::jmin(request.numSamples, destinationSamples - destinationStartSample);
    if (writableSamples <= 0) {
        return 0;
    }

    // TimeStretchCache fast-path
    if (!request.source.timeGridIsIdentity
        && request.source.timeStretchCache != nullptr
        && request.source.materializationId != 0) {
        const int wrote = request.source.timeStretchCache->sliceForOutputRange(
            request.source.materializationId,
            request.readStartSeconds,
            destination,
            destinationStartSample,
            writableSamples,
            static_cast<int>(request.targetSampleRate));
        if (wrote > 0) {
            return wrote;
        }
    }

    const auto& srcBuffer = *request.source.audioBuffer;
    const int srcChannels = srcBuffer.getNumChannels();
    const int64_t srcLengthSamples = srcBuffer.getNumSamples();
    constexpr double srcSampleRate = TimeCoordinate::kRenderSampleRate;
    if (srcChannels <= 0 || srcLengthSamples <= 0) {
        return 0;
    }

    const double ratio = srcSampleRate / request.targetSampleRate;
    const double readStartInSrcSamples = request.readStartSeconds * srcSampleRate;
    if (readStartInSrcSamples < 0.0 || readStartInSrcSamples >= static_cast<double>(srcLengthSamples)) {
        return 0;
    }

    const int maxSrcSample = static_cast<int>(srcLengthSamples) - 1;
    int availableSamples = writableSamples;
    {
        const double lastSrcPos = readStartInSrcSamples + (writableSamples - 1) * ratio;
        if (lastSrcPos >= static_cast<double>(srcLengthSamples)) {
            availableSamples = static_cast<int>((static_cast<double>(srcLengthSamples) - readStartInSrcSamples) / ratio);
            if (availableSamples <= 0) return 0;
        }
    }

    // Write dry signal with linear interpolation
    for (int channel = 0; channel < destinationChannels; ++channel) {
        const int srcCh = channel % srcChannels;
        const float* srcPtr = srcBuffer.getReadPointer(srcCh);
        float* dstPtr = destination.getWritePointer(channel, destinationStartSample);

        double srcPos = readStartInSrcSamples;
        for (int s = 0; s < availableSamples; ++s) {
            const int idx0 = static_cast<int>(srcPos);
            const int idx1 = juce::jmin(idx0 + 1, maxSrcSample);
            const float fraction = static_cast<float>(srcPos - idx0);
            dstPtr[s] = srcPtr[idx0] + (srcPtr[idx1] - srcPtr[idx0]) * fraction;
            srcPos += ratio;
        }
    }

    if (request.source.renderCache != nullptr) {
        request.source.renderCache->overlayPublishedAudioForRate(destination,
                                                                  destinationStartSample,
                                                                  availableSamples,
                                                                  request.readStartSeconds,
                                                                  static_cast<int>(request.targetSampleRate));
    }

    return availableSamples;
}

} // namespace OpenTune
