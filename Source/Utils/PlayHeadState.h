#pragma once

/**
 * PlayHeadState — canonical transport truth shared across UI and audio threads.
 *
 * Originally inlined in PluginProcessor.h. Extracted here to serve as the
 * single definition shared by both per-processor local state and
 * document-level shared state (OpenTuneDocumentController).
 *
 * PlayHeadPresentationProjection — seqlock-based smooth projection anchor.
 * Audio thread publishes anchors; UI thread reads snapshots and projects.
 *
 * Multi-writer variant (tryPublish): ARA realtime editor renderers may share
 * one PlayHeadState. Duplicate observations of the same canonical time are
 * idempotent: they do not advance presentationEpoch or publish another anchor.
 * Only the writer that advances canonical time publishes the anchor for that
 * transport generation.
 */

#include <atomic>
#include <cstdint>
#include <juce_audio_processors/juce_audio_processors.h>

namespace OpenTune {

// ============================================================================
// PlayHeadPresentationProjection — single/multi-writer seqlock projection
// ============================================================================
//
// Audio thread is the regular writer. UI thread reads snapshots.
// sequence=0 means no anchor published yet; odd means writer inside; even means valid.
//
// projectAt(nowClockSeconds) = min(horizon, anchorPosition + max(0, nowClock - anchorClock))
// The horizon is the end of the committed audio block; UI never paints beyond it.
//
// All anchor fields are std::atomic to avoid UB from concurrent reads under seqlock.
// Field accesses use memory_order_relaxed; ordering between field stores/loads
// and the seqlock sequence is provided by the two acquire loads.
struct PlayHeadPresentationProjection
{
    // seqlock sequence (even=valid snapshot, odd=writer active, 0=never published)
    std::atomic<uint64_t> sequence{0};

    std::atomic<double>   anchorPositionSeconds{0.0};
    std::atomic<double>   anchorClockSeconds{0.0};
    std::atomic<double>   horizonPositionSeconds{0.0};
    std::atomic<uint64_t> anchorEpoch{0};

    struct Snapshot
    {
        double anchorPosition = 0.0;
        double anchorClock    = 0.0;
        double horizon        = 0.0;
        uint64_t epoch        = 0;
        bool     valid        = false;

        double projectAt(double nowClockSeconds) const
        {
            if (!valid) return 0.0;
            const double elapsed = nowClockSeconds - anchorClock;
            if (elapsed <= 0.0) return anchorPosition;
            const double projected = anchorPosition + elapsed;
            return (projected > horizon) ? horizon : projected;
        }
    };

    // Seqlock reader with two acquire sequence loads. sequence==0 (never published)
    // returns invalid Snapshot immediately; odd sequence (writer inside) retries.
    Snapshot load() const
    {
        for (;;)
        {
            const uint64_t seq0 = sequence.load(std::memory_order_acquire);
            if (seq0 == 0)
                return {};
            if (seq0 & 1)
                continue;

            Snapshot snap;
            snap.anchorPosition = anchorPositionSeconds.load(std::memory_order_relaxed);
            snap.anchorClock    = anchorClockSeconds.load(std::memory_order_relaxed);
            snap.horizon        = horizonPositionSeconds.load(std::memory_order_relaxed);
            snap.epoch          = anchorEpoch.load(std::memory_order_relaxed);

            const uint64_t seq1 = sequence.load(std::memory_order_acquire);
            if (seq0 == seq1)
            {
                snap.valid = true;
                return snap;
            }
        }
    }

    // Single-writer publish: fetch_add acq_rel enters odd phase; fetch_add release
    // commits even phase and makes field stores visible to readers.
    // Use only from codepaths that guarantee a single writer (Standalone local path).
    void publish(double positionSec, double nowClockSec, double horizonSec, uint64_t epoch)
    {
        sequence.fetch_add(1, std::memory_order_acq_rel);  // enter write (odd)
        anchorPositionSeconds.store(positionSec,  std::memory_order_relaxed);
        anchorClockSeconds.store(nowClockSec,      std::memory_order_relaxed);
        horizonPositionSeconds.store(horizonSec,    std::memory_order_relaxed);
        anchorEpoch.store(epoch,                    std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_release);  // commit (even)
    }

