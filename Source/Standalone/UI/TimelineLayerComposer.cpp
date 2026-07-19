#include "TimelineLayerComposer.h"
#include "UIColors.h"
#include "UiAssets.h"
#include "ThemeTokens.h"
#include "../../Utils/NoteGeneratorTypes.h"
#include <cmath>
#include <algorithm>
#include <array>
#include <cstdint>

namespace OpenTune {

// ============================================================================
// 刻度间隔选择
// ============================================================================

double TimelineLayerComposer::selectBeatInterval(double pixelsPerBeat) {
    if (pixelsPerBeat < 2.5) return 32.0;
    if (pixelsPerBeat < 5.0) return 16.0;
    if (pixelsPerBeat < 10.0) return 8.0;
    if (pixelsPerBeat < 40.0) return 4.0;
    return 1.0;
}

double TimelineLayerComposer::selectMarkerInterval(double pixelsPerSecond) {
    if (pixelsPerSecond < 1.33) return 60.0;
    if (pixelsPerSecond < 4.0) return 30.0;
    if (pixelsPerSecond < 8.0) return 10.0;
    if (pixelsPerSecond < 40.0) return 5.0;
    return 1.0;
}

// ============================================================================
// 解码 laneStyle
// ============================================================================

static bool decodeShowLanes(int laneStyle) {
    return (laneStyle & 0x1) != 0;
}

static int decodeScaleRootNote(int laneStyle) {
    return (laneStyle >> 1) & 0xFF;
}

static int decodeScaleType(int laneStyle) {
    return (laneStyle >> 9) & 0xFF;
}

// ============================================================================
// Scale pitch-class helper
// ============================================================================
static std::array<bool, 12> buildInScalePitchClasses(int scaleType, int rootNote) noexcept {
    std::array<bool, 12> result{};
    static constexpr int kScaleTypeChromatic = 3;
    if (scaleType == kScaleTypeChromatic) {
        result.fill(true);
        return result;
    }
    result.fill(false);
    const int rootPc = juce::jlimit(0, 11, rootNote % 12);

    ScaleMode mode = ScaleMode::Major;
    switch (scaleType) {
        case 1: mode = ScaleMode::Major; break;
        case 2: mode = ScaleMode::Minor; break;
        case 4: mode = ScaleMode::HarmonicMinor; break;
        case 5: mode = ScaleMode::Dorian; break;
        case 6: mode = ScaleMode::Mixolydian; break;
        case 7: mode = ScaleMode::PentatonicMajor; break;
        case 8: mode = ScaleMode::PentatonicMinor; break;
        default: mode = ScaleMode::Major; break;
    }
    int count = 0;
    const int* intervals = ScaleSnapConfig::semitones(mode, count);
    for (int i = 0; i < count; ++i)
        result[static_cast<std::size_t>((rootPc + intervals[i]) % 12)] = true;
    return result;
}

// ============================================================================
// resolveRulerStyle — 从 themeId 推导 ruler 视觉样式
// ============================================================================
TimelineRulerStyle TimelineLayerComposer::resolveRulerStyle(const std::string& viewKind, ThemeId themeId) {
    TimelineRulerStyle style;
    const bool isArrangement = (viewKind == "arrangement");

    if (themeId == ThemeId::Aurora) {
        style.labelColour = UIColors::textSecondary.withMultipliedAlpha(0.48f);
        style.tickColour = UIColors::gridLine.withAlpha(0.080f);
        style.separatorColour = UIColors::gridLine.withAlpha(0.060f);
        style.tickStroke = isArrangement ? 1.0f : 0.7f;
    } else if (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) {
        style.labelColour = UIColors::textSecondary.withAlpha(0.58f);
        style.tickColour = UIColors::pianoRollGrid.withAlpha(0.052f);
        style.separatorColour = UIColors::pianoRollGrid.withAlpha(0.040f);
        style.tickStroke = 0.7f;
    } else {
        style.labelColour = UIColors::textSecondary;
        style.tickColour = UIColors::gridLine;
        style.separatorColour = UIColors::panelBorder;
        style.tickStroke = 1.0f;
    }
    return style;
}

// ============================================================================
// drawGridLines — 在 tile 内按 absolute time 绘制网格线
// ============================================================================
void TimelineLayerComposer::drawGridLines(juce::Graphics& g, const RenderParams& params) {
    const int w = params.viewportWidth;
    const int h = params.viewportHeight;
    const auto themeId = static_cast<ThemeId>(params.themeId);
    const double pps = params.pixelsPerSecond;

    if (params.timeUnit == 1) { // Bars
        double bpm = static_cast<double>(params.tempo);
        if (bpm <= 0.0) bpm = 120.0;
        double secondsPerBeat = 60.0 / bpm;
        double pixelsPerBeat = pps * secondsPerBeat;
        double beatInterval = selectBeatInterval(pixelsPerBeat);

        int64_t startBeat = static_cast<int64_t>(params.visibleStartSeconds / secondsPerBeat);
        if (startBeat < 0) startBeat = 0;
        startBeat = (startBeat / static_cast<int64_t>(beatInterval)) * static_cast<int64_t>(beatInterval);
        int64_t endBeat = static_cast<int64_t>(params.visibleEndSeconds / secondsPerBeat) + 1;
        if (endBeat - startBeat > 2000) endBeat = startBeat + 2000;

        for (int64_t beat = startBeat; beat <= endBeat; beat += static_cast<int64_t>(beatInterval)) {
            double time = beat * secondsPerBeat;
            int pixelX = static_cast<int>(std::llround((time - params.visibleStartSeconds) * pps));
            if (pixelX < -2 || pixelX > w + 2) continue;

            bool isMeasure = (beatInterval >= 4.0) ? true : ((beat % 4) == 0);

            if (themeId == ThemeId::Aurora) {
                if (params.viewKind == "arrangement")
                    g.setColour(UIColors::gridLine.withAlpha(isMeasure ? 0.060f : 0.022f));
                else
                    g.setColour(isMeasure
                        ? UIColors::pianoRollGrid.interpolatedWith(UIColors::textSecondary, 0.14f).withAlpha(0.064f)
                        : UIColors::pianoRollGrid.withAlpha(0.022f));
            } else if (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) {
                g.setColour(UIColors::pianoRollGrid.withAlpha(isMeasure ? 0.040f : 0.016f));
            } else if (themeId == ThemeId::DarkBlueGrey) {
                g.setColour(UIColors::panelBorder.withAlpha(0.12f));
            } else {
                g.setColour(isMeasure
                    ? UIColors::panelBorder.brighter(0.3f)
                    : UIColors::panelBorder.withAlpha(0.25f));
            }
            g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(h));
        }
    } else { // Seconds
        double markerInterval = selectMarkerInterval(pps);
        if (markerInterval < 0.001) markerInterval = 1.0;

        double startTime = params.visibleStartSeconds;
        if (startTime < 0.0) startTime = 0.0;
        startTime = std::floor(startTime / markerInterval) * markerInterval;
        double endTime = params.visibleEndSeconds;

        for (double time = startTime; time < endTime + markerInterval; time += markerInterval) {
            int pixelX = static_cast<int>(std::llround((time - params.visibleStartSeconds) * pps));
            if (pixelX < -2 || pixelX > w + 2) continue;

            if (themeId == ThemeId::Aurora) {
                if (params.viewKind == "arrangement")
                    g.setColour(UIColors::gridLine.withAlpha(0.022f));
                else
                    g.setColour(UIColors::pianoRollGrid.withAlpha(0.016f));
            } else if (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) {
                g.setColour(UIColors::pianoRollGrid.withAlpha(0.022f));
            } else if (themeId == ThemeId::DarkBlueGrey) {
                g.setColour(UIColors::panelBorder.withAlpha(0.12f));
            } else {
                g.setColour(UIColors::panelBorder.withAlpha(0.25f));
            }
            g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(h));
        }
    }
}

