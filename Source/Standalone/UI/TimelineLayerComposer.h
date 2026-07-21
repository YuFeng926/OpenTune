#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <string>

namespace OpenTune {

enum class ThemeId : int;

struct RenderParams {
    double visibleStartSeconds = 0.0;
    double visibleEndSeconds = 0.0;
    double pixelsPerSecond = 100.0;
    int timeUnit = 0;
    int tempo = 120;
    int themeId = 0;
    float pixelsPerSemitone = 1.0f;
    float worldTopY = 0.0f;
    int rulerHeight = 0;
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


