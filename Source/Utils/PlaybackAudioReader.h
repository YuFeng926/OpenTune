#pragma once

#include "../Render/ContentRenderService.h"
#include "TimeCoordinate.h"
#include "../Inference/TimeStretchCache.h"
#include "../Inference/RenderCache.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

namespace OpenTune {

/**
 * PlaybackReadRequest — 实时读取请求，使用目标采样率下的绝对样本位置。
 *
 * readStartSample 是 output/prepared sample 空间中的绝对样本偏移。
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
 * CanonicalReadRequest — 离线 canonical 读取请求（Stage2 source-domain Stage1 读取）。
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
 * AutomationLane 增益应用：output seconds 经 snapshot->timeGrid->tauInverse
 * 映射到 source seconds 后逐样本 evalAt。无分配、无锁；包络为空 = 单位增益。
 * snapshot->timeGrid 由调用方保证非空。
 */
inline void applyAutomationGain(juce::AudioBuffer<float>& destination,
                                int destinationStartSample,
                                int numSamples,
                                const EditableContentSnapshot& snapshot,
                                int64_t readStartSample,
                                double targetSampleRate)
{
    const auto& envelope = snapshot.volumeEnvelope;
    if (envelope.empty())
        return;

    constexpr float kDbToLinear = 0.11512925465f; // ln(10) / 20
    const int channels = destination.getNumChannels();
    for (int s = 0; s < numSamples; ++s) {
        const double outputSeconds = static_cast<double>(readStartSample + s) / targetSampleRate;
        const double sourceSeconds = snapshot.timeGrid->tauInverse(outputSeconds);
        const float gainLinear = std::exp(envelope.evalAt(sourceSeconds) * kDbToLinear);
        for (int channel = 0; channel < channels; ++channel) {
            destination.getWritePointer(channel, destinationStartSample + s)[0] *= gainLinear;
        }
    }
}

/**
 * 实时播放读取 — 纯 direct copy，无插值。
 *
 * 全部 editable 数据（timeGrid/volumeEnvelope/contentRevision）来自
 * request.source.contentSnapshot；调用方保证 snapshot 与其 timeGrid 非空。
 *
 * 合同：readStartSample 是 output/prepared sample 位置。
 * 1. 非恒等 timeGrid：只尝试 TimeStretchCache prepared 切片；cache miss 或
 *    版本不匹配立即返回 0，绝不读 preparedDry。
 * 2. 恒等 timeGrid：从 preparedDry 直接 copy（已在 prepare 阶段重采样），
 *    再按 snapshot->contentRevision 从 RenderCache prepared chunks overlay。
 * 3. 汇合到 applyAutomationGain() 收尾。
 *
 * 无 canonical fallback。prepared 数据不存在时返回 0。
 *
 * per-note EQ 的唯一处理位置在 Stage1 写入缓存前（ProcessRenderRuntime
 * publishChunkWithPerNoteEq），本读取路径不含任何 EQ 代码。
 */
inline int readPlaybackAudio(const PlaybackReadRequest& request,
                             juce::AudioBuffer<float>& destination,
                             int destinationStartSample)
{
    const auto& snapshot = request.source.contentSnapshot;
    if (request.numSamples <= 0
        || request.targetSampleRate <= 0.0
        || !request.source.hasAudio()
        || snapshot == nullptr) {
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
    // 非恒等 timeGrid — 只走 TimeStretchCache prepared 切片；
    // miss/版本不匹配立即返回，不读 preparedDry。
    // ============================================================
    if (!snapshot->timeGrid->isIdentity()) {
        if (request.source.timeStretchCache == nullptr)
            return 0;

        const int wrote = request.source.timeStretchCache->sliceForOutputRange(
            request.source.contentKey,
            snapshot->contentRevision,
            snapshot->timeGridRevision,
            request.readStartSample,
            destination,
            destinationStartSample,
            writableSamples,
            static_cast<int>(request.targetSampleRate));
        if (wrote <= 0)
            return 0;

        applyAutomationGain(destination, destinationStartSample, wrote,
                            *snapshot,
                            request.readStartSample,
                            request.targetSampleRate);
        return wrote;
    }

    // ============================================================
    // 恒等 timeGrid — 从 preparedDry 直接 copy（已在 prepare 阶段重采样）
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
    // RenderCache prepared overlay — 只复制匹配 contentRevision 的 chunk
    // ============================================================
    if (request.source.renderCache != nullptr) {
        request.source.renderCache->overlayPreparedAudio(destination,
                                                          destinationStartSample,
                                                          availableSamples,
                                                          request.readStartSample,
                                                          static_cast<int>(request.targetSampleRate),
                                                          snapshot->contentRevision);
    }

    // ============================================================
    // 统一最终增益收尾
    // ============================================================
    applyAutomationGain(destination, destinationStartSample, availableSamples,
                        *snapshot,
                        request.readStartSample,
                        request.targetSampleRate);

    return availableSamples;
}

/**
 * 离线 canonical 读取 — 只作为 Stage2 source-domain Stage1 canonical 读取。
 *
 * 1. 从 source audioBuffer 直接 copy（canonical 44.1kHz truth）。
 * 2. RenderCache::overlayCanonicalAudio 叠加 Stage1 渲染结果。
 *
 * 不使用 TimeStretchCache（Stage2 必须读 Stage1 原始 PCM，不能读自己的输出），
 * 不要求 snapshot timeGrid 参与。无插值、无 target rate 参数、无 prepared fallback。
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
