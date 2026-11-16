#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdint>
#include <memory>

namespace OpenTune {

/// 原始采样率（native）PCM 值元数据。
/// buffer 不可变：shared_ptr 指向的 buffer 在生命周期内不重建，地址可直接作为
/// source PCM 身份（地址复用安全）；同一 buffer 的内容版本由 audioRevision 区分。
/// firstSourceSample 是 buffer 本地 sample 0 在全局 source sample 网格上的位置。
struct NativePcmSource
{
    std::shared_ptr<const juce::AudioBuffer<float>> buffer;
    double sampleRate{0.0};
    int64_t firstSourceSample{0};
    uint64_t audioRevision{0};

    bool operator==(const NativePcmSource& other) const noexcept
    {
        return buffer == other.buffer && sampleRate == other.sampleRate
            && firstSourceSample == other.firstSourceSample
            && audioRevision == other.audioRevision;
    }
};

} // namespace OpenTune
