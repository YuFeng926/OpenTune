#include "TimeStretchCache.h"
#include "RenderCache.h"
#include "../DSP/ResamplingManager.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

size_t TimeStretchCache::entryTotalBytes(const Entry& e) {
    if (!e.canonicalAudio || e.canonicalAudio->empty()) return 0;
    size_t total = e.canonicalAudio->size() * sizeof(float);
    if (e.preparedAudio && !e.preparedAudio->empty() && e.preparedAudio != e.canonicalAudio) {
        total += e.preparedAudio->size() * sizeof(float);
    }
    return total;
}

TimeStretchCache::TimeStretchCache() = default;
TimeStretchCache::~TimeStretchCache() { clear(); }

uint32_t TimeStretchCache::beginBuild(ContentKey key)
{
    if (!key.isValid()) return 0;

    std::lock_guard<std::mutex> lg(mutex_);
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

    std::lock_guard<std::mutex> lg(mutex_);

    auto genIt = invalidationGen_.find(key);
    if (genIt == invalidationGen_.end() || genIt->second != buildGeneration) {
        return;
    }

    auto entry = std::make_shared<Entry>();
    entry->canonicalAudio = std::make_shared<const std::vector<float>>(std::move(audio));
    entry->pitchRevision = pitchRevision;
    entry->pitchShiftRevision = pitchShiftRevision;
    entry->timeGridRevision = timeGridRevision;
    entry->sampleRate = sampleRate;
    entry->published = true;

    // Compute prepared at current target rate (inside mutex)
    double targetSr = targetSampleRate_;
    if (targetSr > 0.0) {
        if (std::abs(targetSr - sampleRate) < 1.0) {
            entry->preparedAudio = entry->canonicalAudio;
            entry->preparedSampleRate = targetSr;
        } else {
            const int outputLength = static_cast<int>(
                std::round(static_cast<double>(entry->canonicalAudio->size()) * targetSr / sampleRate));
            if (outputLength > 0) {
                std::vector<float> resampled = preparedResampler_.resampleExactLength(
                    entry->canonicalAudio->data(),
                    entry->canonicalAudio->size(),
                    static_cast<int>(sampleRate),
                    static_cast<int>(targetSr),
                    outputLength);
                entry->preparedAudio = std::make_shared<const std::vector<float>>(std::move(resampled));
                entry->preparedSampleRate = targetSr;
            }
        }
    }

    const size_t newBytes = entryTotalBytes(*entry);

    auto it = entries_.find(key);
    if (it != entries_.end() && it->second != nullptr && it->second->published) {
        const size_t oldBytes = entryTotalBytes(*it->second);
        if (oldBytes > 0) {
            RenderCache::globalCacheCurrentBytes().fetch_sub(oldBytes, std::memory_order_relaxed);
        }
    }

    entries_[key] = std::move(entry);

    const size_t newCurrent = RenderCache::globalCacheCurrentBytes()
                                .fetch_add(newBytes, std::memory_order_relaxed) + newBytes;
    size_t peak = RenderCache::globalCachePeakBytes().load(std::memory_order_relaxed);
    while (newCurrent > peak
           && !RenderCache::globalCachePeakBytes()
                  .compare_exchange_weak(peak, newCurrent, std::memory_order_relaxed)) {}

    // Publish once
    auto snapshot = std::make_shared<const std::map<ContentKey, std::shared_ptr<const Entry>>>(entries_);
    auto old = std::atomic_exchange(&readerMap_, std::move(snapshot));
    if (old)
        retiredSnapshots_.push_back(std::move(old));
    retiredSnapshots_.erase(
        std::remove_if(retiredSnapshots_.begin(), retiredSnapshots_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredSnapshots_.end());
}

// ============================================================================
// prepareForPlaybackSampleRate — writer mutex 内完成
// ============================================================================

void TimeStretchCache::prepareForPlaybackSampleRate(double targetSr)
{
    if (targetSr <= 0.0) return;

    std::lock_guard<std::mutex> lg(mutex_);

    // Compute old total bytes for all published entries before rebuild
    size_t oldTotal = 0;
    for (auto& [_, entry] : entries_) {
        if (entry && entry->published) {
            oldTotal += entryTotalBytes(*entry);
        }
    }

    targetSampleRate_ = targetSr;

    // Re-prepare all published entries
    for (auto& [key, entry] : entries_) {
        juce::ignoreUnused(key);
        if (!entry || !entry->published || !entry->canonicalAudio || entry->canonicalAudio->empty()) continue;
        if (entry->sampleRate <= 0.0) continue;

        auto newEntry = std::make_shared<Entry>();
        newEntry->canonicalAudio = entry->canonicalAudio;
        newEntry->pitchRevision = entry->pitchRevision;
        newEntry->pitchShiftRevision = entry->pitchShiftRevision;
        newEntry->timeGridRevision = entry->timeGridRevision;
        newEntry->sampleRate = entry->sampleRate;
        newEntry->published = true;

        if (std::abs(targetSr - entry->sampleRate) < 1.0) {
            newEntry->preparedAudio = entry->canonicalAudio;
            newEntry->preparedSampleRate = targetSr;
        } else {
            const int outputLength = static_cast<int>(
                std::round(static_cast<double>(entry->canonicalAudio->size()) * targetSr / entry->sampleRate));
            if (outputLength > 0) {
                std::vector<float> resampled = preparedResampler_.resampleExactLength(
                    entry->canonicalAudio->data(),
                    entry->canonicalAudio->size(),
                    static_cast<int>(entry->sampleRate),
                    static_cast<int>(targetSr),
                    outputLength);
                newEntry->preparedAudio = std::make_shared<const std::vector<float>>(std::move(resampled));
                newEntry->preparedSampleRate = targetSr;
            }
        }

        entry = std::move(newEntry);
    }

    // Compute new total bytes and adjust global counter in one shot
    size_t newTotal = 0;
    for (auto& [_, entry] : entries_) {
        if (entry && entry->published) {
            newTotal += entryTotalBytes(*entry);
        }
    }

    if (oldTotal > 0) {
        RenderCache::globalCacheCurrentBytes().fetch_sub(oldTotal, std::memory_order_relaxed);
    }
    const size_t newCurrent = (newTotal > 0)
        ? RenderCache::globalCacheCurrentBytes().fetch_add(newTotal, std::memory_order_relaxed) + newTotal
        : RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed);
    size_t peak = RenderCache::globalCachePeakBytes().load(std::memory_order_relaxed);
    while (newCurrent > peak
           && !RenderCache::globalCachePeakBytes()
                  .compare_exchange_weak(peak, newCurrent, std::memory_order_relaxed)) {}

    // Publish once
    auto snapshot = std::make_shared<const std::map<ContentKey, std::shared_ptr<const Entry>>>(entries_);
    auto old = std::atomic_exchange(&readerMap_, std::move(snapshot));
    if (old)
        retiredSnapshots_.push_back(std::move(old));
    retiredSnapshots_.erase(
        std::remove_if(retiredSnapshots_.begin(), retiredSnapshots_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredSnapshots_.end());
}

// ============================================================================
// sliceForOutputRange — 音频线程只读 prepared audio（无 canonical fallback）
// ============================================================================

int TimeStretchCache::sliceForOutputRange(ContentKey key,
                                           uint64_t pitchRevision,
                                           uint64_t pitchShiftRevision,
                                           uint64_t timeGridRevision,
                                           int64_t readStartSample,
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
    if (!e.preparedAudio || e.preparedAudio->empty()) return 0;

    if (e.pitchRevision != pitchRevision
        || e.pitchShiftRevision != pitchShiftRevision
        || e.timeGridRevision != timeGridRevision)
        return 0;

    if (std::abs(e.preparedSampleRate - static_cast<double>(targetSampleRate)) >= 1.0) return 0;

    const std::vector<float>* srcAudio = e.preparedAudio.get();

    const int64_t start = readStartSample;
    if (start < 0 || start >= static_cast<int64_t>(srcAudio->size())) return 0;

    const int availableSamples = juce::jmin(
        writableSamples,
        static_cast<int>(static_cast<int64_t>(srcAudio->size()) - start));
    if (availableSamples <= 0) return 0;

    for (int i = 0; i < availableSamples; ++i) {
        const float v = (*srcAudio)[static_cast<size_t>(start + i)];
        for (int ch = 0; ch < destinationChannels; ++ch) {
            destination.setSample(ch, destinationStartSample + i, v);
        }
    }

    return availableSamples;
}

// ============================================================================
// sliceCanonicalForOutputRange — 非实时 canonical 切片（仅供 Stage2/export）
// ============================================================================

int TimeStretchCache::sliceCanonicalForOutputRange(ContentKey key,
                                                     uint64_t pitchRevision,
                                                     uint64_t pitchShiftRevision,
                                                     uint64_t timeGridRevision,
                                                     int64_t readStartSample,
                                                     juce::AudioBuffer<float>& destination,
                                                     int destinationStartSample,
                                                     int numSamples) const
{
    if (numSamples <= 0) return 0;
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
    if (!e.canonicalAudio || e.canonicalAudio->empty()) return 0;

    if (e.pitchRevision != pitchRevision
        || e.pitchShiftRevision != pitchShiftRevision
        || e.timeGridRevision != timeGridRevision)
        return 0;

    const std::vector<float>* srcAudio = e.canonicalAudio.get();

    const int64_t start = readStartSample;
    if (start < 0 || start >= static_cast<int64_t>(srcAudio->size())) return 0;

    const int availableSamples = juce::jmin(
        writableSamples,
        static_cast<int>(static_cast<int64_t>(srcAudio->size()) - start));
    if (availableSamples <= 0) return 0;

    for (int i = 0; i < availableSamples; ++i) {
        const float v = (*srcAudio)[static_cast<size_t>(start + i)];
        for (int ch = 0; ch < destinationChannels; ++ch) {
            destination.setSample(ch, destinationStartSample + i, v);
        }
    }

    return availableSamples;
}

void TimeStretchCache::invalidate(ContentKey key)
{
    std::lock_guard<std::mutex> lg(mutex_);
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second != nullptr) {
        const size_t oldBytes = entryTotalBytes(*it->second);
        if (oldBytes > 0 && it->second->published) {
            RenderCache::globalCacheCurrentBytes().fetch_sub(oldBytes, std::memory_order_relaxed);
        }
        it->second = std::make_shared<Entry>();
    }
    ++invalidationGen_[key];

    auto snapshot = std::make_shared<const std::map<ContentKey, std::shared_ptr<const Entry>>>(entries_);
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
    std::lock_guard<std::mutex> lg(mutex_);
    for (auto& [_, e] : entries_) {
        if (e && e->published) {
            const size_t bytes = entryTotalBytes(*e);
            if (bytes > 0) {
                RenderCache::globalCacheCurrentBytes().fetch_sub(bytes, std::memory_order_relaxed);
            }
        }
    }
    entries_.clear();
    invalidationGen_.clear();

    auto snapshot = std::make_shared<const std::map<ContentKey, std::shared_ptr<const Entry>>>();
    auto old = std::atomic_exchange(&readerMap_, std::move(snapshot));
    if (old)
        retiredSnapshots_.push_back(std::move(old));
    retiredSnapshots_.erase(
        std::remove_if(retiredSnapshots_.begin(), retiredSnapshots_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredSnapshots_.end());
}

} // namespace OpenTune
