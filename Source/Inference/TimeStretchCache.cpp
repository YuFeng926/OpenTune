#include "TimeStretchCache.h"
#include "RenderCache.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/PlaybackSampleGeometry.h"
#include "../Utils/TimeCoordinate.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

// Rebuilds one entry's prepared PCM from the complete Stage2 output with the
// shared range SRC contract and the explicit output-domain origin.
void TimeStretchCache::prepareEntry(Entry& entry, double targetSr)
{
    entry.preparedAudio.reset();
    entry.preparedSampleRate = 0.0;
    entry.firstPreparedOutputSample = entry.firstCanonicalOutputSample;
    if (!entry.canonicalAudio || entry.canonicalAudio->empty()
        || entry.sampleRate <= 0.0 || targetSr <= 0.0)
        return;

    if (targetSr == entry.sampleRate) {
        entry.preparedAudio = entry.canonicalAudio;
        entry.preparedSampleRate = targetSr;
        return;
    }

    using PlaybackSampleGeometry::projectToSample;
    const int64_t canonicalLength = static_cast<int64_t>(entry.canonicalAudio->size());
    const int64_t canonicalEnd = entry.firstCanonicalOutputSample + canonicalLength;
    const int64_t preparedBegin = projectToSample(
        static_cast<double>(entry.firstCanonicalOutputSample) * targetSr / entry.sampleRate);
    const int64_t preparedEnd = projectToSample(
        static_cast<double>(canonicalEnd) * targetSr / entry.sampleRate);
    const int64_t outputLength = preparedEnd - preparedBegin;
    if (outputLength <= 0
        || outputLength > static_cast<int64_t>(std::numeric_limits<int>::max()))
        return;

    entry.preparedAudio = std::make_shared<const std::vector<float>>(
        preparedResampler_.resampleRange(
            entry.canonicalAudio->data(), entry.canonicalAudio->size(),
            entry.sampleRate, targetSr,
            entry.firstCanonicalOutputSample, preparedBegin,
            static_cast<int>(outputLength)));
    entry.preparedSampleRate = targetSr;
    entry.firstPreparedOutputSample = preparedBegin;
}

uint32_t TimeStretchCache::beginBuild(ContentKey key)
{
    if (!key.isValid()) return 0;

    std::lock_guard<std::mutex> lg(mutex_);
    auto [it, _] = invalidationGen_.try_emplace(key, 0);
    return it->second;
}