// ============================================================================
// drawTimeRuler — 在 tile 内绘制时间标尺
// ============================================================================
void TimelineLayerComposer::drawTimeRuler(juce::Graphics& g, const RenderParams& params) {
    const int rulerHeight = params.rulerHeight;
    const juce::Rectangle<int> rulerPaintBounds { 0, 0, params.viewportWidth, rulerHeight };

    const auto themeId = static_cast<ThemeId>(params.themeId);
    const auto rulerStyle = resolveRulerStyle(params.viewKind, themeId);
    const double pps = params.pixelsPerSecond;

    int rulerTop = 0;
    int rulerBottom = rulerHeight;

    // Bottom separator line — Arrangement 的 separator 由组件层绘制
    if (params.viewKind != "arrangement") {
        g.setColour(rulerStyle.separatorColour);
        g.drawLine(0.0f, static_cast<float>(rulerBottom),
                   static_cast<float>(rulerPaintBounds.getWidth()), static_cast<float>(rulerBottom),
                   rulerStyle.tickStroke);
    }

    if (params.timeUnit == 1) { // Bars
        double bpm = static_cast<double>(params.tempo);
        if (bpm <= 0.0) bpm = 120.0;
        double secondsPerBeat = 60.0 / bpm;
        double pixelsPerBeat = pps * secondsPerBeat;
        double beatInterval = selectBeatInterval(pixelsPerBeat);

        int64_t startBeat = static_cast<int64_t>(params.visibleStartSeconds / secondsPerBeat);
        if (startBeat < 0) startBeat = 0;
        startBeat = (startBeat / static_cast<int64_t>(beatInterval)) * static_cast<int64_t>(beatInterval);
        int64_t endBeat = static_cast<int64_t>(params.visibleEndSeconds / secondsPerBeat) + 1;

        g.setFont(UIColors::getUIFont(13.0f));

        for (int64_t beat = startBeat; beat <= endBeat; beat += static_cast<int64_t>(beatInterval)) {
            double time = beat * secondsPerBeat;
            int pixelX = static_cast<int>(std::llround((time - params.visibleStartSeconds) * pps));

            g.setColour(rulerStyle.tickColour);
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerBottom - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerBottom),
                       rulerStyle.tickStroke);

            int64_t bar = (beat / 4) + 1;
            int64_t beatInBar = (beat % 4) + 1;
            juce::String label = (beatInterval >= 4.0)
                ? juce::String(bar)
                : juce::String::formatted("%lld.%lld", static_cast<long long>(bar), static_cast<long long>(beatInBar));

            g.setColour(rulerStyle.labelColour);
            g.drawText(label, pixelX - 20, rulerTop + 2, 40, rulerHeight - 12, juce::Justification::centred);
        }
    } else { // Seconds
        double markerInterval = selectMarkerInterval(pps);
        double startTime = params.visibleStartSeconds;
        if (startTime < 0.0) startTime = 0.0;
        startTime = std::floor(startTime / markerInterval) * markerInterval;
        double endTime = params.visibleEndSeconds;

        g.setFont(UIColors::getUIFont(13.0f));
        for (double time = startTime; time < endTime; time += markerInterval) {
            int pixelX = static_cast<int>(std::llround((time - params.visibleStartSeconds) * pps));

            g.setColour(rulerStyle.tickColour);
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerBottom - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerBottom),
                       rulerStyle.tickStroke);

            const int totalSecs = static_cast<int>(time);
            const int mins = totalSecs / 60;
            const int secs = totalSecs % 60;
            juce::String timeStr = juce::String::formatted("%d:%02d", mins, secs);

            g.setColour(rulerStyle.labelColour);
            g.drawText(timeStr, pixelX - 20, rulerTop + 2, 40, rulerHeight - 12, juce::Justification::centred);
        }
    }
}

