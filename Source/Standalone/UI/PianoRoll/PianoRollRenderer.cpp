#include "PianoRollRenderer.h"
#include "../UIColors.h"
#include "../../../Utils/AppLogger.h"
#include "../../../Utils/LegacyNoteGenerator.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace OpenTune {

static double selectBeatInterval(double pixelsPerBeat) {
    if (pixelsPerBeat < 2.5) return 32.0;
    if (pixelsPerBeat < 5.0) return 16.0;
    if (pixelsPerBeat < 10.0) return 8.0;
    if (pixelsPerBeat < 40.0) return 4.0;
    return 1.0;
}

static double selectMarkerInterval(double pixelsPerSecond) {
    if (pixelsPerSecond < 1.33) return 60.0;
    if (pixelsPerSecond < 4.0) return 30.0;
    if (pixelsPerSecond < 8.0) return 10.0;
    if (pixelsPerSecond < 40.0) return 5.0;
    return 1.0;
}

// ============================================================================
// Shared scale computation helper
// ============================================================================
static std::array<bool, 12> buildInScalePitchClasses(int scaleType, int rootNote) noexcept
{
    std::array<bool, 12> result{};
    static constexpr int kScaleTypeChromatic = 3;

    if (scaleType == kScaleTypeChromatic) {
        result.fill(true);
        return result;
    }

    result.fill(false);
    const int rootPc = juce::jlimit(0, 11, rootNote);

    // Map UI scaleType (1-8) to ScaleMode enum
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

namespace {

struct VisibleTimeWindow {
    int viewportStartX = 0;
    int viewportEndX = 0;
    double visibleStartTime = 0.0;
    double visibleEndTime = 0.0;
    double visibleMaterializationStartTime = 0.0;
    double visibleMaterializationEndTime = 0.0;

    bool isValid() const
    {
        return viewportEndX > viewportStartX
            && visibleEndTime > visibleStartTime
            && visibleMaterializationEndTime > visibleMaterializationStartTime;
    }
};

VisibleTimeWindow computeVisibleTimeWindow(const PianoRollRenderer::RenderContext& ctx,
                                           const PianoRollRenderer::MaterializationRenderItem& item)
{
    VisibleTimeWindow window;
    if (!item.projection.isValid()) {
        return window;
    }

    window.viewportStartX = ctx.pianoKeyWidth;
    window.viewportEndX = ctx.width;
    if (window.viewportEndX <= window.viewportStartX)
        return {};

    window.visibleStartTime = ctx.xToTime(window.viewportStartX);
    window.visibleEndTime = ctx.xToTime(window.viewportEndX);
    if (window.visibleEndTime <= window.visibleStartTime)
        return {};

    window.visibleMaterializationStartTime = item.projection.projectTimelineTimeToMaterialization(window.visibleStartTime);
    window.visibleMaterializationEndTime = item.projection.projectTimelineTimeToMaterialization(window.visibleEndTime);

    // vocal-time-stretch §8.5 — When a non-identity TimeGrid is published,
    // projectTimelineTimeToMaterialization returns OUTPUT time inside the
    // materialization, but Notes / PitchCurve / F0 timeline / WaveformMipmap
    // are all indexed by SOURCE time. Convert to source time via tauInverse
    // so downstream filters work correctly. Identity grid → no-op.
    if (ctx.timeGridSnapshot != nullptr && !ctx.timeGridSnapshot->isIdentity()) {
        window.visibleMaterializationStartTime = ctx.timeGridSnapshot->tauInverse(window.visibleMaterializationStartTime);
        window.visibleMaterializationEndTime   = ctx.timeGridSnapshot->tauInverse(window.visibleMaterializationEndTime);
    }
    return window;
}

// vocal-time-stretch §8.5 — convert a SOURCE-time anchor (Note.startTime,
// f0Timeline frame timestamp, WaveformMipmap peak) into screen X via the
// item's projection. Identity TimeGrid → degenerates to existing pipeline.
inline int sourceTimeToScreenX(double sourceTime,
                                const PianoRollRenderer::RenderContext& ctx,
                                const PianoRollRenderer::MaterializationRenderItem& item)
{
    double outputTime = sourceTime;
    if (ctx.timeGridSnapshot != nullptr && !ctx.timeGridSnapshot->isIdentity()) {
        outputTime = ctx.timeGridSnapshot->tauForward(sourceTime);
    }
    const double timelineTime = item.projection.projectMaterializationTimeToTimeline(outputTime);
    return ctx.timeToX(timelineTime);
}

bool isVoicedFrame(float frequencyHz) noexcept
{
    return frequencyHz > 0.0f;
}

} // namespace

void PianoRollRenderer::drawLanes(juce::Graphics& g, const RenderContext& ctx)
{
    const int w = ctx.width;
    const int h = ctx.height;
    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
    static constexpr int kScaleTypeChromatic = 3;
    const auto inScalePitchClass = buildInScalePitchClasses(ctx.scaleType, ctx.scaleRootNote);

    for (int midi = static_cast<int>(ctx.minMidi); midi <= static_cast<int>(ctx.maxMidi); ++midi)
    {
        float y = ctx.midiToY(static_cast<float>(midi));
        float laneH = ctx.pixelsPerSemitone;

        if (y < -laneH || y > h) continue;

        int noteInOctave = midi % 12;
        bool isBlackKey = (noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 ||
                          noteInOctave == 8 || noteInOctave == 10);

        if (ctx.showLanes)
        {
            if (isAurora)
            {
                const auto laneColour = isBlackKey
                    ? UIColors::glassSurface.withAlpha(0.075f)
                    : UIColors::pianoRollLane.withAlpha(0.024f);
                g.setColour(laneColour);
                g.fillRect(static_cast<float>(ctx.pianoKeyWidth), y, static_cast<float>(w - ctx.pianoKeyWidth), laneH);
            }
            else if (isBlackKey)
            {
                g.setColour(isBlueBreeze
                    ? UIColors::pianoRollLane.withAlpha(0.16f)
                    : UIColors::backgroundDark.withAlpha(0.3f));
                g.fillRect(static_cast<float>(ctx.pianoKeyWidth), y, static_cast<float>(w - ctx.pianoKeyWidth), laneH);
            }

            // Scale-aware lane highlighting: tint in-scale lanes with scaleHighlight color
            if (ctx.scaleType != kScaleTypeChromatic)
            {
                const int pitchClass = ((midi % 12) + 12) % 12;
                if (inScalePitchClass[static_cast<std::size_t>(pitchClass)])
                {
                    const float scaleAlpha = isAurora ? 0.060f : (isBlueBreeze ? 0.14f : 0.65f);
                    g.setColour(UIColors::scaleHighlight.withMultipliedAlpha(scaleAlpha));
                    g.fillRect(static_cast<float>(ctx.pianoKeyWidth), y, static_cast<float>(w - ctx.pianoKeyWidth), laneH);
                }
            }
        }

        const auto rowLineColour = isAurora
            ? UIColors::pianoRollGrid.withAlpha(0.022f)
            : (isBlueBreeze ? UIColors::pianoRollGrid.withAlpha(0.030f) : UIColors::panelBorder.withAlpha(0.15f));
        g.setColour(rowLineColour);
        g.drawLine(static_cast<float>(ctx.pianoKeyWidth), y, static_cast<float>(w), y, (isAurora || isBlueBreeze) ? 0.55f : 1.0f);
    }
}

void PianoRollRenderer::drawUnvoicedFrameBands(juce::Graphics& g,
                                               const RenderContext& ctx,
                                               const MaterializationRenderItem& item)
{
    if (!ctx.showUnvoicedFrames || item.pitchSnapshot == nullptr || item.f0Timeline.isEmpty()) {
        return;
    }

    const auto& originalF0 = item.pitchSnapshot->getOriginalF0();
    if (originalF0.empty()) {
        return;
    }

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid()) {
        return;
    }

    const auto visibleFrames = item.f0Timeline.rangeForTimesWithMargin(visibleWindow.visibleMaterializationStartTime,
                                                                       visibleWindow.visibleMaterializationEndTime,
                                                                       1);
    const int visibleStartFrame = visibleFrames.startFrame;
    const int visibleEndFrame = visibleFrames.endFrameExclusive;
    if (visibleEndFrame <= visibleStartFrame) {
        return;
    }

    const auto bandColour = UIColors::currentThemeId() == ThemeId::DarkBlueGrey
        ? UIColors::backgroundDark.withAlpha(0.28f)
        : UIColors::backgroundMedium.withAlpha(0.22f);
    g.setColour(bandColour);

    auto drawBand = [&](int startFrame, int endFrameExclusive) {
        if (endFrameExclusive <= startFrame) {
            return;
        }

        // §8.5 — frame timestamps are SOURCE time; project through τ so
        // unvoiced bands align with the stretched waveform.
        const int x1 = std::max(ctx.pianoKeyWidth,
            sourceTimeToScreenX(item.f0Timeline.timeAtFrame(startFrame), ctx, item));
        const int x2 = std::min(ctx.width,
            sourceTimeToScreenX(item.f0Timeline.timeAtFrame(endFrameExclusive), ctx, item));
        if (x2 <= x1) {
            return;
        }

        g.fillRect(static_cast<float>(x1), 0.0f, static_cast<float>(x2 - x1), static_cast<float>(ctx.height));
    };

    int currentBandStart = -1;
    for (int frame = visibleStartFrame; frame < visibleEndFrame; ++frame) {
        const bool unvoiced = !isVoicedFrame(originalF0[static_cast<std::size_t>(frame)]);
        if (unvoiced) {
            if (currentBandStart < 0) {
                currentBandStart = frame;
            }
            continue;
        }

        if (currentBandStart >= 0) {
            drawBand(currentBandStart, frame);
            currentBandStart = -1;
        }
    }

    if (currentBandStart >= 0) {
        drawBand(currentBandStart, visibleEndFrame);
    }
}

