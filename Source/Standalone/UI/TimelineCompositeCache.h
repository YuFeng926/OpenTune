#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <unordered_map>
#include <cstdint>
#include "../../Utils/TimelineDisplayMode.h"

namespace OpenTune {

// Background-plane generation parameters (BPM/time sig/display mode/theme only affect grid/lanes)
struct BackgroundGenerationSignature {
    double pixelsPerSecond = 0.0;
    float renderScale = 1.0f;   // 有效倍率（peer/host/root transform 真实绘制栈）
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
    float renderScale = 1.0f;   // 与背景平面同一有效倍率；变化时清除前景平面
    int trackHeight = 0;
    uint64_t contentRevision = 0;
    uint64_t selectionRevision = 0;

    bool operator==(const ForegroundGenerationSignature&) const;
};

class TimelineCompositeCache {
public:
    static constexpr int kTileWidthPx = 1024;
    static constexpr int kWorldTileHeight = 256;

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
        BackgroundGenerationSignature backgroundSignature;
        ForegroundGenerationSignature foregroundSignature;
        bool hasBackgroundSignature = false;
        bool hasForegroundSignature = false;

        bool hasBackgroundFor(const BackgroundGenerationSignature& signature) const noexcept
        {
            return background.isValid() && hasBackgroundSignature
                && backgroundSignature == signature;
        }

        bool hasForegroundFor(const ForegroundGenerationSignature& signature) const noexcept
        {
            return foreground.isValid() && hasForegroundSignature
                && foregroundSignature == signature;
        }
    };

    using TileBuilder = std::function<void(juce::Graphics&, juce::Rectangle<int>, TileKey)>;

    void prepare(
        const BackgroundGenerationSignature& bgSig,
        const ForegroundGenerationSignature& fgSig,
        int64_t firstTimeTile, int64_t lastTimeTile,
        int firstVertRow, int lastVertRow,
        TileBuilder backgroundBuilder,
        TileBuilder foregroundBuilder);

    void prepareTile(const BackgroundGenerationSignature& bgSig,
                     const ForegroundGenerationSignature& fgSig, TileKey key,
                     TileBuilder backgroundBuilder, TileBuilder foregroundBuilder);

    void removeTilesInTimeRange(int64_t firstTimeTile, int64_t lastTimeTile,
                                 int firstVertRow, int lastVertRow);

    const TileEntry* findTile(TileKey key) const noexcept;

    size_t getTileCount() const noexcept { return tiles_.size(); }

private:
    std::unordered_map<TileKey, TileEntry, TileKeyHash> tiles_;
};

} // namespace OpenTune
