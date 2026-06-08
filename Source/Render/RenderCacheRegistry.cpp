#include "RenderCacheRegistry.h"

namespace OpenTune {

std::shared_ptr<RenderCache> RenderCacheRegistry::getOrCreate(ContentKey key)
{
    // 先读锁查找
    {
        const juce::ScopedReadLock readLock(lock_);
        auto it = caches_.find(key);
        if (it != caches_.end())
            return it->second;
    }

    // 未找到，升级写锁创建
    auto cache = std::make_shared<RenderCache>();
    {
        const juce::ScopedWriteLock writeLock(lock_);
        auto [it, inserted] = caches_.try_emplace(key, cache);
        return it->second;
    }
}

std::shared_ptr<RenderCache> RenderCacheRegistry::get(ContentKey key) const
{
    const juce::ScopedReadLock readLock(lock_);
    auto it = caches_.find(key);
    return (it != caches_.end()) ? it->second : nullptr;
}

void RenderCacheRegistry::put(ContentKey key, std::shared_ptr<RenderCache> cache)
{
    const juce::ScopedWriteLock writeLock(lock_);
    caches_[key] = std::move(cache);
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

} // namespace OpenTune
