#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Content/ContentKey.h"
#include <cstdint>
#include <string>
#include <functional>
#include <unordered_map>
#include <utility>

namespace OpenTune {

// ============================================================================
// ContentSlot — typed content type identifier
// ============================================================================
enum class ContentSlot : int {
    Waveform = 0,
    Notes = 1,
    F0 = 2,
    TimeAnchors = 3,
    ArrangementClips = 4,
};

// ============================================================================
// ContentTileKey — uniquely identifies a content tile
// contentKey uses typed ContentKey (includes domainKind/objectId/sourceWindowDiscriminator)
// revision is the generation counter, NOT part of content identity
// ============================================================================
struct ContentTileKey {
    std::string viewKind;        // "pianoroll" / "arrangement"
    ContentSlot slot;            // typed slot identity
    ContentKey contentKey;       // typed content identity
    double startSeconds;
    double endSeconds;
    double pixelsPerSecond;
    std::uint64_t verticalGeometry;  // vertical layout hash (64-bit for full vertical window)
    std::uint64_t revision;      // immutable generation version

    bool operator==(const ContentTileKey& other) const noexcept
    {
        return viewKind == other.viewKind
            && slot == other.slot
            && contentKey == other.contentKey
            && startSeconds == other.startSeconds
            && endSeconds == other.endSeconds
            && pixelsPerSecond == other.pixelsPerSecond
            && verticalGeometry == other.verticalGeometry
            && revision == other.revision;
    }

    bool operator!=(const ContentTileKey& other) const noexcept { return !(*this == other); }
};

// ============================================================================
// ContentTileKeyHasher — custom hasher delegating to std::hash<ContentKey>
// ============================================================================
struct ContentTileKeyHasher {
    std::size_t operator()(const ContentTileKey& key) const noexcept
    {
        std::size_t h = 0;
        const auto fprime = 0x100000001b3ull;

        for (char c : key.viewKind) h = (h ^ static_cast<std::size_t>(c)) * fprime;
        h = (h ^ static_cast<std::size_t>(key.slot)) * fprime;
        h = (h ^ std::hash<ContentKey>{}(key.contentKey)) * fprime;

        auto dhash = [&h, fprime](double v) {
            auto bits = static_cast<std::uint64_t>(std::llround(v * 1000.0));
            h = (h ^ bits) * fprime;
        };
        dhash(key.startSeconds);
        dhash(key.endSeconds);
        dhash(key.pixelsPerSecond);

        // Hash 64-bit verticalGeometry by splitting into two 32-bit parts
        h = (h ^ static_cast<std::size_t>(key.verticalGeometry & 0xFFFFFFFF)) * fprime;
        h = (h ^ static_cast<std::size_t>((key.verticalGeometry >> 32) & 0xFFFFFFFF)) * fprime;
        h = (h ^ static_cast<std::size_t>(key.revision)) * fprime;
        return h;
    }
};

// ============================================================================
// TimelineContentCache — canonical content tile cache
//
// getOrBuildTile: if tile exists, return cached. Otherwise allocate image AND
// call painter callback to render into it. The cache is the single canonical
// render path — no transparent-then-render-later pattern.
// ============================================================================
class TimelineContentCache {
public:
    TimelineContentCache() = default;

    // Painter: receives (Graphics& g, ContentTileKey key, Rectangle<int> tileBounds)
    using ContentTilePainter = std::function<void(juce::Graphics&, const ContentTileKey&, juce::Rectangle<int>)>;

    // Canonical tile builder: cache manages allocation + rendering.
    // Returns cached tile if key matches, otherwise allocates and calls painter.
    const juce::Image& getOrBuildTile(
        const ContentTileKey& key,
        int heightPx,
        ContentTilePainter painter);

    // Clear all cached tiles
    void clear();

private:
    std::unordered_map<ContentTileKey, juce::Image, ContentTileKeyHasher> tiles_;
};

} // namespace OpenTune
