#pragma once

#include "../Content/ContentKey.h"
#include "../Inference/RenderCache.h"
#include "../Inference/TimeStretchCache.h"
#include "../Utils/PitchShiftSettings.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <cstdint>

namespace OpenTune {

/**
 * PlaybackReadSource — 播放时的只读音频来源。
 *
 * 统一定义，使用 ContentKey 标识内容。
 * 优先使用 RenderCache（overlay），降级为原始 audioBuffer（dry）。
 *
 * Phase 0: 提取自 MaterializationStore::PlaybackReadSource 和
 *          ContentRenderService::PlaybackReadSource
 * 关键改变：使用 ContentKey 替代 materializationId
 */
struct PlaybackReadSource
{
    ContentKey contentKey;

    std::shared_ptr<RenderCache> renderCache;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    TimeStretchCache* timeStretchCache{nullptr};

    uint64_t renderRevision{0};
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};

    PitchShiftSettings pitchShiftSettings;
    bool timeGridIsIdentity{true};

    bool hasAudio() const noexcept
    {
        return audioBuffer != nullptr && audioBuffer->getNumSamples() > 0;
    }

    bool canRead() const noexcept
    {
        return renderCache != nullptr || hasAudio();
    }
};

} // namespace OpenTune