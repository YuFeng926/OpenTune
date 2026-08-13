#include "TimelineCompositeCache.h"

namespace OpenTune {

bool BackgroundGenerationSignature::operator==(const BackgroundGenerationSignature& o) const {
    return pixelsPerSecond == o.pixelsPerSecond
        && dpiMilli == o.dpiMilli
        && trackHeight == o.trackHeight
        && visibleTrackCount == o.visibleTrackCount
        && themeId == o.themeId
        && displayMode == o.displayMode
        && tempo == o.tempo
        && timeSigNumerator == o.timeSigNumerator
        && timeSigDenominator == o.timeSigDenominator;
}

bool ForegroundGenerationSignature::operator==(const ForegroundGenerationSignature& o) const {
    return pixelsPerSecond == o.pixelsPerSecond
        && trackHeight == o.trackHeight
        && contentRevision == o.contentRevision
        && selectionRevision == o.selectionRevision;
}

void TimelineCompositeCache::prepare(
    const BackgroundGenerationSignature& bgSig,
    const ForegroundGenerationSignature& fgSig,
    int64_t firstTimeTile, int64_t lastTimeTile,
    int firstVertRow, int lastVertRow,
    TileBuilder backgroundBuilder,
    TileBuilder foregroundBuilder)
{
    // Background signature changed → clear all background planes
    const bool bgChanged = !bgGen_ || !(*bgGen_ == bgSig);
    if (bgChanged) {
        for (auto& kv : tiles_)
            kv.second.background = juce::Image();
        bgGen_ = bgSig;
    }

    // Foreground signature changed → clear all foreground planes
    const bool fgChanged = !fgGen_ || !(*fgGen_ == fgSig);
    if (fgChanged) {
        for (auto& kv : tiles_)
            kv.second.foreground = juce::Image();
        fgGen_ = fgSig;
    }

    // Evict tiles outside coverage
    for (auto it = tiles_.begin(); it != tiles_.end(); ) {
        const auto& key = it->first;
        if (key.timeTile < firstTimeTile || key.timeTile > lastTimeTile
            || key.vertRow < firstVertRow || key.vertRow > lastVertRow)
            it = tiles_.erase(it);
        else
            ++it;
    }

    // Build missing tiles (2D traversal)
    for (int64_t tt = firstTimeTile; tt <= lastTimeTile; ++tt) {
        for (int vr = firstVertRow; vr <= lastVertRow; ++vr) {
            TileKey key{tt, vr};
            auto it = tiles_.find(key);
            const bool missing = (it == tiles_.end());
            const bool bgMissing = missing || (it->second.background.isValid() == false);
            const bool fgMissing = missing || (it->second.foreground.isValid() == false);

            if (!bgMissing && !fgMissing)
                continue;

            TileEntry entry;
            if (missing) {
                // New tile
            } else {
                entry = std::move(it->second);
            }

            if (bgMissing) {
                entry.background = juce::Image(
                    juce::Image::ARGB, kTileWidthPx, kWorldTileHeight, true);
                juce::Graphics g(entry.background);
                juce::Rectangle<int> b(0, 0, kTileWidthPx, kWorldTileHeight);
                backgroundBuilder(g, b, key);
            }

            if (fgMissing) {
                entry.foreground = juce::Image(
                    juce::Image::ARGB, kTileWidthPx, kWorldTileHeight, true);
                juce::Graphics g(entry.foreground);
                juce::Rectangle<int> b(0, 0, kTileWidthPx, kWorldTileHeight);
                foregroundBuilder(g, b, key);
            }

            tiles_[key] = std::move(entry);
        }
    }
}

void TimelineCompositeCache::removeTilesInTimeRange(
    int64_t firstTimeTile, int64_t lastTimeTile,
    int firstVertRow, int lastVertRow)
{
    for (auto it = tiles_.begin(); it != tiles_.end(); ) {
        const auto& key = it->first;
        if (key.timeTile >= firstTimeTile && key.timeTile <= lastTimeTile
            && key.vertRow >= firstVertRow && key.vertRow <= lastVertRow)
            it = tiles_.erase(it);
        else
            ++it;
    }
}

const TimelineCompositeCache::TileEntry*
TimelineCompositeCache::findTile(TileKey key) const noexcept {
    auto it = tiles_.find(key);
    return (it != tiles_.end()) ? &it->second : nullptr;
}

} // namespace OpenTune
