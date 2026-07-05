#include "TimelinePatternCache.h"

namespace OpenTune {

const juce::Image& TimelinePatternCache::getPatternTile(
    const PatternTileKey& key,
    std::function<juce::Image(const PatternTileKey&)> buildFn) const
{
    auto it = tiles_.find(key);
    if (it != tiles_.end()) {
        if (it->second.pendingInvalidate) {
            it->second.image = buildFn(key);
            it->second.pendingInvalidate = false;
        }
        return it->second.image;
    }

    Tile tile;
    tile.key = key;
    tile.image = buildFn(key);
    tile.pendingInvalidate = false;
    auto [inserted, _] = tiles_.emplace(key, std::move(tile));
    return inserted->second.image;
}

void TimelinePatternCache::invalidatePatternTile(const PatternTileKey& key)
{
    auto it = tiles_.find(key);
    if (it != tiles_.end()) {
        it->second.pendingInvalidate = true;
    }
}

void TimelinePatternCache::clear()
{
    tiles_.clear();
}

} // namespace OpenTune