    // Multi-writer CAS publish for ARA realtime editor renderers. Only the
    // writer that advanced canonical time calls this; duplicate observations do
    // not publish. Reject an older epoch so a stale anchor cannot overwrite a
    // newer one. On CAS contention, skip; the writer for the newer transport
    // generation, or the next block, owns the anchor.
    bool tryPublish(double positionSec, double nowClockSec, double horizonSec, uint64_t epoch)
    {
        uint64_t expected = sequence.load(std::memory_order_relaxed);
        // Wait for even sequence (not writer-active) but never spin past 0
        // (never-published is fine to transition from).
        if (expected & 1)
            return false;  // writer active right now, skip

        if (epoch < anchorEpoch.load(std::memory_order_relaxed))
            return false;  // stale anchor from an older transport generation

        if (!sequence.compare_exchange_strong(expected, expected + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed))
            return false;  // lost race, skip this projection

        anchorPositionSeconds.store(positionSec,  std::memory_order_relaxed);
        anchorClockSeconds.store(nowClockSec,      std::memory_order_relaxed);
        horizonPositionSeconds.store(horizonSec,    std::memory_order_relaxed);
        anchorEpoch.store(epoch,                    std::memory_order_relaxed);

        sequence.fetch_add(1, std::memory_order_release);  // commit (even)
        return true;
    }
};

// ============================================================================
// PlayHeadState — processor-owned or DC-shared canonical transport truth
// ============================================================================
//
// Each OpenTuneAudioProcessor owns one PlayHeadState locally. In ARA mode,
// all ARA roles within the same document share a single PlayHeadState owned
// by the OpenTuneDocumentController. getPlayHeadState() routes accordingly.
//
// processBlock updates the PlayHeadState from host AudioPlayHead::PositionInfo
// exactly once per block. UI binds to a const, non-owning reference and reads
// atomics directly. ARA transport requests are single-direction
// HostPlaybackController requests and never write back here.
//
// presentationEpoch is a transport generation: it advances only when the
// canonical time changes or play state changes, not for duplicate observations
// of the same current time. The writer that advances time publishes an anchor
// tagged with that generation; duplicate realtime writers are no-ops. UI
// projection is valid when anchorEpoch == presentationEpoch; otherwise fall
// back to canonical time.
//
// isPlaying is the release/acquire publication point between writer
// (audio/control thread) and reader (UI thread). getPresentedPositionAt
// acquires isPlaying; when it sees false, timeInSeconds is guaranteed
// visible. When it sees true, the projection path handles ordering via
// the presentation epoch.
//
// update() contract:
//  1. nullopt: no-op. Do not clear fields or fake stopped.
//  2. valid PositionInfo but no timeInSeconds: keep the last valid time; write
//     isPlaying/isLooping. A play-state change advances the transport generation.
//  3. valid PositionInfo with timeInSeconds: update canonical time with CAS.
//     Only the writer that actually changes time advances the transport
//     generation; duplicate observations of the same current time are no-ops.
//
// reset() contract (only prepareToPlay/releaseResources call it on local state):
//  - clear isPlaying/isLooping; keep last time; bump presentationEpoch.
//  In shared (DC) mode, reset is NOT called from per-processor prepare/release.
struct PlayHeadState
{
    std::atomic<bool>    isPlaying { false };
    std::atomic<bool>    isLooping { false };
    std::atomic<double>  timeInSeconds { 0.0 };
    std::atomic<uint64_t> presentationEpoch { 0 };

    PlayHeadPresentationProjection presentationProjection;

    // ---- presented-position API (UI entry point) ----

    double getPresentedPositionAt(double nowClockSeconds) const
    {
        if (!isPlaying.load(std::memory_order_acquire))
            return timeInSeconds.load(std::memory_order_relaxed);

        const auto snap = presentationProjection.load();
        if (!snap.valid || snap.epoch != presentationEpoch.load(std::memory_order_acquire))
            return timeInSeconds.load(std::memory_order_relaxed);

        return snap.projectAt(nowClockSeconds);
    }

    double getPresentedPositionSeconds() const
    {
        return getPresentedPositionAt(juce::Time::getMillisecondCounterHiRes() * 0.001);
    }

    // ---- canonical-state update (audio thread only) ----
    //
    // Returns the transport generation and whether this call advanced canonical
    // time. Only the caller that observes timeChanged=true publishes a
    // projection anchor. Duplicate observations of the same current time are
    // idempotent: no epoch bump, no anchor.

    struct UpdateResult
    {
        uint64_t epoch = 0;
        bool timeChanged = false;
    };

    UpdateResult update(const juce::Optional<juce::AudioPlayHead::PositionInfo>& info)
    {
        UpdateResult result;
        result.epoch = presentationEpoch.load(std::memory_order_acquire);

        if (!info.hasValue())
            return result;

        const auto& positionInfo = *info;
        bool transportChanged = false;

        // CAS on canonical time: concurrent ARA realtime editor renderers that
        // observe the same current time do not each advance the generation.
        if (const auto timeSeconds = positionInfo.getTimeInSeconds())
        {
            double current = timeInSeconds.load(std::memory_order_relaxed);
            while (current != *timeSeconds)
            {
                if (timeInSeconds.compare_exchange_weak(current, *timeSeconds,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_relaxed))
                {
                    result.timeChanged = true;
                    transportChanged = true;
                    break;
                }
                // CAS failure reloads current. If another writer already
                // installed the same time, the loop exits without a bump.
            }
        }

        isLooping.store(positionInfo.getIsLooping(), std::memory_order_relaxed);

        // isPlaying exchange publishes time/loop written above and lets only
        // the first observer of a play-state change advance the generation.
        const bool wasPlaying = isPlaying.exchange(positionInfo.getIsPlaying(),
                                                   std::memory_order_acq_rel);
        if (wasPlaying != positionInfo.getIsPlaying())
            transportChanged = true;

        if (transportChanged)
            result.epoch = presentationEpoch.fetch_add(1, std::memory_order_release) + 1;

        return result;
    }

    // reset() is only called from prepareToPlay/releaseResources on the audio thread
    // for per-processor local state. Bumps presentationEpoch so that any stale
    // projection anchor is invalidated.
    void reset()
    {
        isPlaying.store(false, std::memory_order_release);
        isLooping.store(false, std::memory_order_relaxed);
        presentationEpoch.fetch_add(1, std::memory_order_release);
    }
};

} // namespace OpenTune
