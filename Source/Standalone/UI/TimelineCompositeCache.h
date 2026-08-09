#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>
#include <unordered_map>
#include <cstdint>
#include "../../Utils/TimelineDisplayMode.h"

namespace OpenTune {

// Background-plane generation parameters (BPM/time sig/display mode/theme only affect grid/lanes)
struct BackgroundGenerationSignature {
    double pixelsPerSecond = 0.0;
    int64_t dpiMilli = 1000;
    int trackHeight = 0;
    int visibleTrackCount = 2;
    int themeId = 0;
    TimelineDisplayMode displayMode = TimelineDisplayMode::Time;
    double tempo = 120.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;

    bool operator==(const BackgroundGenerationSignature&) const;
};

// Foreground-plane generation parameters: rasterization depends on horizontal zoom,
// content revision, and the current placement selection
struct ForegroundGenerationSignature {
    double pixelsPerSecond = 0.0;
    uint64_t contentRevision = 0;
    uint64_t selectionRevision = 0;

    bool operator==(const ForegroundGenerationSignature&) const;
};

class TimelineCompositeCache {
public:
    static constexpr int kTileWidthPx = 4096;
    static constexpr int kWorldTileHeight = 512;

    struct TileKey {
        int64_t timeTile = 0;
        int vertRow = 0;
        bool operator==(const TileKey& o) const { return timeTile == o.timeTile && vertRow == o.vertRow; }
    };

    struct TileKeyHash {
        size_t operator()(const TileKey& k) const {
            return std::hash<int64_t>{}(k.timeTile)
                ^ (std::hash<int>{}(k.vertRow) << 1);
        }
    };

    struct TileEntry {
        juce::Image background;
        juce::Image foreground;
    };

    using TileBuilder = std::function<void(juce::Graphics&, juce::Rectangle<int>, TileKey)>;

    void prepare(
        const BackgroundGenerationSignature& bgSig,
        const ForegroundGenerationSignature& fgSig,
        int64_t firstTimeTile, int64_t lastTimeTile,
        int firstVertRow, int lastVertRow,
        TileBuilder backgroundBuilder,
        TileBuilder foregroundBuilder);

    void removeTilesInTimeRange(int64_t firstTimeTile, int64_t lastTimeTile,
                                 int firstVertRow, int lastVertRow);

    const TileEntry* findTile(TileKey key) const noexcept;

    size_t getTileCount() const noexcept { return tiles_.size(); }

private:
    std::optional<BackgroundGenerationSignature> bgGen_;
    std::optional<ForegroundGenerationSignature> fgGen_;
    std::unordered_map<TileKey, TileEntry, TileKeyHash> tiles_;
};

} // namespace OpenTune
