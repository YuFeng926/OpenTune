#pragma once

#include "PlaybackReadSource.h"
#include "../Content/ContentKey.h"
#include <juce_core/juce_core.h>
#include <map>
#include <memory>

namespace OpenTune {

/**
 * PlaybackSourcePublisher — lock-free playback source snapshot publisher.
 * 
 * 发布 audio-thread 可无锁读取的 PlaybackReadSource snapshot。
 * 使用 atomic shared_ptr 模式，writer 可用锁，reader 完全无锁。
 * 
 * Phase 0: 提取自 MaterializationStore::playbackSourceCache_ (L317-320)
 *          和 ContentRenderService::playbackSourceCache_ (L178-179)
 * 
 * 复用代码：
 * - ContentRenderService.cpp:21-59 (rebuildPlaybackSourceCache + publish/remove/clear)
 */
class PlaybackSourcePublisher
{
public:
    PlaybackSourcePublisher() = default;
    ~PlaybackSourcePublisher() = default;

    PlaybackSourcePublisher(const PlaybackSourcePublisher&) = delete;
    PlaybackSourcePublisher& operator=(const PlaybackSourcePublisher&) = delete;

    void publish(ContentKey key, PlaybackReadSource source);
    bool get(ContentKey key, PlaybackReadSource& out) const noexcept;
    void remove(ContentKey key);
    void clear();

private:
    void rebuildSnapshot();

    juce::ReadWriteLock lock_;
    std::map<ContentKey, PlaybackReadSource> sources_;
    mutable std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>> snapshot_;
};

} // namespace OpenTune
