#include "TimelineCompositeCache.h"

#include <cmath>

namespace OpenTune {

bool BackgroundGenerationSignature::operator==(const BackgroundGenerationSignature& o) const {
    return pixelsPerSecond == o.pixelsPerSecond
        && renderScale == o.renderScale
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
        && renderScale == o.renderScale
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
    // 位图物理尺寸 = ceil(逻辑 tile 尺寸 × 有效倍率)；离屏 Graphics 施加同一倍率，
    // builder 继续用逻辑坐标绘制。
    const float renderScale = bgSig.renderScale;
    const int physTileW = static_cast<int>(std::ceil(static_cast<float>(kTileWidthPx) * renderScale));
    const int physTileH = static_cast<int>(std::ceil(static_cast<float>(kWorldTileHeight) * renderScale));

    for (int64_t tt = firstTimeTile; tt <= lastTimeTile; ++tt) {
        for (int vr = firstVertRow; vr <= lastVertRow; ++vr) {
            TileKey key{tt, vr};
            auto& entry = tiles_[key];
            const bool bgMissing = !entry.hasBackgroundFor(bgSig);
            const bool fgMissing = !entry.hasForegroundFor(fgSig);

            if (!bgMissing && !fgMissing)
                continue;

            if (bgMissing) {
                entry.background = juce::Image(juce::Image::ARGB, physTileW, physTileH, true);
                juce::Graphics g(entry.background);
                g.addTransform(juce::AffineTransform::scale(renderScale));
                juce::Rectangle<int> b(0, 0, kTileWidthPx, kWorldTileHeight);
                backgroundBuilder(g, b, key);
                entry.backgroundSignature = bgSig;
                entry.hasBackgroundSignature = true;
            }

            if (fgMissing) {
                entry.foreground = juce::Image(juce::Image::ARGB, physTileW, physTileH, true);
                juce::Graphics g(entry.foreground);
                g.addTransform(juce::AffineTransform::scale(renderScale));
                juce::Rectangle<int> b(0, 0, kTileWidthPx, kWorldTileHeight);
                foregroundBuilder(g, b, key);
                entry.foregroundSignature = fgSig;
                entry.hasForegroundSignature = true;
            }
        }
    }
}

void TimelineCompositeCache::prepareTile(
    const BackgroundGenerationSignature& bgSig,
    const ForegroundGenerationSignature& fgSig, TileKey key,
    TileBuilder backgroundBuilder, TileBuilder foregroundBuilder)
{
    const float scale = bgSig.renderScale;
    const int width = static_cast<int>(std::ceil(kTileWidthPx * scale));
    const int height = static_cast<int>(std::ceil(kWorldTileHeight * scale));
    auto& entry = tiles_[key];
    if (!entry.hasBackgroundFor(bgSig)) {
        entry.background = juce::Image(juce::Image::ARGB, width, height, true);
        juce::Graphics g(entry.background);
        g.addTransform(juce::AffineTransform::scale(scale));
        backgroundBuilder(g, {0, 0, kTileWidthPx, kWorldTileHeight}, key);
        entry.backgroundSignature = bgSig;
        entry.hasBackgroundSignature = true;
    }
    if (!entry.hasForegroundFor(fgSig)) {
        entry.foreground = juce::Image(juce::Image::ARGB, width, height, true);
        juce::Graphics g(entry.foreground);
        g.addTransform(juce::AffineTransform::scale(scale));
        foregroundBuilder(g, {0, 0, kTileWidthPx, kWorldTileHeight}, key);
        entry.foregroundSignature = fgSig;
        entry.hasForegroundSignature = true;
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
