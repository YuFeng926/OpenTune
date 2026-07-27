#include "RenderCacheRegistry.h"

namespace OpenTune {

std::shared_ptr<RenderCache> RenderCacheRegistry::getOrCreate(ContentKey key)
{
    {
        const juce::ScopedReadLock readLock(lock_);
        auto it = caches_.find(key);
        if (it != caches_.end())
            return it->second;
    }

    auto cache = std::make_shared<RenderCache>();
    double targetSr = 0.0;
    {
        const juce::ScopedWriteLock writeLock(lock_);
        auto [it, inserted] = caches_.try_emplace(key, cache);
        targetSr = currentTargetRate_;
        if (!inserted) cache = it->second;
    }

    if (targetSr > 0.0 && cache) {
        cache->prepareForPlaybackSampleRate(targetSr);
    }
    return cache;
}

std::shared_ptr<RenderCache> RenderCacheRegistry::get(ContentKey key) const
{
    const juce::ScopedReadLock readLock(lock_);
    auto it = caches_.find(key);
    return (it != caches_.end()) ? it->second : nullptr;
}

void RenderCacheRegistry::remove(ContentKey key)
{
    const juce::ScopedWriteLock writeLock(lock_);
    caches_.erase(key);
}

void RenderCacheRegistry::invalidate(ContentKey key)
{
    const juce::ScopedWriteLock writeLock(lock_);
    auto it = caches_.find(key);
    if (it != caches_.end())
        it->second->clear();
}

void RenderCacheRegistry::clear()
{
    const juce::ScopedWriteLock writeLock(lock_);
    caches_.clear();
}

void RenderCacheRegistry::preparePlaybackSampleRate(double targetSr)
{
    if (targetSr <= 0.0) return;

    std::vector<std::shared_ptr<RenderCache>> snapshot;
    {
        const juce::ScopedWriteLock writeLock(lock_);
        currentTargetRate_ = targetSr;
        snapshot.reserve(caches_.size());
        for (auto& [key, cache] : caches_) {
            juce::ignoreUnused(key);
            if (cache) snapshot.push_back(cache);
        }
    }
    // r8brain outside lock
    for (auto& cache : snapshot) {
        cache->prepareForPlaybackSampleRate(targetSr);
    }
}

} // namespace OpenTune
