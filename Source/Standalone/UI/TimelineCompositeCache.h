#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>
#include <unordered_map>
#include <cstdint>

namespace OpenTune {

struct GeometryState {
    int contentViewportHeight = 0;
    int rulerHeight = 0;

    // PianoRoll specific
    int pianoKeyWidth = 0;
    float minMidi = 0.0f;
    float maxMidi = 127.0f;
    float pixelsPerSemitone = 0.0f;
    float verticalScrollOffset = 0.0f;

    // Arrangement specific
    int trackHeight = 0;
    int scrollTopPx = 0;

    bool operator==(const GeometryState&) const;
};

struct GenerationSignature {
    int64_t ppsMilli = 0;              // round(pps * 1000)
    GeometryState geometry;
    int themeId = 0;
    int laneStyle = 0;
    int timeUnit = 0;
    int tempo = 120;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    uint64_t stableVisualSceneEpoch = 0;

    bool operator==(const GenerationSignature&) const;
};

class TimelineCompositeCache {
public:
    static constexpr int kTileWidthPx = 4096;

    using TileBuilder = std::function<void(juce::Graphics&, juce::Rectangle<int>, int64_t)>;

    void prepare(
        const GenerationSignature& generation,
        int64_t firstTile,
        int64_t lastTile,
        int tileHeightPx,
        TileBuilder builder);

    const juce::Image* findTile(int64_t absoluteTile) const noexcept;

    size_t getTileCount() const noexcept { return tiles_.size(); }

private:
    std::optional<GenerationSignature> generation_;
    std::unordered_map<int64_t, juce::Image> tiles_;
};

} // namespace OpenTune