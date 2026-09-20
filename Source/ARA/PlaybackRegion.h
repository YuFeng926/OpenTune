#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace OpenTune {

// DC 内部 host object 索引/生命周期查找用。
// placement 属性（时间、timestretch、fade、effective color、归属 modification）
// 一律实时读取 juce::ARAPlaybackRegion*，且只在 DC 消息线程 projection/render
// plan 构建路径读取；音频线程/RenderWorker 只消费 projection 快照。
struct PlaybackRegion
{
    juce::ARAPlaybackRegion* playbackRegion{nullptr};

    bool hasValidPlacement() const noexcept;
};

} // namespace OpenTune
