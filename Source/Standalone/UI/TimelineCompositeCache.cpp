#include "TimelineCompositeCache.h"

namespace OpenTune {

bool GeometryState::operator==(const GeometryState& o) const {
    return minMidi == o.minMidi
        && maxMidi == o.maxMidi
        && pixelsPerSemitone == o.pixelsPerSemitone
        && trackHeight == o.trackHeight;
}

bool GenerationSignature::operator==(const GenerationSignature& o) const {
    return pixelsPerSecond == o.pixelsPerSecond
        && dpiMilli == o.dpiMilli
        && geometry == o.geometry
        && themeId == o.themeId
        && laneStyle == o.laneStyle
        && timeUnit == o.timeUnit
        && tempo == o.tempo
        && timeSigNumerator == o.timeSigNumerator
        && timeSigDenominator == o.timeSigDenominator
        && showOriginalF0 == o.showOriginalF0
        && showCorrectedF0 == o.showCorrectedF0
        && showUnvoicedFrames == o.showUnvoicedFrames
        && contentRevision == o.contentRevision;
}

void TimelineCompositeCache::prepare(
    const GenerationSignature& generation,
    int64_t firstTimeTile, int64_t lastTimeTile,
    int firstVertRow, int lastVertRow,
    TileBuilder backgroundBuilder,
    TileBuilder foregroundBuilder,
    bool allocateForeground)
{
    // Generation 变化 → 清空所有旧 tiles
    if (!generation_ || !(*generation_ == generation)) {
        tiles_.clear();
        generation_ = generation;
    }

    // 淘汰 coverage 外的 tiles（逐行检查 time 范围）
    for (auto it = tiles_.begin(); it != tiles_.end(); ) {
        const auto& key = it->first;
        if (key.timeTile < firstTimeTile || key.timeTile > lastTimeTile
            || key.vertRow < firstVertRow || key.vertRow > lastVertRow)
            it = tiles_.erase(it);
        else
            ++it;
    }

    // 构建缺失 tiles（二维遍历）
    for (int64_t tt = firstTimeTile; tt <= lastTimeTile; ++tt) {
        for (int vr = firstVertRow; vr <= lastVertRow; ++vr) {
            TileKey key{tt, vr};
            if (tiles_.find(key) != tiles_.end())
                continue;

            TileEntry entry;
            // Background plane
            entry.background = juce::Image(
                juce::Image::ARGB, kTileWidthPx, kWorldTileHeight, true);
            {
                juce::Graphics g(entry.background);
                juce::Rectangle<int> b(0, 0, kTileWidthPx, kWorldTileHeight);
                backgroundBuilder(g, b, key);
            }

            // Foreground plane (Arrangement skips this)
            if (allocateForeground) {
                entry.foreground = juce::Image(
                    juce::Image::ARGB, kTileWidthPx, kWorldTileHeight, true);
                {
                    juce::Graphics g(entry.foreground);
                    juce::Rectangle<int> b(0, 0, kTileWidthPx, kWorldTileHeight);
                    foregroundBuilder(g, b, key);
                }
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
