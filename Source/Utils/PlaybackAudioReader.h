#pragma once

#include "../Render/ContentRenderService.h"
#include "TimeCoordinate.h"
#include "../Inference/TimeStretchCache.h"
#include "../Inference/RenderCache.h"
#include "../DSP/NoteEqProcessor.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

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
 * AutomationLane 增益应用：output-time 经 TimeGrid 映射到 source-time 后逐样本 evalAt。
 * 无分配、无锁；包络为空 = 单位增益。
 */
inline void applyAutomationGain(juce::AudioBuffer<float>& destination,
                                int destinationStartSample,
                                int numSamples,
                                const std::shared_ptr<const AutomationLane>& envelope,
                                const std::shared_ptr<const TimeGridSnapshot>& timeGrid,
                                int64_t readStartSample,
                                double targetSampleRate)
{
    if (envelope == nullptr || envelope->empty())
        return;

    constexpr float kDbToLinear = 0.11512925465f; // ln(10) / 20
    const int channels = destination.getNumChannels();
    for (int s = 0; s < numSamples; ++s) {
        const double outputSeconds = static_cast<double>(readStartSample + s) / targetSampleRate;
        const double sourceSeconds = timeGrid != nullptr
            ? timeGrid->tauInverse(outputSeconds)
            : outputSeconds;
        const float gainLinear = std::exp(envelope->evalAt(sourceSeconds) * kDbToLinear);
        for (int channel = 0; channel < channels; ++channel) {
            destination.getWritePointer(channel, destinationStartSample + s)[0] *= gainLinear;
        }
    }
}

/**
 * Per-note EQ processing: apply EQ to audio buffer based on note time ranges.
 * 
 * For each sample, find which note it belongs to and apply that note's EQ settings.
 * If a sample doesn't belong to any note, no EQ is applied.
 * If a note has no EQ settings (nullopt), no EQ is applied.
 */
inline void applyPerNoteEq(juce::AudioBuffer<float>& destination,
                           int destinationStartSample,
                           int numSamples,
                           const std::vector<Note>* notes,
                           const std::shared_ptr<const TimeGridSnapshot>& timeGrid,
                           int64_t readStartSample,
                           double targetSampleRate)
{
    if (notes == nullptr || notes->empty())
        return;
    
    // Process in chunks for each note to avoid per-sample filter state issues
    for (int s = 0; s < numSamples; ) {
        // Find which note this sample belongs to
        const double outputSeconds = static_cast<double>(readStartSample + s) / targetSampleRate;
        const double sourceSeconds = timeGrid != nullptr
            ? timeGrid->tauInverse(outputSeconds)
            : outputSeconds;
        
        // Find the note at this time
        const Note* activeNote = nullptr;
        for (const auto& note : *notes) {
            if (sourceSeconds >= note.startTime && sourceSeconds < note.endTime) {
                activeNote = &note;
                break;
            }
        }
        
        if (activeNote == nullptr || !activeNote->eq.has_value() || !activeNote->eq->active) {
            // No EQ for this sample, skip ahead
            ++s;
            continue;
        }
        
        // Find how many samples belong to this note
        const double noteEndSeconds = activeNote->endTime;
        double noteEndOutputSeconds = noteEndSeconds;
        if (timeGrid != nullptr) {
            noteEndOutputSeconds = timeGrid->tauForward(noteEndSeconds);
        }
        const int64_t noteEndSample = static_cast<int64_t>(noteEndOutputSeconds * targetSampleRate);
        const int chunkSize = static_cast<int>(juce::jmin(
            static_cast<int64_t>(numSamples - s),
            noteEndSample - (readStartSample + s)));
        
        if (chunkSize <= 0) {
            ++s;
            continue;
        }
        
        // Apply EQ to this chunk
        NoteEqProcessor processor;
        processor.prepare(targetSampleRate, chunkSize);
        processor.updateCoefficients(*activeNote->eq);
        
        // Extract chunk, process, and put back
        juce::AudioBuffer<float> chunk(destination.getNumChannels(), chunkSize);
        for (int ch = 0; ch < destination.getNumChannels(); ++ch) {
            chunk.copyFrom(ch, 0, destination, ch, destinationStartSample + s, chunkSize);
        }
        
        processor.process(chunk);
        
        for (int ch = 0; ch < destination.getNumChannels(); ++ch) {
            destination.copyFrom(ch, destinationStartSample + s, chunk, ch, 0, chunkSize);
        }
        
        s += chunkSize;
    }
}

/**
 * 实时播放读取 — 纯 direct copy，无插值。
 *
 * 1. TimeStretchCache fast-path：从 prepared 缓存直接整数切片。
 * 2. 否则从 preparedDry buffer 直接 copy（已在 prepare 阶段由 r8brain 重采样）。
 * 3. 然后从 RenderCache prepared chunks overlay（同样直接 copy）。
 * 4. 两路径汇合到同一 applyAutomationGain() 收尾。
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
    if (request.source.timeGrid != nullptr
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
            applyAutomationGain(destination, destinationStartSample, wrote,
                                request.source.volumeEnvelope,
                                request.source.timeGrid,
                                request.readStartSample,
                                request.targetSampleRate);
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
    // Per-note EQ processing
    // ============================================================
    applyPerNoteEq(destination, destinationStartSample, availableSamples,
                   request.source.notes,
                   request.source.timeGrid,
                   request.readStartSample,
                   request.targetSampleRate);

    // ============================================================
    // 统一最终增益收尾
    // ============================================================
    applyAutomationGain(destination, destinationStartSample, availableSamples,
                        request.source.volumeEnvelope,
                        request.source.timeGrid,
                        request.readStartSample,
                        request.targetSampleRate);

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
    if (request.source.timeGrid != nullptr
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
