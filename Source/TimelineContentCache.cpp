#include "TimelineContentCache.h"

namespace OpenTune {

// ============================================================================
// getOrBuildTile — canonical content tile builder
//
// If tile exists for key → return cached.
// Otherwise: allocate image, call painter to render, cache and return.
// The cache is the SINGLE render path. No transparent-then-render-later.
// ============================================================================
const juce::Image& TimelineContentCache::getOrBuildTile(
    const ContentTileKey& key,
    int heightPx,
    ContentTilePainter painter)
{
    auto it = tiles_.find(key);
    if (it != tiles_.end())
        return it->second;

    jassert(key.contentKey.isValid());
    jassert(heightPx > 0);

    const int widthPx = static_cast<int>(std::llround(
        (key.endSeconds - key.startSeconds) * key.pixelsPerSecond));
    jassert(widthPx > 0);

    juce::Image image(juce::Image::ARGB, widthPx, heightPx, true);
    {
        juce::Graphics g(image);
        painter(g, key, image.getBounds());
    }

    auto [emplaceIt, emplaceSuccess] = tiles_.emplace(key, std::move(image));
    return emplaceIt->second;
}

// ============================================================================
// clear — 清空所有缓存
// ============================================================================
void TimelineContentCache::clear()
{
    tiles_.clear();
}

} // namespace OpenTune