void PianoRollRenderer::drawWaveform(juce::Graphics& g,
                                     const RenderContext& ctx,
                                     const MaterializationRenderItem& item)
{
    if (item.audioBuffer == nullptr || item.waveformMipmap == nullptr)
        return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    const int startX = visibleWindow.viewportStartX;
    const int endX = visibleWindow.viewportEndX;
    const int w = endX - startX;
    if (w <= 0) return;

    const int levelIndex = item.waveformMipmap->selectBestLevelIndex(ctx.pixelsPerSecond);
    const auto& level = item.waveformMipmap->getLevel(levelIndex);
    
    if (level.peaks.empty())
        return;

    const double materializationVisibleDuration = visibleWindow.visibleMaterializationEndTime - visibleWindow.visibleMaterializationStartTime;
    if (materializationVisibleDuration <= 0.0) return;

    const float centerY = ctx.height / 2.0f;
    const float amplitudeScale = ctx.height / 2.0f;

    const int samplesPerPeak = WaveformMipmap::kSamplesPerPeak[levelIndex];
    const double timePerPeak = static_cast<double>(samplesPerPeak) / WaveformMipmap::kBaseSampleRate;
    const int64_t numPeaks = static_cast<int64_t>(level.peaks.size());
    const int64_t builtPeaks = level.complete ? numPeaks : level.buildProgress;
    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;

    juce::Path waveformPath;

    // ⚡️ vocal-time-stretch §8.5 (Phase H) — waveform stretching.
    // When the TimeGrid is non-identity, the visible waveform must reflect
    // the user's retiming.  We invert the output → source mapping (tau_inverse)
    // so the screen X axis (output time) reads from the SOURCE peaks at the
    // tau-inverted time.  Identity grid → zero-cost passthrough.
    const bool useTauInverse = (ctx.timeGridSnapshot != nullptr
                                  && !ctx.timeGridSnapshot->isIdentity());

    for (int x = startX; x < endX; ++x)
    {
        double matTime = item.projection.projectTimelineTimeToMaterialization(ctx.xToTime(x));
        if (useTauInverse) {
            // §8.5 — output materialization time → source time (TimeGrid stretching).
            matTime = ctx.timeGridSnapshot->tauInverse(matTime);
        }
        const int64_t peakIndex = static_cast<int64_t>(matTime / timePerPeak);
        
        if (peakIndex < 0 || peakIndex >= builtPeaks)
            continue;

        const auto& peak = level.peaks[static_cast<std::size_t>(peakIndex)];
        
        if (peak.isZero())
            continue;

        const float yMin = centerY - peak.getMax() * amplitudeScale;
        const float yMax = centerY - peak.getMin() * amplitudeScale;
        
        waveformPath.startNewSubPath(static_cast<float>(x), yMin);
        waveformPath.lineTo(static_cast<float>(x), yMax);
    }

    if (!waveformPath.isEmpty())
    {
        if (isAurora)
        {
            const auto waveformColour = UIColors::pianoRollWaveform.brighter(0.08f);
            g.setColour(waveformColour.withAlpha(0.24f));
            g.strokePath(waveformPath, juce::PathStrokeType(3.2f));
            g.setColour(waveformColour.withAlpha(0.52f));
            g.strokePath(waveformPath, juce::PathStrokeType(1.25f));
        }
        else if (isBlueBreeze)
        {
            const auto waveformColour = UIColors::pianoRollWaveform;
            g.setColour(waveformColour.withAlpha(0.13f));
            g.strokePath(waveformPath, juce::PathStrokeType(2.0f));
            g.setColour(waveformColour.withAlpha(0.24f));
            g.strokePath(waveformPath, juce::PathStrokeType(1.05f));
        }
        else
        {
            g.setColour(UIColors::waveformFill.withAlpha(0.20f));
            g.strokePath(waveformPath, juce::PathStrokeType(1.0f));
        }
    }
}

