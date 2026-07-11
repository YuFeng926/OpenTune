#include "TimelineCompositeCache.h"

namespace OpenTune {

bool GeometryState::operator==(const GeometryState& o) const {
    return contentViewportHeight == o.contentViewportHeight
        && rulerHeight == o.rulerHeight
        && pianoKeyWidth == o.pianoKeyWidth
        && minMidi == o.minMidi
        && maxMidi == o.maxMidi
        && pixelsPerSemitone == o.pixelsPerSemitone
        && verticalScrollOffset == o.verticalScrollOffset
        && trackHeight == o.trackHeight
        && scrollTopPx == o.scrollTopPx;
}

bool GenerationSignature::operator==(const GenerationSignature& o) const {
    return ppsMilli == o.ppsMilli
        && geometry == o.geometry
        && themeId == o.themeId
        && laneStyle == o.laneStyle
        && timeUnit == o.timeUnit
        && tempo == o.tempo
        && timeSigNumerator == o.timeSigNumerator
        && timeSigDenominator == o.timeSigDenominator
        && stableVisualSceneEpoch == o.stableVisualSceneEpoch;
}

void TimelineCompositeCache::prepare(
    const GenerationSignature& generation,
    int64_t firstTile,
    int64_t lastTile,
    int tileHeightPx,
    TileBuilder builder)
{
    jassert(firstTile >= 0);
    jassert(lastTile >= firstTile);
    jassert(tileHeightPx > 0);

    // Generation 变化 → 清空所有旧 tiles
    if (!generation_ || !(*generation_ == generation)) {
        tiles_.clear();
        generation_ = generation;
    }

    // 删除离开 coverage 的 tiles
    for (auto it = tiles_.begin(); it != tiles_.end(); ) {
        if (it->first < firstTile || it->first > lastTile)
            it = tiles_.erase(it);
        else
            ++it;
    }

    // 构建缺失的 tiles
    for (int64_t tile = firstTile; tile <= lastTile; ++tile) {
        if (tiles_.find(tile) != tiles_.end())
            continue;

        juce::Image image(juce::Image::ARGB, kTileWidthPx, tileHeightPx, true);
        juce::Graphics g(image);

        juce::Rectangle<int> tileBounds(0, 0, kTileWidthPx, tileHeightPx);
        builder(g, tileBounds, tile);

        tiles_[tile] = std::move(image);
    }
}

const juce::Image* TimelineCompositeCache::findTile(int64_t absoluteTile) const noexcept {
    auto it = tiles_.find(absoluteTile);
    return (it != tiles_.end()) ? &it->second : nullptr;
}

} // namespace OpenTune
