#include "PlaybackSourcePublisher.h"
#include "../Inference/RenderCache.h"
#include <algorithm>
#include <set>

namespace OpenTune {

PlaybackSourcePublisher::PlaybackSourcePublisher()
{
    std::atomic_store(&data_, std::make_shared<const std::map<ContentKey, PlaybackReadSource>>());
}

PlaybackSourcePublisher::~PlaybackSourcePublisher()
{
    if (totalPreparedBytes_ > 0) {
        RenderCache::globalCacheCurrentBytes().fetch_sub(totalPreparedBytes_, std::memory_order_relaxed);
    }
}

// ---- prepared dry byte tracking ----

void PlaybackSourcePublisher::recomputeActivePreparedBytes()
{
    auto snap = std::atomic_load(&data_);

    std::set<uintptr_t> seenPtrs;
    size_t newTotal = 0;

    if (snap) {
        for (const auto& pair : *snap) {
            const auto& src = pair.second;
            const auto& dry = src.preparedDry;
            if (!dry.buffer || dry.buffer == src.audioBuffer) continue;

            const uintptr_t ptr = reinterpret_cast<uintptr_t>(dry.buffer.get());
            if (!seenPtrs.insert(ptr).second) continue;  // deduplicate by unique buffer pointer

            const size_t bytes = static_cast<size_t>(dry.buffer->getNumChannels())
                               * static_cast<size_t>(dry.buffer->getNumSamples())
                               * sizeof(float);
            if (bytes == 0) continue;

            newTotal += bytes;
        }
    }

    const int64_t delta = static_cast<int64_t>(newTotal) - static_cast<int64_t>(totalPreparedBytes_);
    if (delta > 0) {
        const size_t newCurrent = RenderCache::globalCacheCurrentBytes().fetch_add(
            static_cast<size_t>(delta), std::memory_order_relaxed) + static_cast<size_t>(delta);
        size_t peak = RenderCache::globalCachePeakBytes().load(std::memory_order_relaxed);
        while (newCurrent > peak && !RenderCache::globalCachePeakBytes().compare_exchange_weak(
            peak, newCurrent, std::memory_order_relaxed)) {}
    } else if (delta < 0) {
        RenderCache::globalCacheCurrentBytes().fetch_sub(
            static_cast<size_t>(-delta), std::memory_order_relaxed);
    }

    totalPreparedBytes_ = newTotal;
}

// ---- public API ----

void PlaybackSourcePublisher::setPlaybackSampleRate(double sr)
{
    if (sr <= 0.0) return;

    std::lock_guard<std::mutex> lg(writerMutex_);
    playbackSampleRate_ = sr;

    auto current = std::atomic_load(&data_);
    if (!current || current->empty()) return;

    auto next = std::make_shared<std::map<ContentKey, PlaybackReadSource>>(*current);
    for (auto& [key, src] : *next) {
        juce::ignoreUnused(key);
        prepareSourceDry(src, playbackSampleRate_);
    }
    auto old = std::atomic_exchange(&data_,
        std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>>(std::move(next)));
    if (old) retiredMaps_.push_back(std::move(old));
    retiredMaps_.erase(
        std::remove_if(retiredMaps_.begin(), retiredMaps_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredMaps_.end());

    recomputeActivePreparedBytes();
}

void PlaybackSourcePublisher::publish(ContentKey key, PlaybackReadSource source)
{
    source.contentKey = key;

    std::lock_guard<std::mutex> lg(writerMutex_);

    auto current = std::atomic_load(&data_);
    if (current) {
        const auto existing = current->find(key);
        if (existing != current->end()
            && existing->second.audioBuffer == source.audioBuffer) {
            source.preparedDry = existing->second.preparedDry;
        }
    }

    if (playbackSampleRate_ > 0.0) {
        prepareSourceDry(source, playbackSampleRate_);
    }

    // Clear lifetime token before storing in map — avoid map→self cycle.
    source.lifetimeToken.reset();

    auto next = current
        ? std::make_shared<std::map<ContentKey, PlaybackReadSource>>(*current)
        : std::make_shared<std::map<ContentKey, PlaybackReadSource>>();
    (*next)[key] = std::move(source);
    auto old = std::atomic_exchange(&data_,
        std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>>(std::move(next)));
    if (old) retiredMaps_.push_back(std::move(old));
    retiredMaps_.erase(
        std::remove_if(retiredMaps_.begin(), retiredMaps_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredMaps_.end());

    recomputeActivePreparedBytes();
}

bool PlaybackSourcePublisher::get(ContentKey key, PlaybackReadSource& out) const noexcept
{
    auto snap = std::atomic_load(&data_);
    if (!snap) return false;
    auto it = snap->find(key);
    if (it == snap->end()) return false;
    out = it->second;
    out.lifetimeToken = std::shared_ptr<const void>(snap);
    return true;
}

void PlaybackSourcePublisher::remove(ContentKey key)
{
    std::lock_guard<std::mutex> lg(writerMutex_);
    auto current = std::atomic_load(&data_);
    if (!current || current->find(key) == current->end()) return;
    auto next = std::make_shared<std::map<ContentKey, PlaybackReadSource>>(*current);
    next->erase(key);
    auto old = std::atomic_exchange(&data_,
        std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>>(std::move(next)));
    if (old) retiredMaps_.push_back(std::move(old));
    retiredMaps_.erase(
        std::remove_if(retiredMaps_.begin(), retiredMaps_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredMaps_.end());

    recomputeActivePreparedBytes();
}

void PlaybackSourcePublisher::clear()
{
    std::lock_guard<std::mutex> lg(writerMutex_);
    auto old = std::atomic_exchange(&data_,
        std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>>());
    if (old) retiredMaps_.push_back(std::move(old));
    retiredMaps_.erase(
        std::remove_if(retiredMaps_.begin(), retiredMaps_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredMaps_.end());

    recomputeActivePreparedBytes();
}

// ---- private helpers ----

void PlaybackSourcePublisher::prepareSourceDry(PlaybackReadSource& src, double targetSr)
{
    if (targetSr <= 0.0) return;
    if (!src.audioBuffer || src.audioBuffer->getNumSamples() <= 0) return;
    if (src.audioSampleRate <= 0.0) return;

    // Reuse existing prepared buffer when canonical source buffer and target rate unchanged.
    if (src.preparedDry.buffer != nullptr
        && std::abs(src.preparedDry.sampleRate - targetSr) < 1.0
        && src.preparedDry.canonicalBuffer == src.audioBuffer) {
        return;
    }

    // Canonical rate == target → alias canonical buffer
    if (std::abs(targetSr - src.audioSampleRate) < 1.0) {
        PlaybackPreparedDry dry;
        dry.sampleRate = targetSr;
        dry.buffer = src.audioBuffer;
        dry.canonicalBuffer = src.audioBuffer;
        src.preparedDry = dry;
        return;
    }

    const int srcLen = src.audioBuffer->getNumSamples();
    const int srcChs = src.audioBuffer->getNumChannels();
    const int outputLength = static_cast<int>(
        std::round(static_cast<double>(srcLen) * targetSr / src.audioSampleRate));
    if (outputLength <= 0) return;

    auto preparedBuf = std::make_shared<juce::AudioBuffer<float>>(srcChs, outputLength);

    for (int ch = 0; ch < srcChs; ++ch) {
        std::vector<float> resampled = resampler_.resampleExactLength(
            src.audioBuffer->getReadPointer(ch),
            static_cast<size_t>(srcLen),
            static_cast<int>(src.audioSampleRate),
            static_cast<int>(targetSr),
            outputLength);
        preparedBuf->copyFrom(ch, 0, resampled.data(), outputLength);
    }

    PlaybackPreparedDry dry;
    dry.sampleRate = targetSr;
    dry.buffer = std::move(preparedBuf);
    dry.canonicalBuffer = src.audioBuffer;
    src.preparedDry = dry;
}

} // namespace OpenTune
