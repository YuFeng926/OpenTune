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
 * Multi-writer variant (tryPublish): When multiple ARA-role processors share
 * one PlayHeadState, the projection may be published from different audio
 * threads. The CAS-based tryPublish path aborts on contention rather than
 * spinning — the canonical atomics already hold the latest truth, so a lost
 * publish is harmless (UI falls back to canonical timeInSeconds).
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

    // Multi-writer CAS publish: attempts one projection commit. On contention
    // (another writer is active), returns false — the caller's canonical atomics
    // are already updated so the UI will read correct time via fallback path.
    // The epoch bump on new PositionInfo with time ensures stale projections
    // from an older epoch are invalidated by readers.
    bool tryPublish(double positionSec, double nowClockSec, double horizonSec, uint64_t epoch)
    {
        uint64_t expected = sequence.load(std::memory_order_relaxed);
        // Wait for even sequence (not writer-active) but never spin past 0
        // (never-published is fine to transition from).
        if (expected & 1)
            return false;  // writer active right now, skip

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
// presentationEpoch is incremented on every discrete control change
// (play/stop/seek/reset) or on every valid PositionInfo with time; the audio
// thread publishes projection anchors tagged with the current epoch. UI
// projection is only valid when anchorEpoch == presentationEpoch; otherwise
// fall back to canonical time.
//
// isPlaying is the release/acquire publication point between writer
// (audio/control thread) and reader (UI thread). getPresentedPositionAt
// acquires isPlaying; when it sees false, timeInSeconds is guaranteed
// visible. When it sees true, the projection path handles ordering via
// the presentation epoch.
//
// update() contract:
//  1. nullopt: no-op. Do not clear fields or fake stopped.
//  2. valid PositionInfo but no timeInSeconds: keep the last valid time; still
//     write isPlaying/isLooping and loop points when present.
//  3. valid PositionInfo with timeInSeconds: write time before isPlaying, then
//     write isLooping and loop points when present. Advance presentationEpoch
//     so that any stale projection from a previous epoch is invalidated.
//
// reset() contract (only prepareToPlay/releaseResources call it on local state):
//  - clear isPlaying/isLooping; keep last time/loop range; bump presentationEpoch.
//  In shared (DC) mode, reset is NOT called from per-processor prepare/release.
struct PlayHeadState
{
    std::atomic<bool>    isPlaying { false };
    std::atomic<bool>    isLooping { false };
    std::atomic<double>  timeInSeconds { 0.0 };
    std::atomic<double>  loopPpqStart { 0.0 };
    std::atomic<double>  loopPpqEnd { 0.0 };
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
    // Returns the presentationEpoch actually used during this call:
    //  - PositionInfo present with timeInSeconds → bumped epoch (old+1)
    //  - PositionInfo absent, or present without time → current epoch (unchanged)

    uint64_t update(const juce::Optional<juce::AudioPlayHead::PositionInfo>& info)
    {
        if (!info.hasValue())
            return presentationEpoch.load(std::memory_order_relaxed);

        const auto& positionInfo = *info;

        // Write time-in-seconds before isPlaying so a reader that acquires
        // isPlaying==false sees this block's canonical time.
        uint64_t epoch = presentationEpoch.load(std::memory_order_relaxed);
        if (const auto timeSeconds = positionInfo.getTimeInSeconds())
        {
            timeInSeconds.store(*timeSeconds, std::memory_order_relaxed);
            // Advance epoch on every new PositionInfo with time: this ensures
            // any projection published with an older epoch is considered stale
            // by the reader, even if the projection CAS succeeded before the
            // canonical time write became visible.
            epoch = presentationEpoch.fetch_add(1, std::memory_order_release) + 1;
        }

        if (const auto loopPoints = positionInfo.getLoopPoints())
        {
            loopPpqStart.store(loopPoints->ppqStart, std::memory_order_relaxed);
            loopPpqEnd.store(loopPoints->ppqEnd, std::memory_order_relaxed);
        }

        isLooping.store(positionInfo.getIsLooping(), std::memory_order_relaxed);

        // isPlaying release publishes timeInSeconds/loop written above.
        // Pairs with getPresentedPositionAt's isPlaying acquire: when
        // the UI sees false, the canonical pause position is visible.
        isPlaying.store(positionInfo.getIsPlaying(), std::memory_order_release);

        return epoch;
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
