#include "PianoRollTimelineSurfaceCache.h"
#include "PianoRollTileRenderer.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune {

PianoRollTimelineSurfaceCache::PianoRollTimelineSurfaceCache() {
    worker_ = std::thread(&PianoRollTimelineSurfaceCache::workerLoop, this);
}

PianoRollTimelineSurfaceCache::~PianoRollTimelineSurfaceCache() {
    shutdown_.store(true, std::memory_order_release);
    queueCv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    cancelPendingUpdate();
}

void PianoRollTimelineSurfaceCache::requestCoverage(PianoRollRenderSnapshot snapshot,
                                                     TimelineViewportState viewport) {
    enqueueRequest(std::move(snapshot), viewport);
}

void PianoRollTimelineSurfaceCache::invalidateAll() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    tiles_.clear();
    surfaceEpoch_++;
}

void PianoRollTimelineSurfaceCache::drawVisibleTiles(juce::Graphics& g,
                                                      const TimelineViewportState& viewport) const {
    for (const auto& [index, tile] : tiles_) {
        if (!tile.image.isValid()) continue;
        const int drawX = tile.key.tileStartContentX - viewport.scrollOffsetPx + viewport.contentStartX;
        if (drawX + tile.key.tileWidthPx < viewport.contentStartX) continue;
        if (drawX > viewport.viewportWidthPx) continue;
        g.drawImageAt(tile.image, drawX, 0);
    }
}

void PianoRollTimelineSurfaceCache::workerLoop() {
    while (!shutdown_.load(std::memory_order_acquire)) {
        PendingRequest req;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] {
                return shutdown_.load(std::memory_order_acquire) || pending_.valid;
            });
            if (shutdown_.load(std::memory_order_acquire)) return;
            if (!pending_.valid) continue;

            req = pending_;
            pending_.valid = false;
        }

        // Compute needed tile range (viewport + prefetch)
        int firstTile = 0, lastTile = 0;
        computeNeededTiles(req.viewport, firstTile, lastTile);

        // Unconditionally render all needed tiles. Worker does NOT read the published
        // tile map — message thread owns tiles_ exclusively. Deduplication happens in
        // handleAsyncUpdate() where tiles with a matching, already-published key are
        // skipped. This keeps the hot path (worker render-to-publish) simple and avoids
        // lock contention on tiles_.
        std::vector<CompletedTile> completed;
        for (int ti = firstTile; ti <= lastTile; ++ti) {
            if (shutdown_.load(std::memory_order_acquire)) return;

            PianoRollTileKey key = makeTileKey(req.snapshot, req.viewport, ti);
            key.surfaceEpoch = req.surfaceEpoch;

            PianoRollTile tile;
            tile.key = key;
            tile.image = PianoRollTileRenderer::renderTile(req.snapshot, key);

            completed.push_back({ti, std::move(tile), req.surfaceEpoch});
        }

        // Publish completed tiles to message thread
        if (!completed.empty()) {
            {
                std::lock_guard<std::mutex> lock(completedMutex_);
                for (auto& c : completed) {
                    completedTiles_.push_back(std::move(c));
                }
            }
            triggerAsyncUpdate();
        }
    }
}

void PianoRollTimelineSurfaceCache::handleAsyncUpdate() {
    std::vector<CompletedTile> toPublish;
    {
        std::lock_guard<std::mutex> lock(completedMutex_);
        toPublish = std::move(completedTiles_);
        completedTiles_.clear();
    }

    std::lock_guard<std::mutex> lock(queueMutex_);
    for (auto& c : toPublish) {
        // 精确匹配：只接受当前 surface epoch 的 tile
        if (c.surfaceEpoch != surfaceEpoch_) continue;

        // 用完整 key 去重：同 key 跳过，不同 key 覆盖
        auto it = tiles_.find(c.tileIndex);
        if (it != tiles_.end() && it->second.key == c.tile.key) continue;

        tiles_[c.tileIndex] = std::move(c.tile);
    }
}

void PianoRollTimelineSurfaceCache::enqueueRequest(PianoRollRenderSnapshot snapshot,
                                                    TimelineViewportState viewport) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    // 不再递增 surfaceEpoch_！纯滚动不改变内容身份
    pending_.snapshot = std::move(snapshot);
    pending_.viewport = viewport;
    pending_.surfaceEpoch = surfaceEpoch_;
    pending_.valid = true;
    queueCv_.notify_one();
}

PianoRollTileKey PianoRollTimelineSurfaceCache::makeTileKey(const PianoRollRenderSnapshot& snapshot,
                                                            const TimelineViewportState& viewport,
                                                            int tileIndex) const {
    PianoRollTileKey key;
    key.tileIndex = tileIndex;
    key.tileStartContentX = tileIndex * kTileWidth;
    key.tileWidthPx = kTileWidth;
    key.contentHeightPx = viewport.viewportHeightPx;
    key.pitchEpoch = snapshot.pitchEpoch;
    key.notesEpoch = snapshot.notesEpoch;
    key.waveformRevision = snapshot.waveformRevision;
    key.timeGridRevision = snapshot.timeGridRevision;
    key.visualPrefsRevision = snapshot.visualPrefsRevision;
    key.placementProjectionRevision = snapshot.placementProjectionRevision;
    key.zoomBucket = static_cast<int>(snapshot.pixelsPerSecond * 100.0 + 0.5);
    key.verticalZoomBucket = static_cast<int>(snapshot.pixelsPerSemitone * 1000.0f + 0.5f);
    key.verticalScrollBucket = static_cast<int>(snapshot.verticalScrollOffset * 1000.0f + 0.5f);
    key.themeId = snapshot.themeId;
    return key;
}

void PianoRollTimelineSurfaceCache::computeNeededTiles(const TimelineViewportState& viewport,
                                                        int& firstTile, int& lastTile) const {
    const int contentWidth = viewport.viewportWidthPx;
    if (contentWidth <= 0) {
        firstTile = lastTile = 0;
        return;
    }

    firstTile = (viewport.scrollOffsetPx) / kTileWidth - kPrefetchTiles;
    lastTile = (viewport.scrollOffsetPx + contentWidth) / kTileWidth + kPrefetchTiles;

    firstTile = std::max(0, firstTile);
}

} // namespace OpenTune