void PianoRollRenderer::drawTimeRuler(juce::Graphics& g, const RenderContext& ctx)
{
    constexpr int inset = 12;
    auto bounds = juce::Rectangle<int>(0, 0, ctx.width, ctx.height).reduced(inset);
    auto rulerArea = bounds.removeFromTop(ctx.rulerHeight);

    const int rulerTop = rulerArea.getY();
    const int rulerBottom = rulerArea.getBottom();
    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;

    if (isAurora)
    {
        UIColors::fillAuroraTimelineBackground(g, rulerArea.toFloat(), 0.0f);
    }
    else if (isBlueBreeze)
    {
        UIColors::fillMistedTimelineField(g, rulerArea.toFloat(), 0.0f);
    }
    else
    {
        g.setColour(UIColors::backgroundMedium);
        g.fillRect(rulerArea);
    }

    g.setColour(isAurora
        ? UIColors::gridLine.withAlpha(0.060f)
        : (isBlueBreeze ? UIColors::pianoRollGrid.withAlpha(0.040f) : UIColors::panelBorder));
    g.drawLine(0.0f, static_cast<float>(rulerBottom), static_cast<float>(ctx.width), static_cast<float>(rulerBottom), (isAurora || isBlueBreeze) ? 0.7f : 1.0f);

    if (ctx.timeUnit == RenderContext::TimeUnit::Bars)
    {
        double bpm = ctx.bpm;
        if (bpm <= 0.0) bpm = 120.0;

        double pixelsPerSecond = ctx.pixelsPerSecond;
        double secondsPerBeat = 60.0 / bpm;
        double pixelsPerBeat = pixelsPerSecond * secondsPerBeat;

        double beatInterval = selectBeatInterval(pixelsPerBeat);

        double startTime = ctx.xToTime(0);
        double endTime = ctx.xToTime(ctx.width);

        int64_t startBeat = static_cast<int64_t>(startTime / secondsPerBeat);
        if (startBeat < 0) startBeat = 0;
        startBeat = (startBeat / static_cast<int64_t>(beatInterval)) * static_cast<int64_t>(beatInterval);

        int64_t endBeat = static_cast<int64_t>(endTime / secondsPerBeat) + 1;

        g.setFont(UIColors::getUIFont(13.0f));

        for (int64_t beat = startBeat; beat <= endBeat; beat += static_cast<int64_t>(beatInterval))
        {
            double time = beat * secondsPerBeat;
            int pixelX = ctx.timeToX(time);

            g.setColour(isAurora
                ? UIColors::gridLine.withAlpha(0.080f)
                : (isBlueBreeze ? UIColors::pianoRollGrid.withAlpha(0.052f) : UIColors::gridLine));
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerBottom - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerBottom), (isAurora || isBlueBreeze) ? 0.7f : 1.0f);

            int64_t bar = (beat / 4) + 1;
            int64_t beatInBar = (beat % 4) + 1;

            juce::String label;
            if (beatInterval >= 4.0)
                label = juce::String(bar);
            else
                label = juce::String::formatted("%lld.%lld", (long long)bar, (long long)beatInBar);

        g.setColour(isAurora
            ? UIColors::textSecondary.withMultipliedAlpha(0.48f)
            : (isBlueBreeze ? UIColors::textSecondary.withAlpha(0.58f) : UIColors::textSecondary));
        g.drawText(label, pixelX - 20, rulerTop + 2, 40, ctx.rulerHeight - 12, juce::Justification::centred);
        }
    } else {
        double pixelsPerSecond = ctx.pixelsPerSecond;

        double markerInterval = selectMarkerInterval(pixelsPerSecond);

        double startTime = ctx.xToTime(0);
        if (startTime < 0.0) startTime = 0.0;
        startTime = std::floor(startTime / markerInterval) * markerInterval;

        double endTime = ctx.xToTime(ctx.width);

        g.setFont(UIColors::getUIFont(13.0f));
        for (double time = startTime; time < endTime; time += markerInterval)
        {
            int pixelX = ctx.timeToX(time);

            g.setColour(isAurora
                ? UIColors::gridLine.withAlpha(0.080f)
                : (isBlueBreeze ? UIColors::pianoRollGrid.withAlpha(0.052f) : UIColors::gridLine));
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerBottom - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerBottom), (isAurora || isBlueBreeze) ? 0.7f : 1.0f);

            // `time` here is absolute host/arrangement time; the component's
            // xToTime callback applies the current timeline view-domain origin.
            const int totalSecs = static_cast<int>(time);
            const int mins = totalSecs / 60;
            const int secs = totalSecs % 60;
            juce::String timeStr = juce::String::formatted("%d:%02d", mins, secs);

            g.setColour(isAurora
                ? UIColors::textSecondary.withMultipliedAlpha(0.48f)
                : (isBlueBreeze ? UIColors::textSecondary.withAlpha(0.58f) : UIColors::textSecondary));
            g.drawText(timeStr, pixelX - 20, rulerTop + 2, 40, ctx.rulerHeight - 12, juce::Justification::centred);
        }
    }
}

void PianoRollRenderer::drawGridLines(juce::Graphics& g, const RenderContext& ctx)
{
    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;

    if (ctx.timeUnit == RenderContext::TimeUnit::Bars)
    {
        double bpm = ctx.bpm;
        if (bpm <= 0.0) bpm = 120.0;

        double pixelsPerSecond = ctx.pixelsPerSecond;
        double secondsPerBeat = 60.0 / bpm;
        double pixelsPerBeat = pixelsPerSecond * secondsPerBeat;

        double beatInterval = selectBeatInterval(pixelsPerBeat);

        double startTime = ctx.xToTime(0);
        double endTime = ctx.xToTime(ctx.width);

        int64_t startBeat = static_cast<int64_t>(startTime / secondsPerBeat);
        if (startBeat < 0) startBeat = 0;
        startBeat = (startBeat / static_cast<int64_t>(beatInterval)) * static_cast<int64_t>(beatInterval);

        int64_t endBeat = static_cast<int64_t>(endTime / secondsPerBeat) + 1;

        if (endBeat - startBeat > 2000) endBeat = startBeat + 2000;

        for (int64_t beat = startBeat; beat <= endBeat; beat += static_cast<int64_t>(beatInterval))
        {
            double time = beat * secondsPerBeat;
            int pixelX = ctx.timeToX(time);

            if (pixelX < ctx.pianoKeyWidth - 2 || pixelX > ctx.width + 2) continue;

            bool isMeasure = false;
            if (beatInterval >= 4.0) {
                isMeasure = true;
            } else {
                isMeasure = (beat % 4) == 0;
            }

            if (themeId == ThemeId::Aurora)
            {
                const auto colour = isMeasure
                    ? UIColors::pianoRollGrid.interpolatedWith(UIColors::textSecondary, 0.14f).withAlpha(0.064f)
                    : UIColors::pianoRollGrid.withAlpha(0.022f);
                g.setColour(colour);
                g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(ctx.height));
            }
            else if (themeId == ThemeId::BlueBreeze)
            {
                g.setColour(UIColors::pianoRollGrid.withAlpha(isMeasure ? 0.040f : 0.016f));
                g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(ctx.height));
            }
            else if (themeId == ThemeId::DarkBlueGrey)
            {
                g.setColour(UIColors::panelBorder.withAlpha(0.12f));
                g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(ctx.height));
            } else {
                if (isMeasure) {
                    g.setColour(UIColors::panelBorder.brighter(0.3f));
                    g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(ctx.height));
                } else {
                    g.setColour(UIColors::panelBorder.withAlpha(0.25f));
                    g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(ctx.height));
                }
            }
        }
    } else {
        double pixelsPerSecond = ctx.pixelsPerSecond;

        double markerInterval = selectMarkerInterval(pixelsPerSecond);

        double startTime = ctx.xToTime(0);
        if (startTime < 0.0) startTime = 0.0;
        startTime = std::floor(startTime / markerInterval) * markerInterval;

        double endTime = ctx.xToTime(ctx.width);

        if (markerInterval < 0.001) markerInterval = 1.0;

        for (double time = startTime; time < endTime + markerInterval; time += markerInterval)
        {
            int pixelX = ctx.timeToX(time);

            if (pixelX < ctx.pianoKeyWidth - 2 || pixelX > ctx.width + 2) continue;
            if (themeId == ThemeId::Aurora)
                g.setColour(UIColors::pianoRollGrid.withAlpha(0.016f));
            else if (themeId == ThemeId::BlueBreeze)
                g.setColour(UIColors::pianoRollGrid.withAlpha(0.022f));
            else
                g.setColour(themeId == ThemeId::DarkBlueGrey ? UIColors::panelBorder.withAlpha(0.12f) : UIColors::panelBorder.withAlpha(0.25f));
            g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(ctx.height));
        }
    }
}

