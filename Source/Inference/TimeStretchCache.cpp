#include "TimeStretchCache.h"
#include "RenderCache.h"   // §6.3 — shared global LRU pool

#include <algorithm>
#include <cmath>

namespace OpenTune {

TimeStretchCache::TimeStretchCache() = default;
TimeStretchCache::~TimeStretchCache() = default;

uint32_t TimeStretchCache::beginBuild(ContentKey key) const
{
    if (!key.isValid()) return 0;

    juce::SpinLock::ScopedLockType sl(lock_);
    auto [it, _] = invalidationGen_.try_emplace(key, 0);
    return it->second;
}

void TimeStretchCache::store(ContentKey key,
                              std::vector<float> audio,
                              uint64_t pitchRevision,
                              uint64_t pitchShiftRevision,
                              uint64_t timeGridRevision,
                              double sampleRate,
                              uint32_t buildGeneration)
{
    if (!key.isValid() || sampleRate <= 0.0) return;

    // ⚡️ Acquire lock before generation check.
    juce::SpinLock::ScopedLockType sl(lock_);

    // Reject stale build output if invalidation occurred during build,
    // or if the content is unknown (e.g. after clear()).
    auto genIt = invalidationGen_.find(key);
    if (genIt == invalidationGen_.end() || genIt->second != buildGeneration) {
        return; // Stale output — unknown or mismatched generation.
    }

    auto entry = std::make_shared<Entry>();
    entry->audio = std::move(audio);
    entry->pitchRevision = pitchRevision;
    entry->pitchShiftRevision = pitchShiftRevision;
    entry->timeGridRevision = timeGridRevision;
    entry->sampleRate = sampleRate;
    entry->published = true;

    const size_t newBytes = entry->audio.size() * sizeof(float);

    // Subtract any prior entry's bytes for this content.
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second != nullptr && it->second->published) {
        const size_t oldBytes = it->second->audio.size() * sizeof(float);
        if (oldBytes > 0) {
            RenderCache::globalCacheCurrentBytes().fetch_sub(oldBytes, std::memory_order_relaxed);
        }
    }

    entries_[key] = std::move(entry);

    // Account new bytes into shared pool + bump peak.
    const size_t newCurrent = RenderCache::globalCacheCurrentBytes()
                                .fetch_add(newBytes, std::memory_order_relaxed) + newBytes;
    size_t peak = RenderCache::globalCachePeakBytes().load(std::memory_order_relaxed);
    while (newCurrent > peak
           && !RenderCache::globalCachePeakBytes()
                  .compare_exchange_weak(peak, newCurrent, std::memory_order_relaxed)) {}

    // Publish atomic snapshot for lock-free readers.
    auto snapshot = std::make_shared<const std::map<ContentKey, std::shared_ptr<Entry>>>(entries_);
    auto old = std::atomic_exchange(&readerMap_, std::move(snapshot));
    if (old)
        retiredSnapshots_.push_back(std::move(old));
    retiredSnapshots_.erase(
        std::remove_if(retiredSnapshots_.begin(), retiredSnapshots_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredSnapshots_.end());
}

bool TimeStretchCache::hit(ContentKey key,
                            uint64_t pitchRevision,
                            uint64_t pitchShiftRevision,
                            uint64_t timeGridRevision) const
{
    auto snap = std::atomic_load(&readerMap_);
    if (!snap) return false;
    auto it = snap->find(key);
    if (it == snap->end() || !it->second) return false;
    const auto& e = *it->second;
    return e.published
        && e.pitchRevision == pitchRevision
        && e.pitchShiftRevision == pitchShiftRevision
        && e.timeGridRevision == timeGridRevision;
}

int TimeStretchCache::sliceForOutputRange(ContentKey key,
                                           uint64_t pitchRevision,
                                           uint64_t pitchShiftRevision,
                                           uint64_t timeGridRevision,
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

    auto snap = std::atomic_load(&readerMap_);
    if (!snap) return 0;
    auto it = snap->find(key);
    if (it == snap->end() || !it->second || !it->second->published) return 0;

    const auto& e = *it->second;
    if (e.audio.empty()) return 0;

    // Revision validation: reject stale cache entries.
    if (e.pitchRevision != pitchRevision
        || e.pitchShiftRevision != pitchShiftRevision
        || e.timeGridRevision != timeGridRevision)
        return 0;

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

void TimeStretchCache::invalidate(ContentKey key)
{
    juce::SpinLock::ScopedLockType sl(lock_);
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second != nullptr) {
        const size_t oldBytes = it->second->audio.size() * sizeof(float);
        if (oldBytes > 0 && it->second->published) {
            RenderCache::globalCacheCurrentBytes().fetch_sub(oldBytes, std::memory_order_relaxed);
        }
        // Replace with fresh empty entry instead of modifying in-place,
        // so that any old atomic snapshot remains undisturbed.
        it->second = std::make_shared<Entry>();
    }
    ++invalidationGen_[key];

    // Publish atomic snapshot for lock-free readers.
    auto snapshot = std::make_shared<const std::map<ContentKey, std::shared_ptr<Entry>>>(entries_);
    auto old = std::atomic_exchange(&readerMap_, std::move(snapshot));
    if (old)
        retiredSnapshots_.push_back(std::move(old));
    retiredSnapshots_.erase(
        std::remove_if(retiredSnapshots_.begin(), retiredSnapshots_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredSnapshots_.end());
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
    invalidationGen_.clear();

    // Publish empty atomic snapshot for lock-free readers.
    auto snapshot = std::make_shared<const std::map<ContentKey, std::shared_ptr<Entry>>>();
    auto old = std::atomic_exchange(&readerMap_, std::move(snapshot));
    if (old)
        retiredSnapshots_.push_back(std::move(old));
    retiredSnapshots_.erase(
        std::remove_if(retiredSnapshots_.begin(), retiredSnapshots_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredSnapshots_.end());
}

TimeStretchCache::Stats TimeStretchCache::getStats() const
{
    Stats s;
    juce::SpinLock::ScopedLockType sl(lock_);
    s.contentCount = static_cast<int>(entries_.size());
    for (const auto& [_, e] : entries_) {
        if (!e) continue;
        if (e->published) ++s.publishedCount;
        s.totalBytes += e->audio.capacity() * sizeof(float);
    }
    return s;
}

} // namespace OpenTune
