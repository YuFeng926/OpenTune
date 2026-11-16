#include "PlaybackSourcePublisher.h"
#include "../Inference/RenderCache.h"
#include "../Utils/TimeCoordinate.h"
#include "../Utils/PlaybackSampleGeometry.h"
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
            if (!dry.buffer || dry.buffer == src.nativeSource.buffer) continue;

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
        for (const auto& [otherKey, other] : *next) {
            juce::ignoreUnused(otherKey);
            if (other.preparedDry.buffer && other.preparedDry.sampleRate == sr
                && other.preparedDry.nativeSource == src.nativeSource) {
                src.preparedDry = other.preparedDry;
                break;
            }
        }
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
        for (const auto& [existingKey, existing] : *current) {
            juce::ignoreUnused(existingKey);
            if (existing.preparedDry.buffer
                && existing.preparedDry.sampleRate == playbackSampleRate_
                && existing.preparedDry.nativeSource == source.nativeSource) {
                source.preparedDry = existing.preparedDry;
                break;
            }
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
    if (src.preparedDry.buffer && src.preparedDry.sampleRate == targetSr
        && src.preparedDry.nativeSource == src.nativeSource) {
        return;
    }
    src.preparedDry = prepareDry(src.nativeSource, targetSr);
}

PlaybackPreparedDry PlaybackSourcePublisher::prepareDry(const NativePcmSource& native, double targetSr)
{
    jassert(native.buffer != nullptr && native.sampleRate > 0.0 && targetSr > 0.0);
    PlaybackPreparedDry dry;
    dry.sampleRate = targetSr;
    dry.nativeSource = native;
    if (targetSr == native.sampleRate) {
        dry.buffer = native.buffer;
        dry.firstPreparedSample = native.firstSourceSample;
        return dry;
    }

    using PlaybackSampleGeometry::projectToSample;
    dry.firstPreparedSample = projectToSample(
        static_cast<double>(native.firstSourceSample) * targetSr / native.sampleRate);
    const int64_t end = projectToSample(
        static_cast<double>(native.firstSourceSample + native.buffer->getNumSamples())
        * targetSr / native.sampleRate);
    const int outputLength = static_cast<int>(end - dry.firstPreparedSample);
    auto preparedBuf = std::make_shared<juce::AudioBuffer<float>>(
        native.buffer->getNumChannels(), outputLength);
    ResamplingManager resampler;
    for (int ch = 0; ch < native.buffer->getNumChannels(); ++ch) {
        const auto resampled = resampler.resampleRange(
            native.buffer->getReadPointer(ch), native.buffer->getNumSamples(),
            native.sampleRate, targetSr, native.firstSourceSample,
            dry.firstPreparedSample, outputLength);
        preparedBuf->copyFrom(ch, 0, resampled.data(), outputLength);
    }
    dry.buffer = std::move(preparedBuf);
    return dry;
}

} // namespace OpenTune