void PianoRollRenderer::drawChunkBoundaries(juce::Graphics& g,
                                            const RenderContext& ctx,
                                            const MaterializationRenderItem& item)
{
    if (!ctx.showChunkBoundaries || item.chunkBoundaries.size() < 3) {
        return;
    }

    static constexpr float dashLengths[] { 4.0f, 4.0f };
    g.setColour(UIColors::accent.withAlpha(0.75f));

    for (std::size_t index = 1; index + 1 < item.chunkBoundaries.size(); ++index) {
        // §8.5 — chunk boundaries are SOURCE time; project through τ.
        const int x = sourceTimeToScreenX(item.chunkBoundaries[index], ctx, item);
        if (x < ctx.pianoKeyWidth || x >= ctx.width) {
            continue;
        }

        g.drawDashedLine(juce::Line<float>(static_cast<float>(x),
                                          0.0f,
                                          static_cast<float>(x),
                                          static_cast<float>(ctx.height)),
                         dashLengths,
                         2,
                         1.0f);
    }
}

void PianoRollRenderer::drawPianoKeys(juce::Graphics& g, const RenderContext& ctx)
{
    const int height = ctx.height;
    const int w = ctx.pianoKeyWidth;
    const float blackKeyWidthRatio = 0.6f;
    const float blackKeyW = w * blackKeyWidthRatio;
    static constexpr int kScaleTypeChromatic = 3;
    static constexpr float kOutOfScaleDimAmount = 0.30f;

    const bool isBlueBreeze = UIColors::currentThemeId() == ThemeId::BlueBreeze;
    const float outOfScaleDimAmount = isBlueBreeze ? 0.06f : kOutOfScaleDimAmount;

    // Note name lookup tables
    static const char* kSharpNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const char* kFlatNames[12]  = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
    static constexpr bool kUseFlatsByRoot[12] = {false,true,false,true,false,false,true,false,true,false,true,false};
    static constexpr float kShowAllLabelsMinPPS = 14.0f;
    static constexpr float kShowCOnlyMinPPS = 8.0f;

    // Build scale pitch-class membership using shared helper (supports all 8 scale types)
    const auto inScalePitchClass = buildInScalePitchClasses(ctx.scaleType, ctx.scaleRootNote);

    // Compute effective note name display mode (zoom-adaptive downgrade)
    int effectiveNoteNameMode = static_cast<int>(ctx.noteNameMode); // 0=ShowAll, 1=COnly, 2=Hide
    if (effectiveNoteNameMode == 0 && ctx.pixelsPerSemitone < kShowAllLabelsMinPPS)
        effectiveNoteNameMode = 1; // downgrade to C-only
    if (effectiveNoteNameMode <= 1 && ctx.pixelsPerSemitone < kShowCOnlyMinPPS)
        effectiveNoteNameMode = 2; // downgrade to hidden

    // Accidental preference: sharp or flat based on root note
    const bool useFlats = (ctx.scaleType != kScaleTypeChromatic) ? kUseFlatsByRoot[juce::jlimit(0, 11, ctx.scaleRootNote)] : false;

    const auto isMidiInCurrentScale = [&inScalePitchClass](int midiNote) noexcept {
        const int pitchClass = ((midiNote % 12) + 12) % 12;
        return inScalePitchClass[static_cast<std::size_t>(pitchClass)];
    };

    g.setColour(isBlueBreeze ? UIColors::keyBedWhite : UIColors::backgroundDark);
    g.fillRect(0, 0, w, height);

    juce::Colour cWhite1 = isBlueBreeze ? UIColors::keyBedWhite : juce::Colour(0xFFF7F9F9);
    juce::Colour cWhite2 = isBlueBreeze ? juce::Colour { BlueBreeze::Colors::KeyBedBottom } : juce::Colour(0xFFECF0F1);

    for (int midi = static_cast<int>(ctx.minMidi); midi <= static_cast<int>(ctx.maxMidi); ++midi)
    {
        int drawMidi = midi;
        float y = ctx.midiToY(static_cast<float>(drawMidi));
        float h = ctx.pixelsPerSemitone;

        if (y < -50.0f || y > height + 50.0f) continue;

        int noteInOctave = drawMidi % 12;
        bool isBlackKey = (noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 ||
                          noteInOctave == 8 || noteInOctave == 10);

        float drawH = h + 1.0f;

        if (!isBlackKey)
        {
            const bool inScale = isMidiInCurrentScale(drawMidi);

            juce::Rectangle<float> keyRect(0.0f, y, static_cast<float>(w), drawH);

            const juce::Colour whiteA = inScale ? cWhite1 : cWhite1.darker(outOfScaleDimAmount);
            const juce::Colour whiteB = inScale ? cWhite2 : cWhite2.darker(outOfScaleDimAmount);

            juce::ColourGradient grad(whiteA,
                                      static_cast<float>(0),
                                      y,
                                      whiteB.interpolatedWith(UIColors::keyBedDivider, isBlueBreeze ? 0.08f : 0.0f),
                                      static_cast<float>(0 + w),
                                      y + drawH,
                                      false);
            if (isBlueBreeze)
                grad.addColour(0.16, whiteA.interpolatedWith(juce::Colours::white, 0.12f));
            g.setGradientFill(grad);
            g.fillRect(keyRect);

            // Scale highlight overlay on in-scale white keys
            if (inScale && ctx.scaleType != kScaleTypeChromatic)
            {
                g.setColour(isBlueBreeze ? UIColors::scaleHighlight.withMultipliedAlpha(0.18f) : UIColors::scaleHighlight);
                g.fillRect(keyRect);
            }

            // Pressed key highlight
            if (drawMidi == ctx.pressedPianoKey)
            {
                g.setColour(isBlueBreeze ? juce::Colour { BlueBreeze::Colors::KeyPressedGlow }.withAlpha(0.22f)
                                         : juce::Colour(0x500078D7));
                g.fillRect(keyRect);
            }

            if (isBlueBreeze)
            {
                g.setColour(juce::Colours::white.withAlpha(0.24f));
                g.drawLine(keyRect.getX() + 2.0f, keyRect.getY() + 1.0f,
                           keyRect.getRight() - 1.0f, keyRect.getY() + 1.0f, 1.0f);
                g.setColour(UIColors::keyBedDivider.withAlpha(0.24f));
                g.drawLine(keyRect.getRight() - 1.0f, keyRect.getY(),
                           keyRect.getRight() - 1.0f, keyRect.getBottom(), 1.0f);
            }

            // Note name labels (with outline for readability)
            if (effectiveNoteNameMode == 0 || (effectiveNoteNameMode == 1 && noteInOctave == 0))
            {
                const float fontSize = juce::jmax(8.0f, juce::jmin(h * 0.7f, 14.0f));
                g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultSansSerifFontName(), "Bold", fontSize)));
                int octave = (drawMidi / 12) - 1;
                const char* name = useFlats ? kFlatNames[noteInOctave] : kSharpNames[noteInOctave];
                juce::String noteName = juce::String(name) + juce::String(octave);

                const int tx = 0;
                const int ty = static_cast<int>(y);
                const int tw = w - 4;
                const int th = static_cast<int>(h);

                // White key: dark outline + light text
                g.setColour(juce::Colours::black.withAlpha(0.5f));
                for (int ox = -1; ox <= 1; ++ox)
                    for (int oy = -1; oy <= 1; ++oy)
                        if (ox != 0 || oy != 0)
                            g.drawText(noteName, tx + ox, ty + oy, tw, th, juce::Justification::centredRight);

                const juce::Colour noteLabelColour = isBlueBreeze
                    ? juce::Colour(0xFF25303A).withMultipliedAlpha(inScale ? 0.90f : 0.62f)
                    : juce::Colour(0xFFE0E0E0).withMultipliedAlpha(inScale ? 1.0f : 0.78f);
                g.setColour(noteLabelColour);
                g.drawText(noteName, tx, ty, tw, th, juce::Justification::centredRight);
            }
        } else {
            const bool inScale = isMidiInCurrentScale(drawMidi);

            juce::Rectangle<float> extensionRect(blackKeyW, y, static_cast<float>(w) - blackKeyW, drawH);

            const juce::Colour extensionA = inScale ? cWhite1 : cWhite1.darker(outOfScaleDimAmount);
            const juce::Colour extensionB = inScale ? cWhite2 : cWhite2.darker(outOfScaleDimAmount);
            juce::ColourGradient grad(extensionA,
                                      static_cast<float>(0) + blackKeyW,
                                      y,
                                      extensionB.interpolatedWith(UIColors::keyBedDivider, isBlueBreeze ? 0.08f : 0.0f),
                                      static_cast<float>(0 + w),
                                      y + drawH,
                                      false);
            g.setGradientFill(grad);
            g.fillRect(extensionRect);

            // Scale highlight overlay on in-scale black key extension area
            if (inScale && ctx.scaleType != kScaleTypeChromatic)
            {
                g.setColour(isBlueBreeze ? UIColors::scaleHighlight.withMultipliedAlpha(0.18f) : UIColors::scaleHighlight);
                g.fillRect(extensionRect);
            }

            g.setColour(UIColors::panelBorder.withAlpha(isBlueBreeze ? 0.22f : 1.0f));
            g.drawLine(blackKeyW, y + h * 0.5f, static_cast<float>(w), y + h * 0.5f, 1.0f);
        }
    }

    for (int midi = static_cast<int>(ctx.minMidi); midi <= static_cast<int>(ctx.maxMidi); ++midi)
    {
        int drawMidi = midi;
        float y = ctx.midiToY(static_cast<float>(drawMidi));
        float h = ctx.pixelsPerSemitone;
        if (y < -50.0f || y > height + 50.0f) continue;

        int noteInOctave = drawMidi % 12;
        if (noteInOctave == 5 || noteInOctave == 0)
        {
            g.setColour(isBlueBreeze ? UIColors::keyBedDivider.withAlpha(0.36f) : UIColors::panelBorder);
            g.drawLine(0.0f, y + h, static_cast<float>(w), y + h, 1.0f);
        }
    }

    for (int midi = static_cast<int>(ctx.minMidi); midi <= static_cast<int>(ctx.maxMidi); ++midi)
    {
        int drawMidi = midi;
        float y = ctx.midiToY(static_cast<float>(drawMidi));
        float h = ctx.pixelsPerSemitone;

        if (y < -50.0f || y > height + 50.0f) continue;

        int noteInOctave = drawMidi % 12;
        bool isBlackKey = (noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 ||
                          noteInOctave == 8 || noteInOctave == 10);

        if (isBlackKey)
        {
            const bool inScale = isMidiInCurrentScale(drawMidi);

            float keyH = h * 0.8f;
            float keyY = y + (h - keyH) * 0.5f;

            juce::Rectangle<float> keyRect(0.0f, keyY, blackKeyW, keyH);

            juce::DropShadow ds;
            ds.colour = juce::Colours::black.withAlpha(isBlueBreeze ? 0.28f : 0.25f);
            ds.radius = isBlueBreeze ? 6 : 5;
            ds.offset = {0, 1};

            juce::Path shadowPath;
            shadowPath.addRoundedRectangle(keyRect, 2.0f);
            ds.drawForPath(g, shadowPath);

            juce::ColourGradient sideShadow(juce::Colours::black.withAlpha(isBlueBreeze ? 0.12f : 0.2f), blackKeyW, keyY,
                                            juce::Colours::transparentBlack, blackKeyW + 1.25f, keyY, false);
            g.setGradientFill(sideShadow);
            g.fillRect(static_cast<int>(blackKeyW), static_cast<int>(keyY + 1.0f), 
                       static_cast<int>(1.25f), static_cast<int>(keyH - 1.0f));

            juce::Colour cTop = isBlueBreeze ? UIColors::keyBedBlack : juce::Colour(0xFF34495E);
            juce::Colour cBottom = isBlueBreeze ? juce::Colour { BlueBreeze::Colors::KeyBlackBottom } : juce::Colour(0xFF1B2026);
            if (!inScale)
            {
                cTop = cTop.darker(outOfScaleDimAmount);
                cBottom = cBottom.darker(outOfScaleDimAmount);
            }

            juce::ColourGradient grad(cTop, 0.0f, keyRect.getY(),
                                      cBottom, 0.0f, keyRect.getBottom(), false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(keyRect, 2.0f);

            if (isBlueBreeze)
            {
                juce::ColourGradient keySource(juce::Colours::white.withAlpha(0.14f),
                                               keyRect.getX() + keyRect.getWidth() * 0.18f,
                                               keyRect.getY(),
                                               juce::Colours::transparentWhite,
                                               keyRect.getCentreX(),
                                               keyRect.getY() + keyRect.getHeight() * 0.42f,
                                               false);
                g.setGradientFill(keySource);
                g.fillRoundedRectangle(keyRect.reduced(0.5f), 1.8f);
            }

            // Scale highlight overlay on in-scale black keys (reduced alpha)
            if (inScale && ctx.scaleType != kScaleTypeChromatic)
            {
                g.setColour(isBlueBreeze ? UIColors::scaleHighlight.withMultipliedAlpha(0.22f)
                                         : UIColors::scaleHighlight.withMultipliedAlpha(0.5f));
                g.fillRoundedRectangle(keyRect, 2.0f);
            }

            // Pressed key highlight for black keys
            if (drawMidi == ctx.pressedPianoKey)
            {
                g.setColour(isBlueBreeze ? juce::Colour { BlueBreeze::Colors::KeyPressedGlow }.withAlpha(0.30f)
                                         : juce::Colour(0x500078D7));
                g.fillRoundedRectangle(keyRect, 2.0f);
            }

            g.setColour(juce::Colours::white.withAlpha(isBlueBreeze ? 0.14f : 0.2f));
            g.fillRect(keyRect.getX() + 2.0f, keyRect.getY(), keyRect.getWidth() - 4.0f, keyH * 0.15f);

            g.setColour(juce::Colours::black.withAlpha(isBlueBreeze ? 0.50f : 0.6f));
            g.drawRoundedRectangle(keyRect.reduced(0.5f), 2.0f, 1.0f);

            // Note name labels for black keys (drawn on top of the black key body with outline)
            if (effectiveNoteNameMode == 0)
            {
                const float fontSize = juce::jmax(8.0f, juce::jmin(h * 0.7f, 14.0f));
                g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultSansSerifFontName(), "Bold", fontSize)));
                int octave = (drawMidi / 12) - 1;
                const char* bkName = useFlats ? kFlatNames[noteInOctave] : kSharpNames[noteInOctave];
                juce::String noteName = juce::String(bkName) + juce::String(octave);

                const int tx = 0;
                const int ty = static_cast<int>(y);
                const int tw = w - 4;
                const int th = static_cast<int>(h);

                // Black key: light outline + dark text
                g.setColour(juce::Colours::white.withAlpha(0.7f));
                for (int ox = -1; ox <= 1; ++ox)
                    for (int oy = -1; oy <= 1; ++oy)
                        if (ox != 0 || oy != 0)
                            g.drawText(noteName, tx + ox, ty + oy, tw, th, juce::Justification::centredRight);

                g.setColour(juce::Colour(0xFF2A2A2A).withMultipliedAlpha(inScale ? 1.0f : 0.78f));
                g.drawText(noteName, tx, ty, tw, th, juce::Justification::centredRight);
            }
        }
    }

    g.setColour(isBlueBreeze ? UIColors::keyBedDivider.withAlpha(0.84f) : UIColors::panelBorder);
    g.drawVerticalLine(w, 0.0f, static_cast<float>(height));
}

