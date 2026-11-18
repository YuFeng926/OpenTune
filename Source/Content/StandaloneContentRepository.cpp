#include "StandaloneContentRepository.h"

namespace OpenTune {

StandaloneContentRepository::StandaloneContentRepository() = default;
StandaloneContentRepository::~StandaloneContentRepository() = default;

ContentKey StandaloneContentRepository::createClip(StandaloneClipId forcedId)
{
    juce::ScopedWriteLock wl(lock_);

    StandaloneClipId clipId = forcedId;
    if (clipId == 0) {
        // 工程恢复的 ID 与自动分配共用仓库；已退休但仍保留的 ID 同样占用。
        while (nextClipId_ == 0 || clips_.find(nextClipId_) != clips_.end())
            ++nextClipId_;
        clipId = nextClipId_++;
    } else if (clips_.find(clipId) != clips_.end()) {
        return {};  // ID 已存在
    }

    auto content = std::make_unique<StandaloneClipContent>(clipId);
    const ContentKey key = content->contentKey();
    clips_[clipId] = std::move(content);
    return key;
}

StandaloneClipContent* StandaloneContentRepository::findClipImpl(StandaloneClipId clipId) const
{
    const auto it = clips_.find(clipId);
    return (it != clips_.end()) ? it->second.get() : nullptr;
}

StandaloneClipContent* StandaloneContentRepository::findClip(ContentKey key) const
{
    if (key.domainKind != DomainKind::StandaloneClip)
        return nullptr;

    juce::ScopedReadLock rl(lock_);
    return findClipImpl(static_cast<StandaloneClipId>(key.objectId));
}

bool StandaloneContentRepository::retireClip(ContentKey key)
{
    if (key.domainKind != DomainKind::StandaloneClip)
        return false;

    const StandaloneClipId clipId = static_cast<StandaloneClipId>(key.objectId);

    juce::ScopedWriteLock wl(lock_);
    auto* clip = findClipImpl(clipId);
    if (!clip)
        return false;

    clip->retireContent(key);
    return true;
}

bool StandaloneContentRepository::reviveClip(ContentKey key)
{
    if (key.domainKind != DomainKind::StandaloneClip)
        return false;

    const StandaloneClipId clipId = static_cast<StandaloneClipId>(key.objectId);

    juce::ScopedWriteLock wl(lock_);
    auto* clip = findClipImpl(clipId);
    if (!clip)
        return false;

    clip->reviveContent(key);
    return true;
}

bool StandaloneContentRepository::releaseClip(ContentKey key)
{
    if (key.domainKind != DomainKind::StandaloneClip)
        return false;

    const StandaloneClipId clipId = static_cast<StandaloneClipId>(key.objectId);

    juce::ScopedWriteLock wl(lock_);
    auto* clip = findClipImpl(clipId);
    if (!clip)
        return false;

    clip->releaseRetiredContent(key);
    // 物理删除仅在已是 retired 状态且无外部引用时执行
    if (!clip->hasActiveContent()) {
        clips_.erase(clipId);
    }
    return true;
}

bool StandaloneContentRepository::isRetired(ContentKey key) const
{
    if (key.domainKind != DomainKind::StandaloneClip)
        return false;

    juce::ScopedReadLock rl(lock_);
    const auto* clip = findClipImpl(static_cast<StandaloneClipId>(key.objectId));
    if (!clip)
        return false;

    return clip->isRetired();
}

std::vector<ContentKey> StandaloneContentRepository::getRetiredClips() const
{
    juce::ScopedReadLock rl(lock_);
    std::vector<ContentKey> result;
    for (const auto& [id, clip] : clips_) {
        if (clip && clip->isRetired()) {
            result.push_back(clip->contentKey());
        }
    }
    return result;
}

std::vector<ContentKey> StandaloneContentRepository::getAllClips() const
{
    juce::ScopedReadLock rl(lock_);
    std::vector<ContentKey> result;
    for (const auto& [id, clip] : clips_) {
        if (clip) {
            result.push_back(clip->contentKey());
        }
    }
    return result;
}

const StandaloneRetiredContentRecord* StandaloneContentRepository::getRetiredContent(ContentKey key) const
{
    if (key.domainKind != DomainKind::StandaloneClip)
        return nullptr;

    juce::ScopedReadLock rl(lock_);
    const auto* clip = findClipImpl(static_cast<StandaloneClipId>(key.objectId));
    return clip ? clip->getRetiredContent(key) : nullptr;
}

bool StandaloneContentRepository::isSourceReferenced(uint64_t sourceId) const
{
    if (sourceId == 0)
        return false;

    juce::ScopedReadLock rl(lock_);
    for (const auto& [id, clip] : clips_) {
        if (clip->hasActiveContent()
            && clip->content().sourceWindow.sourceId == sourceId)
            return true;

        const auto* retired = clip->getRetiredContent(clip->contentKey());
        if (retired != nullptr && retired->content.sourceWindow.sourceId == sourceId)
            return true;
    }

    return false;
}

void StandaloneContentRepository::clear()
{
    juce::ScopedWriteLock wl(lock_);
    clips_.clear();
    nextClipId_ = 1;
#if JucePlugin_Build_Standalone && defined(OPENTUNE_ENABLE_CONTROL_API)
    // Invalidate handles from the previous project session.
    contentEpoch_.fetch_add(1, std::memory_order_relaxed);
#endif
}

} // namespace OpenTune
