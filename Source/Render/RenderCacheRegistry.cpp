#include "RenderCacheRegistry.h"
#include "../Utils/AppLogger.h"

namespace OpenTune {

std::shared_ptr<RenderCache> RenderCacheRegistry::getOrCreate(ContentKey key)
{
    const std::lock_guard<std::mutex> prepareGuard(prepareMutex_);
    {
        const juce::ScopedReadLock readLock(lock_);
        auto it = caches_.find(key);
        if (it != caches_.end())
            return it->second;
    }

    auto cache = std::make_shared<RenderCache>();
    double targetSr = 0.0;
    {
        const juce::ScopedReadLock readLock(lock_);
        targetSr = currentTargetRate_;
    }

    if (targetSr > 0.0 && cache) {
        if (!cache->prepareForPlaybackSampleRate(targetSr))
            return nullptr;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    auto [it, inserted] = caches_.try_emplace(key, cache);
    if (!inserted)
        return it->second;
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
    std::shared_ptr<RenderCache> cache;
    {
        const juce::ScopedWriteLock writeLock(lock_);
        const auto it = caches_.find(key);
        if (it == caches_.end())
            return;
        cache = std::move(it->second);
        caches_.erase(it);
    }
    cache->clear();
}

void RenderCacheRegistry::clear()
{
    std::map<ContentKey, std::shared_ptr<RenderCache>> cachesToClear;
    {
        const juce::ScopedWriteLock writeLock(lock_);
        cachesToClear.swap(caches_);
    }
    for (auto& entry : cachesToClear)
        entry.second->clear();
}

void RenderCacheRegistry::collectPlaybackSnapshots()
{
    std::vector<std::shared_ptr<RenderCache>> caches;
    {
        const juce::ScopedReadLock readLock(lock_);
        caches.reserve(caches_.size());
        for (const auto& [key, cache] : caches_) {
            juce::ignoreUnused(key);
            if (cache)
                caches.push_back(cache);
        }
    }

    for (const auto& cache : caches)
        cache->collectPlaybackSnapshots();
}

bool RenderCacheRegistry::preparePlaybackSampleRate(double targetSr)
{
    if (targetSr <= 0.0) return false;

    const std::lock_guard<std::mutex> prepareGuard(prepareMutex_);
    std::vector<std::shared_ptr<RenderCache>> snapshot;
    {
        const juce::ScopedReadLock readLock(lock_);
        snapshot.reserve(caches_.size());
        for (auto& [key, cache] : caches_) {
            juce::ignoreUnused(key);
            if (cache) snapshot.push_back(cache);
        }
    }
    // r8brain outside lock
    struct PreparedChange {
        std::shared_ptr<RenderCache> cache;
        RenderCache::PreparedSnapshotRollback previous;
    };
    std::vector<PreparedChange> prepared;
    prepared.reserve(snapshot.size());
    for (auto& cache : snapshot) {
        if (cache) {
            auto previous = cache->capturePreparedSnapshot();
            if (!cache->prepareForPlaybackSampleRate(targetSr)) {
                for (auto& preparedChange : prepared) {
                    if (!preparedChange.cache
                        || !preparedChange.cache->restorePreparedSnapshot(
                            std::move(preparedChange.previous))) {
                    AppLogger::error("RenderCacheRegistry: prepared snapshot rollback failed");
                    std::terminate();
                    }
                }
                return false;
            }
            prepared.push_back({cache, std::move(previous)});
        }
    }

    const juce::ScopedWriteLock writeLock(lock_);
    currentTargetRate_ = targetSr;
    return true;
}

} // namespace OpenTune