void PianoRollRenderer::drawNotes(juce::Graphics& g,
                                  const RenderContext& ctx,
                                  const MaterializationRenderItem& item)
{
    const auto& notes = item.displayNotes;
    if (notes.empty()) return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    auto firstVisibleNote = std::lower_bound(
        notes.begin(),
        notes.end(),
        visibleWindow.visibleMaterializationStartTime,
        [](const Note& note, double visibleMaterializationStartTime) {
            return note.startTime < visibleMaterializationStartTime;
        });

    if (firstVisibleNote != notes.begin())
    {
        const auto previousVisibleNote = std::prev(firstVisibleNote);
        if (previousVisibleNote->endTime > visibleWindow.visibleMaterializationStartTime)
            firstVisibleNote = previousVisibleNote;
    }

    const auto lastVisibleNote = std::lower_bound(
        firstVisibleNote,
        notes.end(),
        visibleWindow.visibleMaterializationEndTime,
        [](const Note& note, double visibleMaterializationEndTime) {
            return note.startTime < visibleMaterializationEndTime;
        });

    int selectedCount = 0;
    for (auto noteIt = firstVisibleNote; noteIt != lastVisibleNote; ++noteIt)
    {
        if (noteIt->selected)
            ++selectedCount;
    }

    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;

    for (auto noteIt = firstVisibleNote; noteIt != lastVisibleNote; ++noteIt)
    {
        const auto& note = *noteIt;
        float adjustedPitch = note.getAdjustedPitch();
        if (adjustedPitch <= 0.0f) continue;

        float midi = ctx.freqToMidi(adjustedPitch);
        float y = ctx.midiToY(midi) - (ctx.pixelsPerSemitone * 0.5f);
        float h = ctx.pixelsPerSemitone;

        // §8.5 — note.startTime/endTime are SOURCE time; project through τ
        // so a stretched segment renders at its correct visual width.
        int x1 = sourceTimeToScreenX(note.startTime, ctx, item);
        int x2 = sourceTimeToScreenX(note.endTime,   ctx, item);
        if (x2 <= visibleWindow.viewportStartX || x1 >= visibleWindow.viewportEndX)
            continue;

        float w = std::max(1.0f, static_cast<float>(x2 - x1));
        auto noteBounds = juce::Rectangle<float>(static_cast<float>(x1), y, w, h);

        const auto noteColor = note.selected ? UIColors::noteBlockSelected : UIColors::noteBlock;

        if (isAurora)
        {
            g.setColour(noteColor.withAlpha(note.selected ? 0.82f : 0.70f));
            g.fillRect(noteBounds);

            auto topSheenBounds = noteBounds.withHeight(juce::jmin(noteBounds.getHeight() * 0.42f, 7.0f));
            juce::ColourGradient topSheen(noteColor.brighter(0.58f).withAlpha(note.selected ? 0.30f : 0.24f),
                                          topSheenBounds.getX(),
                                          topSheenBounds.getY(),
                                          juce::Colours::transparentWhite,
                                          topSheenBounds.getX(),
                                          topSheenBounds.getBottom(),
                                          false);
            g.setGradientFill(topSheen);
            g.fillRect(topSheenBounds);

            const auto edgeColour = note.selected
                ? noteColor.brighter(0.32f)
                : UIColors::noteBlockBorder;
            g.setColour(edgeColour.withAlpha(note.selected ? 0.98f : 0.90f));
            g.drawRect(noteBounds, note.selected ? 1.6f : 1.35f);

            g.setColour(UIColors::glassHighlight.withAlpha(note.selected ? 0.34f : 0.26f));
            g.drawLine(noteBounds.getX() + 1.0f,
                       noteBounds.getY() + 1.0f,
                       noteBounds.getRight() - 1.0f,
                       noteBounds.getY() + 1.0f,
                       1.0f);
        }
        else if (isBlueBreeze)
        {
            g.setColour(noteColor.withAlpha(note.selected ? 0.72f : 0.56f));
            g.fillRect(noteBounds);

            auto topSheenBounds = noteBounds.withHeight(juce::jmin(noteBounds.getHeight() * 0.42f, 6.0f));
            juce::ColourGradient topSheen(noteColor.brighter(0.42f).withAlpha(note.selected ? 0.28f : 0.20f),
                                          topSheenBounds.getX(),
                                          topSheenBounds.getY(),
                                          juce::Colours::transparentWhite,
                                          topSheenBounds.getX(),
                                          topSheenBounds.getBottom(),
                                          false);
            g.setGradientFill(topSheen);
            g.fillRect(topSheenBounds);

            g.setColour(noteColor.withAlpha(note.selected ? 0.24f : 0.14f));
            g.drawRect(noteBounds.expanded(1.0f, 0.5f), 2.0f);

            const auto edgeColour = note.selected
                ? noteColor.brighter(0.28f)
                : UIColors::noteBlockBorder;
            g.setColour(edgeColour.withAlpha(note.selected ? 0.90f : 0.72f));
            g.drawRect(noteBounds, note.selected ? 1.35f : 1.0f);
        }
        else
        {
            g.setColour(noteColor.withAlpha(0.8f));
            g.fillRect(noteBounds);

            const auto edgeColour = note.selected ? noteColor.brighter(0.3f) : UIColors::noteBlockBorder;
            g.setColour(edgeColour);
            g.drawRect(noteBounds, 1.5f);
        }
    }
}

