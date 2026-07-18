#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <string>
#include <cstdint>

namespace OpenTune {

enum class ThemeId : int;

struct RenderParams {
    double visibleStartSeconds = 0.0;
    double visibleEndSeconds = 0.0;
    double pixelsPerSecond = 100.0;
    int timeUnit = 0;
    int tempo = 120;
    int themeId = 0;
    uint64_t verticalGeometry = 0;
    int laneStyle = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    std::string viewKind = "pianoroll";
};

struct TimelineRulerStyle {
    juce::Colour labelColour;
    juce::Colour tickColour;
    juce::Colour separatorColour;
    float tickStroke = 0.7f;
};

inline uint64_t encodePianoRollVerticalGeometry(float pixelsPerSemitone, int pianoKeyWidth, int rulerHeight,
                                                 float verticalScrollOffset, int contentViewportHeight) {
    uint64_t h = 0;
    h |= (static_cast<uint64_t>(static_cast<int>(pixelsPerSemitone * 100.0f)) & 0xFFFF);
    h |= ((static_cast<uint64_t>(pianoKeyWidth) & 0xFF) << 16);
    h |= ((static_cast<uint64_t>(rulerHeight) & 0xFF) << 24);
    h |= ((static_cast<uint64_t>(static_cast<int>(verticalScrollOffset)) & 0xFFFF) << 32);
    h |= ((static_cast<uint64_t>(contentViewportHeight) & 0xFFFF) << 48);
    return h;
}

inline float decodeVerticalScrollOffset(uint64_t verticalGeometry) {
    return static_cast<float>(static_cast<int>((verticalGeometry >> 32) & 0xFFFF));
}

inline uint64_t encodeArrangementVerticalGeometry(int contentHeight, int rulerHeight) {
    uint64_t h = 0;
    h |= (static_cast<uint64_t>(contentHeight) & 0xFFFF);
    h |= ((static_cast<uint64_t>(rulerHeight) & 0xFF) << 16);
    return h;
}

inline int decodeArrangementRulerHeight(uint64_t verticalGeometry) {
    return static_cast<int>((verticalGeometry >> 16) & 0xFF);
}

inline int encodeLaneStyle(bool showLanes, int scaleRootNote, int scaleType) {
    int h = 0;
    h |= (showLanes ? 1 : 0);
    h |= ((scaleRootNote & 0xFF) << 1);
    h |= ((scaleType & 0xFF) << 9);
    return h;
}

namespace TimelineLayerComposer {
    // Internal helpers (not part of public API)
    double selectBeatInterval(double pixelsPerBeat);
    double selectMarkerInterval(double pixelsPerSecond);
    void drawLaneStripRepeats(juce::Graphics& g, const RenderParams& params);
    TimelineRulerStyle resolveRulerStyle(const std::string& viewKind, ThemeId themeId);
    void drawGridLines(juce::Graphics& g, const RenderParams& params);
    void drawTimeRuler(juce::Graphics& g, const RenderParams& params);

} // namespace TimelineLayerComposer

} // namespace OpenTune


