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
    if (pixelsPerSecond < 60.0) return 5.0;
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
    } else if (themeId == ThemeId::BlueBreeze) {
        style.labelColour = UIColors::textSecondary.withAlpha(0.58f);
        style.tickColour = UIColors::pianoRollGrid.withAlpha(0.052f);
        style.separatorColour = UIColors::pianoRollGrid.withAlpha(0.040f);
        style.tickStroke = 0.7f;
    } else if (themeId == ThemeId::Overdose) {
        style.labelColour = UIColors::textSecondary.withAlpha(0.58f);
        style.tickColour = UIColors::pianoRollGrid.withAlpha(0.10f);
        style.separatorColour = UIColors::pianoRollGrid.withAlpha(0.08f);
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
// resolveGridLineColour — 业务语义统一：同一主题同一 grid token，同一 minor alpha
// Bars 仅在小节线使用 major alpha；Seconds 永远使用 minor alpha
// ============================================================================
static juce::Colour resolveGridLineColour(ThemeId themeId, bool isMeasure) {
    if (themeId == ThemeId::Aurora)
        return UIColors::pianoRollGrid.withAlpha(isMeasure ? 0.064f : 0.022f);
    if (themeId == ThemeId::BlueBreeze)
        return UIColors::pianoRollGrid.withAlpha(isMeasure ? 0.040f : 0.016f);
    if (themeId == ThemeId::Overdose)
        return UIColors::pianoRollGrid.withAlpha(isMeasure ? 0.10f : 0.05f);
    if (themeId == ThemeId::DarkBlueGrey)
        return UIColors::panelBorder.withAlpha(0.12f);
    return UIColors::panelBorder.withAlpha(isMeasure ? 0.35f : 0.25f);
}

// ============================================================================
// drawGridLines — 在 tile 内按 absolute time 绘制网格线
// ============================================================================
void TimelineLayerComposer::drawGridLines(juce::Graphics& g, const RenderParams& params) {
    const int w = params.viewportWidth;
    const int h = params.viewportHeight;
    const auto themeId = static_cast<ThemeId>(params.themeId);
    const double pps = params.pixelsPerSecond;

    if (params.displayMode == TimelineDisplayMode::Bars) {
        const double quarterSeconds = 60.0 / params.tempo;
        const double beatSeconds = quarterSeconds * 4.0 / static_cast<double>(params.timeSigDenominator);
        const double pixelsPerBeat = pps * beatSeconds;
        const double rawBeatInterval = selectBeatInterval(pixelsPerBeat);
        const int num = params.timeSigNumerator;

        // 稀疏刻度保持小节对齐：step 必须是 num 的整数倍
        int64_t beatInterval = static_cast<int64_t>(rawBeatInterval);
        if (beatInterval > 1)
            beatInterval = ((beatInterval + num - 1) / num) * num;

        // ── performance: narrow beat range to clip bounds ──
        const auto clip = g.getClipBounds();
        constexpr int kGridClipPadX = 2;
        const double gridClipStartTime = params.visibleStartSeconds +
            static_cast<double>(std::max(0, clip.getX() - kGridClipPadX)) / pps;
        const double gridClipEndTime = params.visibleStartSeconds +
            static_cast<double>(std::min(w, clip.getRight() + kGridClipPadX)) / pps;

        int64_t startBeat = static_cast<int64_t>(gridClipStartTime / beatSeconds);
        if (startBeat < 0) startBeat = 0;
        startBeat = (startBeat / beatInterval) * beatInterval;
        int64_t endBeat = static_cast<int64_t>(std::min(gridClipEndTime, params.visibleEndSeconds) / beatSeconds) + 1;
        if (endBeat - startBeat > 2000) endBeat = startBeat + 2000;

        for (int64_t beatIndex = startBeat; beatIndex <= endBeat; beatIndex += beatInterval) {
            double time = static_cast<double>(beatIndex) * beatSeconds;
            int pixelX = static_cast<int>(std::llround((time - params.visibleStartSeconds) * pps));
            if (pixelX < -2 || pixelX > w + 2) continue;

            const bool isMeasure = (beatIndex % num) == 0;
            g.setColour(resolveGridLineColour(themeId, isMeasure));
            g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(h));
        }
    } else { // Seconds — 永远 minor alpha
        double markerInterval = selectMarkerInterval(pps);
        if (markerInterval < 0.001) markerInterval = 1.0;

        const auto clip = g.getClipBounds();
        constexpr int kGridClipPadX = 2;
        const double gridClipStartTime = params.visibleStartSeconds +
            static_cast<double>(std::max(0, clip.getX() - kGridClipPadX)) / pps;
        const double gridClipEndTime = params.visibleStartSeconds +
            static_cast<double>(std::min(w, clip.getRight() + kGridClipPadX)) / pps;

        double startTime = gridClipStartTime;
        if (startTime < 0.0) startTime = 0.0;
        startTime = std::floor(startTime / markerInterval) * markerInterval;
        double endTime = std::min(gridClipEndTime, params.visibleEndSeconds);

        const auto minorColour = resolveGridLineColour(themeId, false);
        for (double time = startTime; time < endTime + markerInterval; time += markerInterval) {
            int pixelX = static_cast<int>(std::llround((time - params.visibleStartSeconds) * pps));
            if (pixelX < -2 || pixelX > w + 2) continue;

            g.setColour(minorColour);
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

    if (params.displayMode == TimelineDisplayMode::Bars) {
        const double quarterSeconds = 60.0 / params.tempo;
        const double beatSeconds = quarterSeconds * 4.0 / static_cast<double>(params.timeSigDenominator);
        const double pixelsPerBeat = pps * beatSeconds;
        const double rawBeatInterval = selectBeatInterval(pixelsPerBeat);
        const int num = params.timeSigNumerator;

        // 稀疏刻度保持小节对齐：step 必须是 num 的整数倍
        int64_t beatInterval = static_cast<int64_t>(rawBeatInterval);
        if (beatInterval > 1)
            beatInterval = ((beatInterval + num - 1) / num) * num;

        // ── performance: narrow beat range to clip bounds ──
        const auto clip = g.getClipBounds();
        const double rulerClipStartTime = params.visibleStartSeconds +
            static_cast<double>(std::max(0, clip.getX() - kRulerLabelPaintOverflowX)) / pps;
        const double rulerClipEndTime = params.visibleStartSeconds +
            static_cast<double>(std::min(params.viewportWidth, clip.getRight() + kRulerLabelPaintOverflowX)) / pps;

        int64_t startBeat = static_cast<int64_t>(rulerClipStartTime / beatSeconds);
        if (startBeat < 0) startBeat = 0;
        startBeat = (startBeat / beatInterval) * beatInterval;
        int64_t endBeat = static_cast<int64_t>(std::min(rulerClipEndTime, params.visibleEndSeconds) / beatSeconds) + 1;

        g.setFont(UIColors::getUIFont(13.0f));

        for (int64_t beatIndex = startBeat; beatIndex <= endBeat; beatIndex += beatInterval) {
            double time = static_cast<double>(beatIndex) * beatSeconds;
            int pixelX = static_cast<int>(std::llround((time - params.visibleStartSeconds) * pps));

            g.setColour(rulerStyle.tickColour);
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerBottom - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerBottom),
                       rulerStyle.tickStroke);

            const int64_t bar = beatIndex / num + 1;
            const int64_t beatInBar = beatIndex % num + 1;
            juce::String label = (beatInterval >= static_cast<int64_t>(num))
                ? juce::String(bar)
                : juce::String::formatted("%lld.%lld", static_cast<long long>(bar), static_cast<long long>(beatInBar));

            g.setColour(rulerStyle.labelColour);
            juce::Rectangle<int> labelRect { pixelX - 20, rulerTop + 2, 40, rulerHeight - 12 };
            if (labelRect.getX() < 0)
                labelRect.setX(0); // 左边界标签左对齐，避免被视图左边界裁切
            g.drawText(label, labelRect, juce::Justification::centred);
        }
    } else { // Seconds
        double markerInterval = selectMarkerInterval(pps);

        // ── performance: narrow time range to clip bounds ──
        const auto clip = g.getClipBounds();
        const double rulerClipStartTime = params.visibleStartSeconds +
            static_cast<double>(std::max(0, clip.getX() - kRulerLabelPaintOverflowX)) / pps;
        const double rulerClipEndTime = params.visibleStartSeconds +
            static_cast<double>(std::min(params.viewportWidth, clip.getRight() + kRulerLabelPaintOverflowX)) / pps;

        double startTime = rulerClipStartTime;
        if (startTime < 0.0) startTime = 0.0;
        startTime = std::floor(startTime / markerInterval) * markerInterval;
        double endTime = std::min(rulerClipEndTime, params.visibleEndSeconds);

        g.setFont(UIColors::getUIFont(13.0f));
        for (double time = startTime; time < endTime; time += markerInterval) {
            int pixelX = static_cast<int>(std::llround((time - params.visibleStartSeconds) * pps));

            g.setColour(rulerStyle.tickColour);
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerBottom - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerBottom),
                       rulerStyle.tickStroke);

            const juce::String timeStr = formatSecondsRulerLabel(static_cast<int>(time));

            g.setColour(rulerStyle.labelColour);
            juce::Rectangle<int> labelRect { pixelX - 20, rulerTop + 2, 40, rulerHeight - 12 };
            if (labelRect.getX() < 0)
                labelRect.setX(0); // 左边界标签左对齐，避免被视图左边界裁切
            g.drawText(timeStr, labelRect, juce::Justification::centred);
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
    const float worldTopY = params.worldTopY;
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

// ============================================================================
// formatSecondsRulerLabel
// ============================================================================
juce::String TimelineLayerComposer::formatSecondsRulerLabel(int totalSeconds)
{
    if (totalSeconds == 0)
        return "0";
    const int mins = totalSeconds / 60;
    const int secs = totalSeconds % 60;
    return juce::String::formatted("%02d:%02d", mins, secs);
}

// ============================================================================
// makeRulerScrollDamage — 双向最小带合同
// delta>0: entering=exposed strip 左扩21; exiting空
// delta<0: entering=exposed strip 右扩21; exiting=timeline右21px清幽灵像素
// entering与exiting重叠或相邻时合并为一个entering，exiting为空
// ============================================================================
TimelineLayerComposer::RulerScrollDamage TimelineLayerComposer::makeRulerScrollDamage(
    juce::Rectangle<int> exposedStrip,
    juce::Rectangle<int> timelineBounds,
    int scrollDeltaPixels)
{
    RulerScrollDamage d;

    if (scrollDeltaPixels > 0) {
        const int left = std::max(timelineBounds.getX(),
                                  exposedStrip.getX() - kRulerLabelPaintOverflowX);
        d.entering = juce::Rectangle<int>(left, exposedStrip.getY(),
                                          exposedStrip.getRight() - left, exposedStrip.getHeight());
    } else if (scrollDeltaPixels < 0) {
        const int right = std::min(timelineBounds.getRight(),
                                   exposedStrip.getRight() + kRulerLabelPaintOverflowX);
        d.entering = juce::Rectangle<int>(exposedStrip.getX(), exposedStrip.getY(),
                                          right - exposedStrip.getX(), exposedStrip.getHeight());
        const int exitLeft = std::max(timelineBounds.getX(),
                                      timelineBounds.getRight() - kRulerLabelPaintOverflowX);
        d.exiting = juce::Rectangle<int>(exitLeft, exposedStrip.getY(),
                                         timelineBounds.getRight() - exitLeft, exposedStrip.getHeight());
    }

    if (!d.entering.isEmpty() && !d.exiting.isEmpty()) {
        if (d.entering.getRight() >= d.exiting.getX()) {
            const int mergedLeft = std::min(d.entering.getX(), d.exiting.getX());
            const int mergedRight = std::max(d.entering.getRight(), d.exiting.getRight());
            d.entering = juce::Rectangle<int>(mergedLeft, d.entering.getY(),
                                              mergedRight - mergedLeft, d.entering.getHeight());
            d.exiting = {};
        }
    }

    return d;
}

} // namespace OpenTune
