#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>
#include <unordered_map>
#include <cstdint>

namespace OpenTune {

struct GeometryState {
    float minMidi = 0.0f;
    float maxMidi = 127.0f;
    float pixelsPerSemitone = 0.0f;
    int trackHeight = 0;

    bool operator==(const GeometryState&) const;
};

struct GenerationSignature {
    int64_t ppsMilli = 0;              // round(pps * 1000)
    int64_t dpiMilli = 1000;            // round(desktop scale factor * 1000)
    GeometryState geometry;
    int themeId = 0;
    int laneStyle = 0;
    int timeUnit = 0;
    int tempo = 120;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    uint64_t contentRevision = 0;

    bool operator==(const GenerationSignature&) const;
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
        const GenerationSignature& generation,
        int64_t firstTimeTile, int64_t lastTimeTile,
        int firstVertRow, int lastVertRow,
        TileBuilder backgroundBuilder,
        TileBuilder foregroundBuilder,
        bool allocateForeground = true);

    const TileEntry* findTile(TileKey key) const noexcept;

    size_t getTileCount() const noexcept { return tiles_.size(); }

private:
    std::optional<GenerationSignature> generation_;
    std::unordered_map<TileKey, TileEntry, TileKeyHash> tiles_;
};

} // namespace OpenTune
