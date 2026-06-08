#include "PlaybackSourcePublisher.h"

namespace OpenTune {

void PlaybackSourcePublisher::publish(ContentKey key, PlaybackReadSource source)
{
    source.contentKey = key;
    {
        const juce::ScopedWriteLock wl(lock_);
        sources_[key] = std::move(source);
    }
    rebuildSnapshot();
}

bool PlaybackSourcePublisher::get(ContentKey key, PlaybackReadSource& out) const noexcept
{
    auto snap = std::atomic_load(&snapshot_);
    if (!snap) return false;
    auto it = snap->find(key);
    if (it == snap->end()) return false;
    out = it->second;
    return true;
}

void PlaybackSourcePublisher::remove(ContentKey key)
{
    const juce::ScopedWriteLock wl(lock_);
    sources_.erase(key);
    rebuildSnapshot();
}

void PlaybackSourcePublisher::clear()
{
    const juce::ScopedWriteLock wl(lock_);
    sources_.clear();
    rebuildSnapshot();
}

void PlaybackSourcePublisher::rebuildSnapshot()
{
    auto snap = std::make_shared<const std::map<ContentKey, PlaybackReadSource>>(sources_);
    std::atomic_store(&snapshot_, snap);
}

} // namespace OpenTune
