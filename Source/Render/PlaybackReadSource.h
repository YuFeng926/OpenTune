#pragma once

#include "../Content/ContentKey.h"
#include "../Content/EditableContentSnapshot.h"
#include "../Content/NativePcmSource.h"
#include "../Inference/RenderCache.h"
#include "../Inference/TimeStretchCache.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdint>
#include <memory>
#include <utility>

namespace OpenTune {

/**
 * Immutable prepared dry buffer at a specific playback sample rate.
 * The buffer uses the original source grid at sampleRate. Equal native and
 * playback rates share the original PCM storage.
 */
struct PlaybackPreparedDry
{
    double sampleRate{0.0};
    std::shared_ptr<const juce::AudioBuffer<float>> buffer;
    int64_t firstPreparedSample{0};
    NativePcmSource nativeSource;
};

/**
 * PlaybackReadSource is the immutable read view consumed by render paths.
 *
 * Standalone and regular VST3 capture publish their owner audio here. ARA2
 * publishes a CRS-derived buffer rebuilt from AudioSource sample access. RenderCache
 * and TimeStretchCache are derived overlays keyed by the same ContentKey; they
 * never become persisted source truth.
 *
 * nativeSource retains the original PCM and its actual sampling grid.
 * canonicalBuffer/canonicalSampleRate are derived editing inputs; the first
 * element is firstCanonicalSample on the source's canonical grid.
 * preparedDry represents the active playback sample rate dry buffer.
 * contentSnapshot carries the only editable truth (notes/timeGrid/envelope/revisions).
 *
 * lifecycle:
 * lifetimeToken is declared FIRST → destroyed LAST. It anchors the publisher map
 * snapshot so the map (and all nested shared_ptr resources) outlives any reader copy.
 * When out-parameter reuse triggers copy-and-swap assignment, old resource members
 * are released before the old token — so retiredMaps_ always owns the final map
 * reference and destructs on the writer thread.
 */
struct PlaybackReadSource
{
    // Must be first member — reverse destruction order guarantees token dropped last.
    std::shared_ptr<const void> lifetimeToken;

    ContentKey contentKey;

    // 唯一 editable 数据真相：notes / timeGrid / volumeEnvelope / contentRevision
    // 等全部从 snapshot 读取。调用方保证发布时 contentSnapshot 非空且
    // contentSnapshot->timeGrid 非空（identity 用非空 identity grid 表达，
    // 不用 nullptr 语义）。
    std::shared_ptr<const EditableContentSnapshot> contentSnapshot;

    std::shared_ptr<RenderCache> renderCache;
    NativePcmSource nativeSource;
    std::shared_ptr<const juce::AudioBuffer<float>> canonicalBuffer;
    double canonicalSampleRate{0.0};
    int64_t firstCanonicalSample{0};
    TimeStretchCache* timeStretchCache{nullptr};

    // Immutable prepared dry at active playback rate
    PlaybackPreparedDry preparedDry;

    PlaybackReadSource() = default;
    PlaybackReadSource(const PlaybackReadSource&) = default;
    PlaybackReadSource(PlaybackReadSource&&) = default;

    // Copy-and-swap: replaces all members. Old resource members released before
    // old lifetime token, so the map snapshot in retiredMaps_ retains sole ownership
    // when the last reader copy is recycled.
    PlaybackReadSource& operator=(PlaybackReadSource other) noexcept
    {
        swap(*this, other);
        return *this;
    }

    friend void swap(PlaybackReadSource& a, PlaybackReadSource& b) noexcept
    {
        using std::swap;
        swap(a.lifetimeToken, b.lifetimeToken);
        swap(a.contentKey, b.contentKey);
        swap(a.contentSnapshot, b.contentSnapshot);
        swap(a.renderCache, b.renderCache);
        swap(a.nativeSource, b.nativeSource);
        swap(a.canonicalBuffer, b.canonicalBuffer);
        swap(a.canonicalSampleRate, b.canonicalSampleRate);
        swap(a.firstCanonicalSample, b.firstCanonicalSample);
        swap(a.timeStretchCache, b.timeStretchCache);
        swap(a.preparedDry, b.preparedDry);
    }

    bool hasAudio() const noexcept
    {
        return nativeSource.buffer != nullptr && nativeSource.buffer->getNumSamples() > 0;
    }

};

/**
 * Build the one playback projection shared by Standalone, Capture, ARA and
 * project restore. The snapshot is the only editable input; all other fields
 * are CRS-owned derived handles.
 */
inline PlaybackReadSource makePlaybackReadSource(
    ContentKey key,
    std::shared_ptr<const EditableContentSnapshot> contentSnapshot,
    NativePcmSource nativeSource,
    std::shared_ptr<const juce::AudioBuffer<float>> canonicalBuffer,
    double canonicalSampleRate,
    int64_t firstCanonicalSample,
    std::shared_ptr<RenderCache> renderCache,
    TimeStretchCache& timeStretchCache)
{
    jassert(contentSnapshot != nullptr && contentSnapshot->timeGrid != nullptr
        && !contentSnapshot->timeGrid->empty());
    jassert(nativeSource.buffer != nullptr && nativeSource.sampleRate > 0.0);

    PlaybackReadSource source;
    source.contentKey = key;
    source.contentSnapshot = std::move(contentSnapshot);
    source.renderCache = std::move(renderCache);
    source.nativeSource = std::move(nativeSource);
    source.canonicalBuffer = std::move(canonicalBuffer);
    source.canonicalSampleRate = canonicalSampleRate;
    source.firstCanonicalSample = firstCanonicalSample;
    source.timeStretchCache = &timeStretchCache;
    return source;
}

} // namespace OpenTune
