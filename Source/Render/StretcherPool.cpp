#include "StretcherPool.h"
#include "../Inference/SoundTouchStretcher.h"

namespace OpenTune {

SoundTouchStretcher* StretcherPool::getOrCreate(ContentKey key, double sampleRate, int channels)
{
    if (!key.isValid() || sampleRate <= 0.0 || channels <= 0)
        return nullptr;

    // Fast path: read lock + check existing with matching params
    {
        const juce::ScopedReadLock readLock(lock_);
        const auto it = entries_.find(key);
        if (it != entries_.end()
            && it->second.stretcher != nullptr
            && it->second.sampleRate == sampleRate
            && it->second.channels == channels)
        {
            return it->second.stretcher.get();
        }
    }

    // Slow path: write lock + lazy construct or rebuild
    const juce::ScopedWriteLock writeLock(lock_);
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        // 新規エントリ
        Entry entry;
        entry.sampleRate = sampleRate;
        entry.channels = channels;
        entry.stretcher = std::make_unique<SoundTouchStretcher>(sampleRate, channels);
        it = entries_.emplace(key, std::move(entry)).first;
        return it->second.stretcher.get();
    }

    auto& entry = it->second;

    // パラメータが変わったら再構築
    if (entry.sampleRate != sampleRate || entry.channels != channels)
    {
        entry.stretcher.reset();
        entry.sampleRate = sampleRate;
        entry.channels = channels;
    }

    if (!entry.stretcher)
        entry.stretcher = std::make_unique<SoundTouchStretcher>(sampleRate, channels);

    return entry.stretcher.get();
}

void StretcherPool::remove(ContentKey key)
{
    const juce::ScopedWriteLock writeLock(lock_);
    entries_.erase(key);
}

void StretcherPool::clear()
{
    const juce::ScopedWriteLock writeLock(lock_);
    entries_.clear();
}

} // namespace OpenTune