void PianoRollRenderer::drawF0Curve(juce::Graphics& g,
                                     const std::vector<float>& f0,
                                     juce::Colour colour,
                                     float alpha,
                                     bool isThinLine,
                                     const RenderContext& ctx,
                                     const MaterializationRenderItem& item,
                                     const std::vector<uint8_t>* visibleMask)
{
    if (f0.empty()) return;

    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
    const float lineWidth = isAurora
        ? (isThinLine ? 1.65f : 2.65f)
        : (isBlueBreeze ? (isThinLine ? 1.25f : 2.05f) : (isThinLine ? 1.3f : 2.2f));
    const juce::PathStrokeType strokeType(lineWidth,
                                           juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded);
    const float glowLineWidth = lineWidth + (isAurora ? (isThinLine ? 4.2f : 4.8f) : 3.2f);
    const juce::PathStrokeType glowStrokeType(glowLineWidth,
                                               juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded);
    const juce::PathStrokeType innerGlowStrokeType(lineWidth + (isThinLine ? 1.25f : 1.45f),
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded);
    const juce::PathStrokeType highlightStrokeType(juce::jmax(0.75f, lineWidth * 0.46f),
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded);

    struct Segment {
        juce::Path path;
        std::size_t startIdx;
        std::size_t endIdx;
    };
    std::vector<Segment> segments;

    juce::Path currentPath;
    bool pathStarted = false;
    std::size_t segmentStart = 0;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    const int viewportStartX = visibleWindow.viewportStartX;
    const int viewportEndX = visibleWindow.viewportEndX;

    std::size_t iStart = 0;
    std::size_t iEnd = f0.size();
    if (!item.f0Timeline.isEmpty())
    {
        const int marginFrames = 10;

        const auto visibleFrames = item.f0Timeline.rangeForTimesWithMargin(visibleWindow.visibleMaterializationStartTime,
                                                                          visibleWindow.visibleMaterializationEndTime,
                                                                          marginFrames);
        iStart = std::min(static_cast<std::size_t>(visibleFrames.startFrame), f0.size());
        iEnd = std::min(static_cast<std::size_t>(std::max(visibleFrames.startFrame, visibleFrames.endFrameExclusive)),
                        f0.size());
    }

    for (std::size_t i = iStart; i < iEnd; ++i)
    {
        if (visibleMask != nullptr)
        {
            if (visibleMask->size() != f0.size() || (*visibleMask)[i] == 0)
            {
                if (pathStarted)
                {
                    segments.push_back({currentPath, segmentStart, i - 1});
                    currentPath.clear();
                    pathStarted = false;
                }
                continue;
            }
        }

        const float frequency = f0[i];

        if (frequency <= 0.0f || frequency < 20.0f || frequency > 2000.0f)
        {
            if (pathStarted)
            {
                segments.push_back({currentPath, segmentStart, i - 1});
                currentPath.clear();
                pathStarted = false;
            }
            continue;
        }

        float midi = ctx.freqToMidi(frequency);
        float y = ctx.midiToY(midi);

        // §8.5 — F0 timeline frames are anchored in SOURCE time; project
        // through τ so the F0 curve aligns with its (potentially stretched)
        // waveform underlay.
        const int x = sourceTimeToScreenX(
            item.f0Timeline.timeAtFrame(static_cast<int>(i)), ctx, item);

        if (x < viewportStartX || x > viewportEndX)
        {
            if (pathStarted)
            {
                segments.push_back({currentPath, segmentStart, i - 1});
                currentPath.clear();
                pathStarted = false;
            }
            continue;
        }

        if (!pathStarted)
        {
            currentPath.startNewSubPath(static_cast<float>(x), y);
            pathStarted = true;
            segmentStart = i;
        } else {
            currentPath.lineTo(static_cast<float>(x), y);
        }
    }

    if (pathStarted)
    {
        segments.push_back({currentPath, segmentStart, iEnd - 1});
    }

    const juce::Colour selectionColour = isAurora
        ? colour.brighter(0.18f)
        : UIColors::noteBlockSelected;
    const float selectionLineWidth = isAurora ? lineWidth + 1.1f : 3.0f;
    const juce::PathStrokeType selectionStrokeType(selectionLineWidth,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded);
    const auto drawNormalCurve = [&](const juce::Path& path, float effectiveAlpha)
    {
        if (isAurora)
        {
            g.setColour(colour.withAlpha(effectiveAlpha * (isThinLine ? 0.24f : 0.22f)));
            g.strokePath(path, glowStrokeType);
            g.setColour(colour.withAlpha(effectiveAlpha * (isThinLine ? 0.54f : 0.50f)));
            g.strokePath(path, innerGlowStrokeType);
            g.setColour(colour.withAlpha(effectiveAlpha));
            g.strokePath(path, strokeType);
            g.setColour(colour.brighter(isThinLine ? 0.38f : 0.24f).withAlpha(effectiveAlpha * (isThinLine ? 0.52f : 0.40f)));
            g.strokePath(path, highlightStrokeType);
            return;
        }

        if (isBlueBreeze)
        {
            g.setColour(colour.withAlpha(effectiveAlpha * (isThinLine ? 0.16f : 0.18f)));
            g.strokePath(path, glowStrokeType);
            g.setColour(colour.withAlpha(effectiveAlpha * (isThinLine ? 0.36f : 0.34f)));
            g.strokePath(path, innerGlowStrokeType);
            g.setColour(colour.withAlpha(effectiveAlpha * (isThinLine ? 0.94f : 0.84f)));
            g.strokePath(path, strokeType);
            g.setColour(colour.brighter(0.18f).withAlpha(effectiveAlpha * 0.24f));
            g.strokePath(path, highlightStrokeType);
            return;
        }

        g.setColour(colour.withAlpha(effectiveAlpha));
        g.strokePath(path, strokeType);
    };
    const auto drawSelectionCurve = [&](const juce::Path& path, float effectiveAlpha)
    {
        if (isAurora)
        {
            g.setColour(selectionColour.withAlpha(effectiveAlpha * 0.22f));
            g.strokePath(path, glowStrokeType);
        }
        else if (isBlueBreeze)
        {
            g.setColour(selectionColour.withAlpha(effectiveAlpha * 0.18f));
            g.strokePath(path, glowStrokeType);
        }

        g.setColour(selectionColour.withAlpha(effectiveAlpha));
        g.strokePath(path, selectionStrokeType);
    };

    for (const auto& seg : segments)
    {
        std::size_t segLen = seg.endIdx - seg.startIdx + 1;
        const int fadeFrames = 3;

        if (segLen <= static_cast<std::size_t>(fadeFrames * 2))
        {
            bool inSelection = ctx.hasF0Selection && 
                               static_cast<int>(seg.startIdx) >= ctx.f0SelectionStartFrame &&
                                static_cast<int>(seg.endIdx) < ctx.f0SelectionEndFrameExclusive;
            if (inSelection) {
                drawSelectionCurve(seg.path, alpha * 0.9f);
            } else {
                drawNormalCurve(seg.path, alpha * 0.7f);
            }
        } else {
            bool inSelection = ctx.hasF0Selection && 
                               static_cast<int>(seg.startIdx) >= ctx.f0SelectionStartFrame &&
                               static_cast<int>(seg.endIdx) < ctx.f0SelectionEndFrameExclusive;
            if (inSelection) {
                drawSelectionCurve(seg.path, alpha);
            } else {
                drawNormalCurve(seg.path, alpha);
            }

            for (int fade = 0; fade < fadeFrames; ++fade)
            {
                float fadeAlpha = alpha * (static_cast<float>(fade + 1) / static_cast<float>(fadeFrames + 1));

                std::size_t fadeStartIdx = seg.startIdx + static_cast<std::size_t>(fade);
                if (fadeStartIdx < seg.endIdx)
                {
                    const float freq = f0[fadeStartIdx];
                    if (freq > 20.0f && freq < 2000.0f)
                    {
                        float midi = ctx.freqToMidi(freq);
                        float y = ctx.midiToY(midi);
                        const int x = sourceTimeToScreenX(item.f0Timeline.timeAtFrame(static_cast<int>(fadeStartIdx)), ctx, item);

                        g.setColour(colour.brighter(isAurora ? 0.20f : 0.0f).withAlpha(fadeAlpha));
                        g.fillEllipse(static_cast<float>(x) - lineWidth * 0.5f, y - lineWidth * 0.5f, lineWidth, lineWidth);
                    }
                }

                std::size_t fadeEndIdx = seg.endIdx - static_cast<std::size_t>(fade);
                if (fadeEndIdx > seg.startIdx && fadeEndIdx < f0.size())
                {
                    const float freq = f0[fadeEndIdx];
                    if (freq > 20.0f && freq < 2000.0f)
                    {
                        float midi = ctx.freqToMidi(freq);
                        float y = ctx.midiToY(midi);
                        const int x = sourceTimeToScreenX(item.f0Timeline.timeAtFrame(static_cast<int>(fadeEndIdx)), ctx, item);

                        g.setColour(colour.brighter(isAurora ? 0.20f : 0.0f).withAlpha(fadeAlpha));
                        g.fillEllipse(static_cast<float>(x) - lineWidth * 0.5f, y - lineWidth * 0.5f, lineWidth, lineWidth);
                    }
                }
            }
        }
    }
}