// ============================================================================
// drawLaneStripRepeats — 垂直 lane strip
// ============================================================================
void TimelineLayerComposer::drawLaneStripRepeats(juce::Graphics& g, const RenderParams& params) {
    const auto themeId = static_cast<ThemeId>(params.themeId);
    const float pixelsPerSemitone = params.pixelsPerSemitone;
    const int pianoKeyWidth = 0;
    const int worldTopY = params.worldTopY;
    const bool showLanes = decodeShowLanes(params.laneStyle);
    const int scaleRootNote = decodeScaleRootNote(params.laneStyle);
    const int scaleType = decodeScaleType(params.laneStyle);

    const int w = params.viewportWidth;
    const int h = params.viewportHeight;
    const bool isAurora = themeId == ThemeId::Aurora;
    static constexpr int kScaleTypeChromatic = 3;
    static constexpr float minMidi = 24.0f;
    static constexpr float maxMidi = 108.0f;

    const auto inScalePitchClass = buildInScalePitchClasses(scaleType, scaleRootNote);

    for (int midi = static_cast<int>(minMidi); midi <= static_cast<int>(maxMidi); ++midi) {
        float y = (maxMidi - static_cast<float>(midi)) * pixelsPerSemitone - worldTopY;
        float laneH = pixelsPerSemitone;

        if (y < -laneH || y > h) continue;

        int noteInOctave = midi % 12;
        bool isBlackKey = (noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 ||
                          noteInOctave == 8 || noteInOctave == 10);

        // Lane fill (only when showLanes is on)
        if (showLanes) {
            if (isAurora) {
                g.setColour(isBlackKey
                    ? UIColors::glassSurface.withAlpha(0.075f)
                    : UIColors::pianoRollLane.withAlpha(0.024f));
                g.fillRect(static_cast<float>(pianoKeyWidth), y,
                           static_cast<float>(w - pianoKeyWidth), laneH);
            } else if (isBlackKey) {
                g.setColour((themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose)
                    ? UIColors::pianoRollLane.withAlpha(0.16f)
                    : UIColors::backgroundDark.withAlpha(0.3f));
                g.fillRect(static_cast<float>(pianoKeyWidth), y,
                           static_cast<float>(w - pianoKeyWidth), laneH);
            }

            // Scale-aware lane highlighting
            if (scaleType != kScaleTypeChromatic) {
                const int pitchClass = ((midi % 12) + 12) % 12;
                if (inScalePitchClass[static_cast<std::size_t>(pitchClass)]) {
                    const float scaleAlpha = isAurora ? 0.060f
                        : ((themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) ? 0.14f : 0.65f);
                    g.setColour(UIColors::scaleHighlight.withMultipliedAlpha(scaleAlpha));
                    g.fillRect(static_cast<float>(pianoKeyWidth), y,
                               static_cast<float>(w - pianoKeyWidth), laneH);
                }
            }
        }

        // Row separator line (always drawn)
        const auto rowLineColour = isAurora
            ? UIColors::pianoRollGrid.withAlpha(0.022f)
            : ((themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose)
                ? UIColors::pianoRollGrid.withAlpha(0.030f) : UIColors::panelBorder.withAlpha(0.15f));
        g.setColour(rowLineColour);
        g.drawLine(static_cast<float>(pianoKeyWidth), y, static_cast<float>(w), y,
                   (isAurora || themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) ? 0.55f : 1.0f);
    }
}

} // namespace OpenTune
