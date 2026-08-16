#pragma once

#include "../Content/ContentKey.h"
#include "../Inference/RenderCache.h"
#include "../Inference/TimeStretchCache.h"
#include "../Utils/AutomationLane.h"
#include "../Utils/TimeGrid.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <cstdint>
#include <utility>

namespace OpenTune {

/**
 * Immutable prepared dry buffer at a specific playback sample rate.
 * When sampleRate == canonical 44.1kHz, buffer may alias the owner's canonical buffer.
 * canonicalBuffer shared_ptr equality provides address-reuse-safe identity.
 */
struct PlaybackPreparedDry
{
    double sampleRate{44100.0};
    std::shared_ptr<const juce::AudioBuffer<float>> buffer;
    std::shared_ptr<const juce::AudioBuffer<float>> canonicalBuffer;  // identity of source audioBuffer for republish reuse
};

/**
 * PlaybackReadSource is the immutable read view consumed by render paths.
 *
 * Standalone and regular VST3 capture publish their owner audio here. ARA2
 * publishes a CRS-derived buffer rebuilt from AudioSource sample access. RenderCache
 * and TimeStretchCache are derived overlays keyed by the same ContentKey; they
 * never become persisted source truth.
 *
 * audioBuffer/audioSampleRate represent 44.1kHz (or owner canonical rate) content truth.
 * preparedDry represents the active playback sample rate dry buffer.
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

    std::shared_ptr<RenderCache> renderCache;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double audioSampleRate{0.0};
    TimeStretchCache* timeStretchCache{nullptr};

    // Immutable prepared dry at active playback rate
    PlaybackPreparedDry preparedDry;

    // 唯一音量真相与 output-time -> source-time 映射。
    std::shared_ptr<const AutomationLane> volumeEnvelope;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;

    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};

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
        swap(a.renderCache, b.renderCache);
        swap(a.audioBuffer, b.audioBuffer);
        swap(a.audioSampleRate, b.audioSampleRate);
        swap(a.timeStretchCache, b.timeStretchCache);
        swap(a.preparedDry, b.preparedDry);
        swap(a.volumeEnvelope, b.volumeEnvelope);
        swap(a.timeGrid, b.timeGrid);
        swap(a.pitchRevision, b.pitchRevision);
        swap(a.timeGridRevision, b.timeGridRevision);
        swap(a.pitchShiftRevision, b.pitchShiftRevision);
    }

    bool hasAudio() const noexcept
    {
        return audioBuffer != nullptr && audioBuffer->getNumSamples() > 0;
    }

};

} // namespace OpenTune