// ============================================================================
// vocal-time-stretch §8.5 (Phase F) — TimeGrid handles overlay
//
// Each handle is rendered as a vertical guide line at output_seconds (because
// the piano roll x-axis represents OUTPUT/display time per the v7 "data stores
// source time, display uses output time" invariant).  HandleKind drives color:
//   ClipStart / ClipEnd     → solid grey, locked (dimmer)
//   OnsetVoiced             → cyan
//   OnsetSibilant           → yellow
//   OnsetSilence            → dim grey
//   InternalOnset           → lavender
//   UserAdded               → white
//
// Selected / hovered handles get extra emphasis (thicker line + glow box).
// ============================================================================
void PianoRollRenderer::drawTimeGridHandles(juce::Graphics& g, const RenderContext& ctx)
{
    if (ctx.timeGridSnapshot == nullptr) return;
    // §8.5 (Phase J / Journey-1 fix): handles are a Time-tool-only affordance.
    // Pitch view (note/F0 editing) hides them entirely so the user has a clean
    // canvas; Time view always shows them (including endpoint locked anchors
    // on identity grid as visual reference + double-click target).
    if (!ctx.isTimeView()) {
        return;
    }

    const int contentTop    = ctx.rulerHeight;
    const int contentBottom = ctx.height;
    if (contentBottom <= contentTop) return;

    auto colorForKind = [](HandleKind k) -> juce::Colour {
        switch (k) {
            case HandleKind::ClipStart:     return juce::Colours::lightgrey;
            case HandleKind::ClipEnd:       return juce::Colours::lightgrey;
            case HandleKind::OnsetVoiced:   return juce::Colour::fromRGB(64, 200, 220);
            case HandleKind::OnsetSibilant: return juce::Colour::fromRGB(220, 200, 64);
            case HandleKind::OnsetSilence:  return juce::Colours::dimgrey;
            case HandleKind::InternalOnset: return juce::Colour::fromRGB(180, 140, 220);
            case HandleKind::UserAdded:     return juce::Colours::white;
            case HandleKind::NoteOnly:      return juce::Colour::fromRGB(140, 200, 100); // soft green
        }
        return juce::Colours::white;
    };

    // add-note-confirmed-handles §4.1: High confidence handle 用金色高亮覆盖默认色,
    // 让用户视觉一眼识别"双源命中"的高置信度建议。Locked endpoint 不应用此覆盖
    // (per spec: locked overrides confidence styling)。
    const juce::Colour kHighConfidenceColour = juce::Colour::fromRGB(0xE0, 0xB0, 0x40); // #E0B040 金色

    for (const auto& h : ctx.timeGridSnapshot->handles()) {
        const int x = ctx.timeToX(h.output_seconds);
        if (x < ctx.pianoKeyWidth || x >= ctx.width) continue;

        juce::Colour col = colorForKind(h.kind);
        if (h.locked) {
            col = col.withAlpha(0.45f); // endpoints dimmer; confidence styling 不应用于 locked
        } else if (h.confidence == Confidence::High) {
            col = kHighConfidenceColour;
        }

        const bool selected = (ctx.timeGridSelectedHandleId == h.id);
        const bool hovered  = (ctx.timeGridHoveredHandleId  == h.id);
        // High confidence: 默认线宽 +50% (1.5×); selected/hovered 优先级仍最高。
        const bool isHigh = (!h.locked && h.confidence == Confidence::High);
        const float baseThickness = isHigh ? 1.5f : 1.0f;
        const float lineThickness = (selected ? 2.0f : (hovered ? 1.5f : baseThickness));

        // Vertical guide line spanning full content area (below ruler).
        g.setColour(col.withMultipliedAlpha(selected ? 1.0f : (hovered ? 0.85f : 0.65f)));
        g.drawLine(static_cast<float>(x),
                   static_cast<float>(contentTop),
                   static_cast<float>(x),
                   static_cast<float>(contentBottom),
                   lineThickness);

        // Top "diamond" affordance under the ruler so user can grab it.
        constexpr float kDiamondSize = 6.0f;
        const float cy = static_cast<float>(contentTop) + kDiamondSize;
        juce::Path diamond;
        diamond.startNewSubPath(static_cast<float>(x), cy - kDiamondSize);
        diamond.lineTo(static_cast<float>(x) + kDiamondSize, cy);
        diamond.lineTo(static_cast<float>(x), cy + kDiamondSize);
        diamond.lineTo(static_cast<float>(x) - kDiamondSize, cy);
        diamond.closeSubPath();
        g.setColour(col);
        g.fillPath(diamond);
        g.setColour(col.darker(0.35f));
        g.strokePath(diamond, juce::PathStrokeType(0.8f));
    }
}

} // namespace OpenTune
