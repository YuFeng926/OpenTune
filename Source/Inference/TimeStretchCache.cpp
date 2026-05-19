#include "TimeStretchCache.h"
#include "RenderCache.h"   // §6.3 — shared global LRU pool

#include <algorithm>
#include <cmath>

namespace OpenTune {

TimeStretchCache::TimeStretchCache() = default;
TimeStretchCache::~TimeStretchCache() = default;

void TimeStretchCache::store(uint64_t materializationId,
                              std::vector<float> audio,
                              uint32_t pitchRevision,
                              uint32_t timeGridRevision,
                              double sampleRate)
{
    if (materializationId == 0 || sampleRate <= 0.0) return;

    auto entry = std::make_shared<Entry>();
    entry->audio = std::move(audio);
    entry->pitchRevision = pitchRevision;
    entry->timeGridRevision = timeGridRevision;
    entry->sampleRate = sampleRate;
    entry->published = true;

    const size_t newBytes = entry->audio.size() * sizeof(float);

    // ⚡️ §6.3 (Phase H) — shared LRU pool with RenderCache.
    // Both caches feed the same `RenderCache::globalCacheCurrentBytes()`
    // counter against `globalCacheLimitBytes()` (256 MB default).  When
    // TimeStretchCache pushes total over the limit, RenderCache's per-cache
    // eviction loop will reclaim Stage 1 chunks on its next store call.
    // We don't proactively evict OTHER TimeStretchCache entries here because
    // each materialization holds at most one entry (clip-wide), and replacing
    // is handled below.
    juce::SpinLock::ScopedLockType sl(lock_);

    // Subtract any prior entry's bytes for this materialization.
    auto it = entries_.find(materializationId);
    if (it != entries_.end() && it->second != nullptr && it->second->published) {
        const size_t oldBytes = it->second->audio.size() * sizeof(float);
        if (oldBytes > 0) {
            RenderCache::globalCacheCurrentBytes().fetch_sub(oldBytes, std::memory_order_relaxed);
        }
    }

    entries_[materializationId] = std::move(entry);

    // Account new bytes into shared pool + bump peak.
    const size_t newCurrent = RenderCache::globalCacheCurrentBytes()
                                .fetch_add(newBytes, std::memory_order_relaxed) + newBytes;
    size_t peak = RenderCache::globalCachePeakBytes().load(std::memory_order_relaxed);
    while (newCurrent > peak
           && !RenderCache::globalCachePeakBytes()
                  .compare_exchange_weak(peak, newCurrent, std::memory_order_relaxed)) {}
}

bool TimeStretchCache::hit(uint64_t materializationId,
                            uint32_t pitchRevision,
                            uint32_t timeGridRevision) const
{
    juce::SpinLock::ScopedLockType sl(lock_);
    auto it = entries_.find(materializationId);
    if (it == entries_.end() || !it->second) return false;
    const auto& e = *it->second;
    return e.published
        && e.pitchRevision == pitchRevision
        && e.timeGridRevision == timeGridRevision;
}

int TimeStretchCache::sliceForOutputRange(uint64_t materializationId,
                                           double outputStartSeconds,
                                           juce::AudioBuffer<float>& destination,
                                           int destinationStartSample,
                                           int numSamples,
                                           int targetSampleRate) const
{
    if (numSamples <= 0 || targetSampleRate <= 0) return 0;
    if (destinationStartSample < 0) return 0;
    const int destinationChannels = destination.getNumChannels();
    const int destinationSamples  = destination.getNumSamples();
    if (destinationChannels <= 0 || destinationSamples <= 0) return 0;
    if (destinationStartSample >= destinationSamples) return 0;

    const int writableSamples = std::min(numSamples, destinationSamples - destinationStartSample);
    if (writableSamples <= 0) return 0;

    juce::SpinLock::ScopedTryLockType sl(lock_);
    if (!sl.isLocked()) return 0;

    auto it = entries_.find(materializationId);
    if (it == entries_.end() || !it->second || !it->second->published) return 0;

    const auto& e = *it->second;
    if (e.audio.empty()) return 0;

    const double cacheSampleRate = e.sampleRate;

    // Read by linear interpolation from cached audio at cacheSampleRate.
    // Clamp source sample positions to valid range.
    const int totalCachedSamples = static_cast<int>(e.audio.size());

    int actuallyWritten = 0;
    for (int i = 0; i < writableSamples; ++i) {
        const double t_out = outputStartSeconds + static_cast<double>(i) / static_cast<double>(targetSampleRate);
        const double srcSampleD = t_out * cacheSampleRate;
        if (srcSampleD < 0.0) {
            for (int ch = 0; ch < destinationChannels; ++ch) {
                destination.setSample(ch, destinationStartSample + i, 0.0f);
            }
            ++actuallyWritten;
            continue;
        }

        const int s0 = static_cast<int>(std::floor(srcSampleD));
        if (s0 >= totalCachedSamples) break;
        const int s1 = std::min(s0 + 1, totalCachedSamples - 1);
        const float frac = static_cast<float>(srcSampleD - static_cast<double>(s0));
        const float v = e.audio[static_cast<size_t>(s0)] * (1.0f - frac)
                      + e.audio[static_cast<size_t>(s1)] * frac;

        // Mono cache → broadcast to all destination channels (matches existing
        // OpenTune mono-storage broadcast convention).
        for (int ch = 0; ch < destinationChannels; ++ch) {
            destination.setSample(ch, destinationStartSample + i, v);
        }
        ++actuallyWritten;
    }

    return actuallyWritten;
}

void TimeStretchCache::invalidate(uint64_t materializationId)
{
    juce::SpinLock::ScopedLockType sl(lock_);
    auto it = entries_.find(materializationId);
    if (it != entries_.end() && it->second != nullptr) {
        const size_t oldBytes = it->second->audio.size() * sizeof(float);
        if (oldBytes > 0 && it->second->published) {
            RenderCache::globalCacheCurrentBytes().fetch_sub(oldBytes, std::memory_order_relaxed);
        }
        it->second->published = false;
        it->second->audio.clear();
        it->second->audio.shrink_to_fit();
    }
}

void TimeStretchCache::clear()
{
    juce::SpinLock::ScopedLockType sl(lock_);
    // Decrement any published bytes from shared pool.
    for (auto& [_, e] : entries_) {
        if (e && e->published) {
            const size_t bytes = e->audio.size() * sizeof(float);
            if (bytes > 0) {
                RenderCache::globalCacheCurrentBytes().fetch_sub(bytes, std::memory_order_relaxed);
            }
        }
    }
    entries_.clear();
}

TimeStretchCache::Stats TimeStretchCache::getStats() const
{
    Stats s;
    juce::SpinLock::ScopedLockType sl(lock_);
    s.materializationCount = static_cast<int>(entries_.size());
    for (const auto& [_, e] : entries_) {
        if (!e) continue;
        if (e->published) ++s.publishedCount;
        s.totalBytes += e->audio.capacity() * sizeof(float);
    }
    return s;
}

} // namespace OpenTune