void TimeStretchCache::store(ContentKey key,
                              std::vector<float> audio,
                              uint64_t contentRevision,
                              uint64_t timeGridRevision,
                              double sampleRate,
                              uint32_t buildGeneration,
                              int64_t firstCanonicalOutputSample)
{
    if (!key.isValid() || sampleRate <= 0.0) return;

    std::lock_guard<std::mutex> lg(mutex_);

    auto genIt = invalidationGen_.find(key);
    if (genIt == invalidationGen_.end() || genIt->second != buildGeneration) {
        return;
    }

    auto entry = std::make_shared<Entry>();
    entry->canonicalAudio = std::make_shared<const std::vector<float>>(std::move(audio));
    entry->contentRevision = contentRevision;
    entry->timeGridRevision = timeGridRevision;
    entry->sampleRate = sampleRate;
    entry->firstCanonicalOutputSample = firstCanonicalOutputSample;
    entry->published = true;
    prepareEntry(*entry, targetSampleRate_);

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
        newEntry->contentRevision = entry->contentRevision;
        newEntry->timeGridRevision = entry->timeGridRevision;
        newEntry->sampleRate = entry->sampleRate;
        newEntry->firstCanonicalOutputSample = entry->firstCanonicalOutputSample;
        newEntry->published = true;
        prepareEntry(*newEntry, targetSr);

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
// 版本 key = contentRevision + timeGridRevision
// ============================================================================

int TimeStretchCache::sliceForOutputRange(ContentKey key,
                                           uint64_t contentRevision,
                                           uint64_t timeGridRevision,
                                           int64_t readStartSample,
                                           juce::AudioBuffer<float>& destination,
                                           int destinationStartSample,
                                           int numSamples,
                                           double targetSampleRate) const
{
    if (numSamples <= 0 || targetSampleRate <= 0.0) return 0;
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

    if (e.contentRevision != contentRevision
        || e.timeGridRevision != timeGridRevision)
        return 0;

    if (e.preparedSampleRate != targetSampleRate) return 0;

    // Integer intersection on the explicit playback grid: a negative read start
    // keeps its valid suffix, and the leading clipped offset places the copied
    // PCM at its correct destination position.
    const auto span = PlaybackSampleGeometry::intersect(
        { readStartSample, readStartSample + writableSamples },
        { e.firstPreparedOutputSample,
          e.firstPreparedOutputSample
              + static_cast<int64_t>(e.preparedAudio->size()) });
    if (span.empty()) return 0;

    const int leading = static_cast<int>(span.begin - readStartSample);
    const int copied = static_cast<int>(span.end - span.begin);
    const int sourceOffset = static_cast<int>(span.begin - e.firstPreparedOutputSample);
    if (copied <= 0 || sourceOffset < 0) return 0;

    destination.clear(destinationStartSample, writableSamples);
    const float* source = e.preparedAudio->data() + sourceOffset;
    for (int ch = 0; ch < destinationChannels; ++ch) {
        destination.copyFrom(ch, destinationStartSample + leading, source, copied);
    }

    // Returned extent includes the clipped leading zero prefix.
    return leading + copied;
}

int TimeStretchCache::sliceCanonicalForOutputRange(ContentKey key,
                                                    uint64_t contentRevision,
                                                    uint64_t timeGridRevision,
                                                    int64_t readStartSample,
                                                    juce::AudioBuffer<float>& destination,
                                                    int destinationStartSample,
                                                    int numSamples) const
{
    if (numSamples <= 0 || destinationStartSample < 0)
        return 0;

    const int destinationChannels = destination.getNumChannels();
    const int destinationSamples = destination.getNumSamples();
    if (destinationChannels <= 0 || destinationSamples <= 0
        || destinationStartSample >= destinationSamples)
        return 0;

    const int writableSamples = std::min(numSamples, destinationSamples - destinationStartSample);
    if (writableSamples <= 0)
        return 0;

    auto snap = std::atomic_load(&readerMap_);
    if (!snap)
        return 0;
    const auto it = snap->find(key);
    if (it == snap->end() || !it->second || !it->second->published)
        return 0;

    const auto& entry = *it->second;
    if (entry.contentRevision != contentRevision
        || entry.timeGridRevision != timeGridRevision
        || !entry.canonicalAudio || entry.canonicalAudio->empty())
        return 0;

    // Same integer intersection on the TimeGrid local canonical output grid.
    const auto span = PlaybackSampleGeometry::intersect(
        { readStartSample, readStartSample + writableSamples },
        { entry.firstCanonicalOutputSample,
          entry.firstCanonicalOutputSample
              + static_cast<int64_t>(entry.canonicalAudio->size()) });
    if (span.empty())
        return 0;

    const int leading = static_cast<int>(span.begin - readStartSample);
    const int copied = static_cast<int>(span.end - span.begin);
    const int sourceOffset = static_cast<int>(span.begin - entry.firstCanonicalOutputSample);
    if (copied <= 0 || sourceOffset < 0)
        return 0;

    destination.clear(destinationStartSample, writableSamples);
    const float* source = entry.canonicalAudio->data() + sourceOffset;
    for (int channel = 0; channel < destinationChannels; ++channel)
        destination.copyFrom(channel, destinationStartSample + leading, source, copied);

    // Returned extent includes the clipped leading zero prefix.
    return leading + copied;
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
