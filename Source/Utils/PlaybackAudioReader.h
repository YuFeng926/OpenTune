#pragma once

#include "../Render/ContentRenderService.h"
#include "TimeCoordinate.h"
#include "../Inference/TimeStretchCache.h"
#include "../Inference/RenderCache.h"
#include "OutputGainEnvelope.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

namespace OpenTune {

/**
 * PlaybackReadRequest — 实时读取请求，使用目标采样率下的绝对样本位置。
 *
 * readStartSample 是在 targetSampleRate 空间中的绝对样本偏移。
 * 调用方负责保证 readStartSample + numSamples 不越界。
 * 只从 prepared data 读取，无 canonical fallback。
 */
struct PlaybackReadRequest {
    PlaybackReadSource source;
    int64_t readStartSample{0};
    double targetSampleRate{44100.0};
    int numSamples{0};

    PlaybackReadRequest() = default;
    PlaybackReadRequest(PlaybackReadSource src, int64_t startSample, double rate, int samples)
        : source(src), readStartSample(startSample), targetSampleRate(rate), numSamples(samples) {}
};

/**
 * CanonicalReadRequest — 离线 canonical 读取请求（Stage2/export）。
 *
 * readStartSample 在 canonical 44.1kHz 样本空间中的绝对偏移。
 * 无 target rate 参数，始终读取 44.1kHz truth。
 */
struct CanonicalReadRequest {
    PlaybackReadSource source;
    int64_t readStartSample{0};
    int numSamples{0};

