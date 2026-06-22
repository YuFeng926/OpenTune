#pragma once

#include "PianoRollRenderSnapshot.h"
#include "UI/TimelineViewportState.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <map>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <mutex>
#include <functional>

namespace OpenTune {

struct PianoRollTileKey {
    uint64_t surfaceEpoch = 0;
    int tileIndex = 0;
    int tileStartContentX = 0;
    int tileWidthPx = 1024;
    int contentHeightPx = 0;

    uint64_t pitchEpoch = 0;
    uint64_t notesEpoch = 0;
    uint64_t waveformRevision = 0;
    uint64_t timeGridRevision = 0;
    uint64_t visualPrefsRevision = 0;
    uint64_t placementProjectionRevision = 0;

    int zoomBucket = 0;
    int verticalZoomBucket = 0;
    int verticalScrollBucket = 0;
    uint32_t themeId = 0;

    bool operator==(const PianoRollTileKey& o) const noexcept {
        return surfaceEpoch == o.surfaceEpoch
            && tileIndex == o.tileIndex
            && tileStartContentX == o.tileStartContentX
            && tileWidthPx == o.tileWidthPx
            && contentHeightPx == o.contentHeightPx
            && pitchEpoch == o.pitchEpoch
            && notesEpoch == o.notesEpoch
            && waveformRevision == o.waveformRevision
            && timeGridRevision == o.timeGridRevision
            && visualPrefsRevision == o.visualPrefsRevision
            && placementProjectionRevision == o.placementProjectionRevision
            && zoomBucket == o.zoomBucket
            && verticalZoomBucket == o.verticalZoomBucket
            && verticalScrollBucket == o.verticalScrollBucket
            && themeId == o.themeId;
    }
};

struct PianoRollTile {
    PianoRollTileKey key;
    juce::Image image;
};

// Async DAW-style tile cache.
// - requestCoverage() enqueues tile generation on a worker thread.
// - Worker renders tiles tagged with the current surfaceEpoch.
// - Completed tiles are published to the tile map via AsyncUpdater (safe lifecycle:
//   cancelPendingUpdate() in destructor prevents dangling callbacks).
// - drawVisibleTiles() only reads already-published tiles (consume-only in paint).
// - invalidateAll() clears published tiles AND advances the surfaceEpoch counter,
//   preventing in-flight worker results from stale epochs from being published.
class PianoRollTimelineSurfaceCache : private juce::AsyncUpdater {
public:
    PianoRollTimelineSurfaceCache();
    ~PianoRollTimelineSurfaceCache();

    // Enqueue tile generation for the given viewport. Called from scroll/zoom/content
    // change triggers — NOT from paint(). Worker discards stale requests from older epochs.
    void requestCoverage(PianoRollRenderSnapshot snapshot,
                         TimelineViewportState viewport);

    // Remove all tiles (e.g. on theme change).
    void invalidateAll();

    // Consume-only: draw tiles already published by the worker. Called from paint().
    void drawVisibleTiles(juce::Graphics& g,
                          const TimelineViewportState& viewport) const;

private:
    static constexpr int kTileWidth = 1024;
    static constexpr int kPrefetchTiles = 2;

    // Published tile map — only written by message thread (via AsyncUpdater), read by paint.
    std::map<int, PianoRollTile> tiles_;
    uint64_t surfaceEpoch_ = 0;

    // Worker thread state
    std::thread worker_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::atomic<bool> shutdown_{false};

    // Pending request — single slot, latest wins
    struct PendingRequest {
        PianoRollRenderSnapshot snapshot;
        TimelineViewportState viewport;
        uint64_t surfaceEpoch = 0;
        bool valid = false;
    };
    PendingRequest pending_;

    // Published tiles from worker — transferred to tiles_ on message thread
    struct CompletedTile {
        int tileIndex;
        PianoRollTile tile;
        uint64_t surfaceEpoch;
    };
    std::mutex completedMutex_;
    std::vector<CompletedTile> completedTiles_;

    void workerLoop();
    void enqueueRequest(PianoRollRenderSnapshot snapshot, TimelineViewportState viewport);

    // AsyncUpdater callback — called on message thread to publish completed tiles
    void handleAsyncUpdate() override;

    // Build a tile key from snapshot + tile index
    PianoRollTileKey makeTileKey(const PianoRollRenderSnapshot& snapshot,
                                 const TimelineViewportState& viewport,
                                 int tileIndex) const;

    // Compute tile indices covering viewport + prefetch region
    void computeNeededTiles(const TimelineViewportState& viewport,
                            int& firstTile, int& lastTile) const;

};

} // namespace OpenTune
