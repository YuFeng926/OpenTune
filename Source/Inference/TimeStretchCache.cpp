#include "TimeStretchCache.h"
#include "RenderCache.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/PlaybackSampleGeometry.h"
#include "../Utils/TimeCoordinate.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OpenTune {

namespace {

std::shared_ptr<const std::vector<float>> makeTrackedPcm(std::vector<float>&& audio)
{
    auto* pcm = new std::vector<float>(std::move(audio));
    const size_t bytes = pcm->size() * sizeof(float);
    auto& current = RenderCache::globalCacheCurrentBytes();
    const size_t currentBytes = current.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    auto& peak = RenderCache::globalCachePeakBytes();
    size_t observed = peak.load(std::memory_order_relaxed);
    while (currentBytes > observed
           && !peak.compare_exchange_weak(observed, currentBytes, std::memory_order_relaxed)) {}

    return std::shared_ptr<const std::vector<float>>(
        pcm, [bytes](const std::vector<float>* value) {
            delete value;
            RenderCache::globalCacheCurrentBytes().fetch_sub(bytes, std::memory_order_relaxed);
        });
}

} // namespace

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

    entry.preparedAudio = makeTrackedPcm(preparedResampler_.resampleRange(
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
    readerMap_.collect();
    const uint32_t generation = ++nextBuildGeneration_;
    buildGenerationByKey_[key] = generation;
    return generation;
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
    readerMap_.collect();

    auto genIt = buildGenerationByKey_.find(key);
    if (genIt == buildGenerationByKey_.end() || genIt->second != buildGeneration) {
        return;
    }

    auto entry = std::make_shared<Entry>();
    entry->canonicalAudio = makeTrackedPcm(std::move(audio));
    entry->contentRevision = contentRevision;
    entry->timeGridRevision = timeGridRevision;
    entry->sampleRate = sampleRate;
    entry->firstCanonicalOutputSample = firstCanonicalOutputSample;
    entry->published = true;
    prepareEntry(*entry, targetSampleRate_);

    entries_[key] = std::move(entry);
    publishReaderMapLocked();
}

void TimeStretchCache::publishReaderMapLocked()
{
    readerMap_.publish(std::make_shared<const ReaderMap>(entries_));
    readerMap_.collect();
}

// ============================================================================
// prepareForPlaybackSampleRate — writer mutex 内完成
// ============================================================================

void TimeStretchCache::prepareForPlaybackSampleRate(double targetSr)
{
    if (targetSr <= 0.0) return;

    std::lock_guard<std::mutex> lg(mutex_);
    readerMap_.collect();

    targetSampleRate_ = targetSr;

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

    publishReaderMapLocked();
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

    auto snap = readerMap_.read();
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

    auto snap = readerMap_.read();
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
    readerMap_.collect();
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second != nullptr)
        it->second = std::make_shared<Entry>();
    buildGenerationByKey_.erase(key);
    publishReaderMapLocked();
}

void TimeStretchCache::clear()
{
    std::lock_guard<std::mutex> lg(mutex_);
    readerMap_.collect();
    entries_.clear();
    buildGenerationByKey_.clear();
    readerMap_.clear();
    readerMap_.collect();
}

void TimeStretchCache::collectPlaybackSnapshots()
{
    std::lock_guard<std::mutex> lg(mutex_);
    readerMap_.collect();
}

} // namespace OpenTune