    CanonicalReadRequest() = default;
    CanonicalReadRequest(PlaybackReadSource src, int64_t startSample, int samples)
        : source(src), readStartSample(startSample), numSamples(samples) {}
};

/**
 * 最终输出增益：preparedOutputGainEnvelope 在 playback rate 空间逐样本乘法。
 * readStartSample 与目标采样率同空间（与 preparedDry 索引一致）。
 * 无分配、无锁、无 pow；包络缺失 = 单位增益。
 */
inline void applyPreparedOutputGain(juce::AudioBuffer<float>& destination,
                                    int destinationStartSample,
                                    int numSamples,
                                    const std::shared_ptr<const PreparedOutputGainEnvelope>& prepared,
                                    int64_t readStartSample)
{
    if (prepared == nullptr || prepared->linearGains.empty() || numSamples <= 0)
        return;
    if (readStartSample < 0 || readStartSample >= static_cast<int64_t>(prepared->linearGains.size()))
        return;

    const int applySamples = static_cast<int>(juce::jmin<int64_t>(
        numSamples, static_cast<int64_t>(prepared->linearGains.size()) - readStartSample));
    const float* gains = prepared->linearGains.data();
    const int channels = destination.getNumChannels();
    for (int channel = 0; channel < channels; ++channel) {
        float* dst = destination.getWritePointer(channel, destinationStartSample);
        for (int s = 0; s < applySamples; ++s)
            dst[s] *= gains[static_cast<size_t>(readStartSample + s)];
    }
}

/**
 * 实时播放读取 — 纯 direct copy，无插值。
 *
 * 1. TimeStretchCache fast-path：从 prepared 缓存直接整数切片。
 * 2. 否则从 preparedDry buffer 直接 copy（已在 prepare 阶段由 r8brain 重采样）。
 * 3. 然后从 RenderCache prepared chunks overlay（同样直接 copy）。
 * 4. 两路径汇合到同一 applyPreparedOutputGain() 收尾。
 *
 * 无 canonical fallback。prepared 数据不存在时返回 0。
 */
inline int readPlaybackAudio(const PlaybackReadRequest& request,
                             juce::AudioBuffer<float>& destination,
                             int destinationStartSample)
{
    if (request.numSamples <= 0
        || request.targetSampleRate <= 0.0
        || !request.source.hasAudio()) {
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

    // ============================================================
    // TimeStretchCache fast-path — 直接从 prepared 缓存整数切片
    // ============================================================
    const uint64_t objectId = request.source.contentKey.objectId;
    if (!request.source.timeGridIsIdentity
        && request.source.timeStretchCache != nullptr
        && objectId != 0) {
        const int wrote = request.source.timeStretchCache->sliceForOutputRange(
            request.source.contentKey,
            request.source.pitchRevision,
            request.source.pitchShiftRevision,
            request.source.timeGridRevision,
            request.readStartSample,
            destination,
            destinationStartSample,
            writableSamples,
            static_cast<int>(request.targetSampleRate));
        if (wrote > 0) {
            applyPreparedOutputGain(destination, destinationStartSample, wrote,
                                    request.source.preparedOutputGainEnvelope,
                                    request.readStartSample);
            return wrote;
        }
    }

    // ============================================================
    // 从 preparedDry 直接 copy（已在 prepare 阶段重采样）
    // ============================================================
    const auto& prepared = request.source.preparedDry;
    if (!prepared.buffer
        || std::abs(prepared.sampleRate - request.targetSampleRate) >= 1.0) {
        return 0;  // No canonical fallback
    }

    const int preparedLen = prepared.buffer->getNumSamples();
    const int preparedChannels = prepared.buffer->getNumChannels();
    if (preparedLen <= 0 || preparedChannels <= 0) {
        return 0;
    }

    const int64_t start = request.readStartSample;
    if (start < 0 || start >= preparedLen) {
        return 0;
    }

    const int availableSamples = juce::jmin(writableSamples,
        static_cast<int>(preparedLen - start));
    if (availableSamples <= 0) {
        return 0;
    }

    for (int channel = 0; channel < destinationChannels; ++channel) {
        const int srcCh = channel % preparedChannels;
        destination.copyFrom(channel, destinationStartSample,
                             *prepared.buffer, srcCh,
                             static_cast<int>(start), availableSamples);
    }

    // ============================================================
    // RenderCache prepared overlay — 直接 copy
    // ============================================================
    if (request.source.renderCache != nullptr) {
        request.source.renderCache->overlayPreparedAudio(destination,
                                                          destinationStartSample,
                                                          availableSamples,
                                                          request.readStartSample,
                                                          static_cast<int>(request.targetSampleRate));
    }

    // ============================================================
    // 统一最终增益收尾（与 TimeStretchCache fast-path 同一 apply）
    // ============================================================
    applyPreparedOutputGain(destination, destinationStartSample, availableSamples,
                            request.source.preparedOutputGainEnvelope,
                            request.readStartSample);

    return availableSamples;
}

/**
 * 离线 canonical 读取 — 始终读取 44.1kHz truth（Stage2/export）。
 *
 * 1. 非 identity TimeStretch → 从 TimeStretchCache canonical 切片。
 * 2. 否则 canonical dry direct copy + RenderCache::overlayCanonicalAudio。
 * 无插值、无 target rate 参数、无 prepared fallback。
 */
inline int readCanonicalAudio(const CanonicalReadRequest& request,
                               juce::AudioBuffer<float>& destination,
                               int destinationStartSample)
{
    if (request.numSamples <= 0 || !request.source.hasAudio()) {
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

    // ============================================================
    // TimeStretchCache canonical path
    // ============================================================
    const uint64_t objectId = request.source.contentKey.objectId;
    if (!request.source.timeGridIsIdentity
        && request.source.timeStretchCache != nullptr
        && objectId != 0) {
        const int wrote = request.source.timeStretchCache->sliceCanonicalForOutputRange(
            request.source.contentKey,
            request.source.pitchRevision,
            request.source.pitchShiftRevision,
            request.source.timeGridRevision,
            request.readStartSample,
            destination,
            destinationStartSample,
            writableSamples);
        if (wrote > 0) {
            return wrote;
        }
    }

    // ============================================================
    // Dry canonical direct copy
    // ============================================================
    const auto* srcBuf = request.source.audioBuffer.get();
    if (!srcBuf || srcBuf->getNumSamples() <= 0) {
        return 0;
    }

    const int srcLen = srcBuf->getNumSamples();
    const int srcChs = srcBuf->getNumChannels();
    if (srcChs <= 0) return 0;

    const int64_t start = request.readStartSample;
    if (start < 0 || start >= srcLen) {
        return 0;
    }

    const int availableSamples = juce::jmin(writableSamples,
        static_cast<int>(srcLen - start));
    if (availableSamples <= 0) {
        return 0;
    }

    for (int channel = 0; channel < destinationChannels; ++channel) {
        const int srcCh = channel % srcChs;
        destination.copyFrom(channel, destinationStartSample,
                             *srcBuf, srcCh,
                             static_cast<int>(start), availableSamples);
    }

    // ============================================================
    // RenderCache canonical overlay
    // ============================================================
    if (request.source.renderCache != nullptr) {
        request.source.renderCache->overlayCanonicalAudio(destination,
                                                           destinationStartSample,
                                                           availableSamples,
                                                           request.readStartSample);
    }

    return availableSamples;
}

} // namespace OpenTune
