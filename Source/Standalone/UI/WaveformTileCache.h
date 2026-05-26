#pragma once

/**
 * WaveformTileCache
 *
 * Bounded LRU cache of prepared waveform draw paths (juce::Path).
 * Keyed by materialization/source identity, zoom, visible time window, style,
 * and TimeGrid revision. Evicts least-recently-used tiles when the cache
 * exceeds kMaxTiles. Paint() consumes these pre-built paths rather than
 * looping over peaks each frame.
 *
 * This cache is Standalone-only UI state. NOT stored in the processor.
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <list>
#include <unordered_set>
#include "WaveformMipmap.h"

namespace OpenTune {

class WaveformTileCache {
public:
    struct TileKey {
        uint64_t materializationId;
        uint64_t sourceId;
        int zoomBucket;
        int64_t visibleStartBucket;
        int64_t visibleEndBucket;
        uint64_t styleHash;
        uint64_t timeGridRevision;

        bool operator==(const TileKey& other) const noexcept {
            return materializationId == other.materializationId
                && sourceId == other.sourceId
                && zoomBucket == other.zoomBucket
                && visibleStartBucket == other.visibleStartBucket
                && visibleEndBucket == other.visibleEndBucket
                && styleHash == other.styleHash
                && timeGridRevision == other.timeGridRevision;
        }
    };

    struct TileKeyHash {
        size_t operator()(const TileKey& key) const noexcept {
            size_t hash = std::hash<uint64_t>{}(key.materializationId);
            hash ^= std::hash<uint64_t>{}(key.sourceId) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
            hash ^= std::hash<int>{}(key.zoomBucket) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
            hash ^= std::hash<int64_t>{}(key.visibleStartBucket) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
            hash ^= std::hash<int64_t>{}(key.visibleEndBucket) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
            hash ^= std::hash<uint64_t>{}(key.styleHash) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
            hash ^= std::hash<uint64_t>{}(key.timeGridRevision) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    /** A prepared waveform draw tile. */
    struct Tile {
        juce::Path path;
        int widthPx = 0;
        int generation = 0;
    };

    static constexpr size_t kMaxTiles = 256;

    WaveformTileCache() = default;

    /** Move-only. */
    WaveformTileCache(WaveformTileCache&&) = default;
    WaveformTileCache& operator=(WaveformTileCache&&) = default;

    const Tile* get(uint64_t materializationId,
                    uint64_t sourceId,
                    int zoomBucket,
                    double visibleStartSeconds,
                    double visibleEndSeconds,
                    uint64_t styleHash,
                    uint64_t timeGridRevision) {
        const TileKey key = makeKey(materializationId,
                                    sourceId,
                                    zoomBucket,
                                    visibleStartSeconds,
                                    visibleEndSeconds,
                                    styleHash,
                                    timeGridRevision);
        auto it = tiles_.find(key);
        if (it == tiles_.end())
            return nullptr;
        touchLru(it->second.lruIt);
        return &it->second.tile;
    }

    const Tile& getOrCreate(uint64_t materializationId,
                            uint64_t sourceId,
                            int zoomBucket,
                            const WaveformMipmap& mipmap,
                            float gain,
                            const juce::Rectangle<int>& waveformBounds,
                            double visibleStartSeconds,
                            double visibleEndSeconds,
                            uint64_t styleHash,
                            uint64_t timeGridRevision)
    {
        const TileKey key = makeKey(materializationId,
                                    sourceId,
                                    zoomBucket,
                                    visibleStartSeconds,
                                    visibleEndSeconds,
                                    styleHash,
                                    timeGridRevision);
        auto it = tiles_.find(key);
        if (it != tiles_.end()) {
            touchLru(it->second.lruIt);
            return it->second.tile;
        }

        evictIfNeeded();

        Tile newTile = buildTile(mipmap, gain, waveformBounds, visibleStartSeconds, visibleEndSeconds);

        auto lruIt = lruOrder_.insert(lruOrder_.end(), key);
        auto result = tiles_.emplace(key, Entry{std::move(newTile), lruIt});
        return result.first->second.tile;
    }

    void remove(uint64_t materializationId) {
        for (auto it = tiles_.begin(); it != tiles_.end(); ) {
            if (it->first.materializationId == materializationId) {
                lruOrder_.erase(it->second.lruIt);
                it = tiles_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void prune(const std::unordered_set<uint64_t>& alive) {
        for (auto it = tiles_.begin(); it != tiles_.end(); ) {
            if (alive.find(it->first.materializationId) == alive.end()) {
                lruOrder_.erase(it->second.lruIt);
                it = tiles_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear() {
        tiles_.clear();
        lruOrder_.clear();
    }

    size_t size() const noexcept { return tiles_.size(); }

private:
    struct Entry {
        Tile tile;
        std::list<TileKey>::iterator lruIt;
    };

    std::unordered_map<TileKey, Entry, TileKeyHash> tiles_;
    std::list<TileKey> lruOrder_;

    void touchLru(std::list<TileKey>::iterator it) {
        lruOrder_.splice(lruOrder_.end(), lruOrder_, it);
    }

    static int64_t bucketTime(double seconds) noexcept
    {
        return static_cast<int64_t>(std::llround(seconds * 1000.0));
    }

    static TileKey makeKey(uint64_t materializationId,
                           uint64_t sourceId,
                           int zoomBucket,
                           double visibleStartSeconds,
                           double visibleEndSeconds,
                           uint64_t styleHash,
                           uint64_t timeGridRevision) noexcept
    {
        return TileKey {
            materializationId,
            sourceId,
            zoomBucket,
            bucketTime(visibleStartSeconds),
            bucketTime(visibleEndSeconds),
            styleHash,
            timeGridRevision
        };
    }

    void evictIfNeeded() {
        while (tiles_.size() >= kMaxTiles && !lruOrder_.empty()) {
            auto key = lruOrder_.front();
            lruOrder_.pop_front();
            tiles_.erase(key);
        }
    }

    static Tile buildTile(const WaveformMipmap& mipmap,
                          float gain,
                          const juce::Rectangle<int>& waveformBounds,
                          double visibleStartSeconds,
                          double visibleEndSeconds)
    {
        Tile tile;
        const double visibleDurationSeconds = visibleEndSeconds - visibleStartSeconds;
        if (visibleDurationSeconds <= 0.0)
            return tile;

        const double pixelsPerSecond = static_cast<double>(juce::jmax(1, waveformBounds.getWidth()))
                                     / visibleDurationSeconds;
        const int levelIndex = mipmap.selectBestLevelIndex(pixelsPerSecond);
        const auto& level = mipmap.getLevel(levelIndex);

        if (level.peaks.empty())
            return tile;

        const float midY = static_cast<float>(waveformBounds.getCentreY());
        const float halfH = waveformBounds.getHeight() * 0.45f;
        const int x0 = waveformBounds.getX();
        const int placementWidth = waveformBounds.getWidth();

        const int samplesPerPeak = WaveformMipmap::kSamplesPerPeak[levelIndex];
        const double timePerPeak = static_cast<double>(samplesPerPeak) / WaveformMipmap::kBaseSampleRate;

        const int64_t numPeaks = static_cast<int64_t>(level.peaks.size());
        const int64_t builtPeaks = level.complete ? numPeaks : level.buildProgress;

        if (builtPeaks <= 0)
            return tile;

        juce::Path path;

        for (int x = 0; x < placementWidth; ++x)
        {
            const double visibleLocalTime = (static_cast<double>(x) / static_cast<double>(juce::jmax(1, placementWidth)))
                                          * visibleDurationSeconds;
            const double materializationTime = visibleStartSeconds + visibleLocalTime;

            const int64_t peakIndex = static_cast<int64_t>(materializationTime / timePerPeak);

            if (peakIndex < 0 || peakIndex >= builtPeaks)
                continue;

            const auto& peak = level.peaks[static_cast<std::size_t>(peakIndex)];
            const float magnitude = peak.getMagnitude() * gain;
            float displayHeight = magnitude * halfH * 2.0f;

            if (magnitude > 0.0001f)
                displayHeight = juce::jmax(displayHeight, 2.0f);

            const float y1 = midY - displayHeight * 0.5f;
            const float y2 = midY + displayHeight * 0.5f;

            path.startNewSubPath(static_cast<float>(x0 + x), y1);
            path.lineTo(static_cast<float>(x0 + x), y2);
        }

        tile.path = std::move(path);
        tile.widthPx = placementWidth;
        tile.generation = 1;
        return tile;
    }
};

} // namespace OpenTune
