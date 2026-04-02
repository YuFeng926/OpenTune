#include "PianoRollRenderer.h"
#include "../UIColors.h"
#include "../../../Utils/AppLogger.h"
#include <algorithm>
#include <cmath>

namespace OpenTune {

void PianoRollRenderer::updateCorrectedF0Cache(std::shared_ptr<const PitchCurveSnapshot> snapshot)
{
    if (!snapshot) {
        AppLogger::debug("[PianoRollRenderer] updateCorrectedF0Cache: snapshot is null, clearing cache");
        correctedF0Cache_.clear();
        cachedSnapshot_.reset();
        return;
    }

    const int totalFrames = static_cast<int>(snapshot->size());
    if (totalFrames <= 0 || !snapshot->hasAnyCorrection()) {
        AppLogger::debug("[PianoRollRenderer] updateCorrectedF0Cache: no corrections in snapshot, clearing cache");
        correctedF0Cache_.clear();
        cachedSnapshot_.reset();
        return;
    }

    AppLogger::debug("[PianoRollRenderer] updateCorrectedF0Cache: building cache for " + juce::String(totalFrames) + " frames");

    correctedF0Cache_.assign(static_cast<std::size_t>(totalFrames), 0.0f);
    snapshot->renderCorrectedOnlyRange(0, totalFrames,
        [&](int offsetFrame, const float* data, int length) {
            std::copy(data, data + length, correctedF0Cache_.begin() + offsetFrame);
        });
    cachedSnapshot_ = snapshot;
    
    AppLogger::debug("[PianoRollRenderer] updateCorrectedF0Cache: cache built successfully");
}

void PianoRollRenderer::drawBackground(juce::Graphics& g, const RenderContext& ctx)
{
    juce::ignoreUnused(ctx);
    PerfTimer timer("[PianoRollRenderer] drawBackground");
    g.fillAll(UIColors::rollBackground);
}

void PianoRollRenderer::drawLanes(juce::Graphics& g, const RenderContext& ctx)
{
    PerfTimer timer("[PianoRollRenderer] drawLanes");
    const int w = ctx.width;
    const int h = ctx.height;

    AppLogger::debug("[PianoRollRenderer] drawLanes: width=" + juce::String(w) + ", height=" + juce::String(h)
        + ", minMidi=" + juce::String(ctx.minMidi) + ", maxMidi=" + juce::String(ctx.maxMidi)
        + ", showLanes=" + juce::String(ctx.showLanes ? "true" : "false"));

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
            if (isBlackKey)
            {
                if (Theme::getActiveTheme() == ThemeId::BlueBreeze)
                {
                    g.setColour(juce::Colour(BlueBreeze::Colors::GraphBgDeep).withAlpha(0.6f));
                } else {
                    g.setColour(UIColors::backgroundDark.withAlpha(0.3f));
                }
                g.fillRect(static_cast<float>(ctx.pianoKeyWidth), y, static_cast<float>(w - ctx.pianoKeyWidth), laneH);
            }
        }

        g.setColour(UIColors::panelBorder.withAlpha(0.15f));
        g.drawLine(static_cast<float>(ctx.pianoKeyWidth), y, static_cast<float>(w), y, 1.0f);
    }
}

void PianoRollRenderer::drawWaveform(juce::Graphics& g, const RenderContext& ctx)
{
    PerfTimer timer("[PianoRollRenderer] drawWaveform");
    
    if (!ctx.hasUserAudio || !waveformMipmap_)
        return;

    const int startX = ctx.pianoKeyWidth;
    const int endX = ctx.width;
    const int w = endX - startX;
    if (w <= 0) return;

    const double pixelsPerSecond = 100.0 * ctx.zoomLevel;
    const int levelIndex = waveformMipmap_->selectBestLevelIndex(pixelsPerSecond);
    const auto& level = waveformMipmap_->getLevel(levelIndex);
    
    if (level.peaks.empty())
        return;

    AppLogger::debug("[PianoRollRenderer] drawWaveform: using level " + juce::String(levelIndex)
        + " with " + juce::String(static_cast<int>(level.peaks.size())) + " peaks");

    const double startTime = ctx.xToTime(startX);
    const double endTime = ctx.xToTime(endX);
    if (endTime <= startTime) return;

    const double startClipTime = startTime - ctx.trackOffsetSeconds;
    const double endClipTime = endTime - ctx.trackOffsetSeconds;
    const double clipVisibleDuration = endClipTime - startClipTime;
    if (clipVisibleDuration <= 0.0) return;

    const float centerY = ctx.height / 2.0f;
    const float amplitudeScale = ctx.height / 2.0f;

    g.setColour(UIColors::waveformFill.withAlpha(0.2f));

    const int samplesPerPeak = WaveformMipmap::kSamplesPerPeak[levelIndex];
    const double timePerPeak = static_cast<double>(samplesPerPeak) / WaveformMipmap::kBaseSampleRate;
    const int64_t numPeaks = static_cast<int64_t>(level.peaks.size());
    const int64_t builtPeaks = level.complete ? numPeaks : level.buildProgress;

    juce::Path waveformPath;

    for (int x = startX; x < endX; ++x)
    {
        const double time = ctx.xToTime(x) - ctx.trackOffsetSeconds;
        const int64_t peakIndex = static_cast<int64_t>(time / timePerPeak);
        
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
        g.strokePath(waveformPath, juce::PathStrokeType(1.0f));
    }
}

void PianoRollRenderer::drawTimeRuler(juce::Graphics& g, const RenderContext& ctx)
{
    PerfTimer timer("[PianoRollRenderer] drawTimeRuler");
    
    constexpr int inset = 12;
    auto bounds = juce::Rectangle<int>(0, 0, ctx.width, ctx.height).reduced(inset);
    auto rulerArea = bounds.removeFromTop(ctx.rulerHeight);

    const int rulerTop = rulerArea.getY();
    const int rulerBottom = rulerArea.getBottom();

    AppLogger::debug("[PianoRollRenderer] drawTimeRuler: timeUnit=" 
        + juce::String(static_cast<int>(ctx.timeUnit)) + ", bpm=" + juce::String(ctx.bpm)
        + ", zoomLevel=" + juce::String(ctx.zoomLevel));

    g.setColour(UIColors::backgroundMedium);
    g.fillRect(rulerArea);

    g.setColour(UIColors::panelBorder);
    g.drawLine(0.0f, static_cast<float>(rulerBottom), static_cast<float>(ctx.width), static_cast<float>(rulerBottom), 1.0f);

    if (ctx.timeUnit == RenderContext::TimeUnit::Bars)
    {
        double bpm = ctx.bpm;
        if (bpm <= 0.0) bpm = 120.0;

        double pixelsPerSecond = 100.0 * ctx.zoomLevel;
        double secondsPerBeat = 60.0 / bpm;
        double pixelsPerBeat = pixelsPerSecond * secondsPerBeat;

        double beatInterval = 1.0;
        if (pixelsPerBeat < 40.0) beatInterval = 4.0;
        if (pixelsPerBeat < 10.0) beatInterval = 8.0;
        if (pixelsPerBeat < 5.0) beatInterval = 16.0;
        if (pixelsPerBeat < 2.5) beatInterval = 32.0;

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

            g.setColour(UIColors::gridLine);
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerBottom - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerBottom), 1.0f);

            int64_t bar = (beat / 4) + 1;
            int64_t beatInBar = (beat % 4) + 1;

            juce::String label;
            if (beatInterval >= 4.0)
                label = juce::String(bar);
            else
                label = juce::String::formatted("%lld.%lld", (long long)bar, (long long)beatInBar);

            g.setColour(UIColors::textSecondary);
            g.drawText(label, pixelX - 20, rulerTop + 2, 40, ctx.rulerHeight - 12, juce::Justification::centred);
        }
    } else {
        double pixelsPerSecond = 100.0 * ctx.zoomLevel;

        double markerInterval = 1.0;
        if (pixelsPerSecond < 40.0) markerInterval = 5.0;
        if (pixelsPerSecond < 8.0) markerInterval = 10.0;
        if (pixelsPerSecond < 4.0) markerInterval = 30.0;
        if (pixelsPerSecond < 1.33) markerInterval = 60.0;

        double startTime = ctx.xToTime(0);
        if (startTime < 0.0) startTime = 0.0;
        startTime = std::floor(startTime / markerInterval) * markerInterval;

        double endTime = ctx.xToTime(ctx.width);

        g.setFont(UIColors::getUIFont(13.0f));
        for (double time = startTime; time < endTime; time += markerInterval)
        {
            int pixelX = ctx.timeToX(time);

            g.setColour(UIColors::gridLine);
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerBottom - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerBottom), 1.0f);

            int totalSecs = static_cast<int>(time);
            int mins = totalSecs / 60;
            int secs = totalSecs % 60;
            juce::String timeStr = juce::String::formatted("%d:%02d", mins, secs);

            g.setColour(UIColors::textSecondary);
            g.drawText(timeStr, pixelX - 20, rulerTop + 2, 40, ctx.rulerHeight - 12, juce::Justification::centred);
        }
    }
}

void PianoRollRenderer::drawGridLines(juce::Graphics& g, const RenderContext& ctx)
{
    PerfTimer timer("[PianoRollRenderer] drawGridLines");
    
    const auto themeId = Theme::getActiveTheme();
    AppLogger::debug("[PianoRollRenderer] drawGridLines: timeUnit=" + juce::String(static_cast<int>(ctx.timeUnit))
        + ", theme=" + juce::String(static_cast<int>(themeId)));

    if (ctx.timeUnit == RenderContext::TimeUnit::Bars)
    {
        double bpm = ctx.bpm;
        if (bpm <= 0.0) bpm = 120.0;

        double pixelsPerSecond = 100.0 * ctx.zoomLevel;
        double secondsPerBeat = 60.0 / bpm;
        double pixelsPerBeat = pixelsPerSecond * secondsPerBeat;

        double beatInterval = 1.0;
        if (pixelsPerBeat < 40.0) beatInterval = 4.0;
        if (pixelsPerBeat < 10.0) beatInterval = 8.0;
        if (pixelsPerBeat < 5.0) beatInterval = 16.0;
        if (pixelsPerBeat < 2.5) beatInterval = 32.0;

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

            if (themeId == ThemeId::DarkBlueGrey)
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
        double pixelsPerSecond = 100.0 * ctx.zoomLevel;

        double markerInterval = 1.0;
        if (pixelsPerSecond < 40.0) markerInterval = 5.0;
        if (pixelsPerSecond < 8.0) markerInterval = 10.0;
        if (pixelsPerSecond < 4.0) markerInterval = 30.0;
        if (pixelsPerSecond < 1.33) markerInterval = 60.0;

        double startTime = ctx.xToTime(0);
        if (startTime < 0.0) startTime = 0.0;
        startTime = std::floor(startTime / markerInterval) * markerInterval;

        double endTime = ctx.xToTime(ctx.width);

        if (markerInterval < 0.001) markerInterval = 1.0;

        for (double time = startTime; time < endTime + markerInterval; time += markerInterval)
        {
            int pixelX = ctx.timeToX(time);

            if (pixelX < ctx.pianoKeyWidth - 2 || pixelX > ctx.width + 2) continue;
            g.setColour(themeId == ThemeId::DarkBlueGrey ? UIColors::panelBorder.withAlpha(0.12f) : UIColors::panelBorder.withAlpha(0.25f));
            g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(ctx.height));
        }
    }
}

void PianoRollRenderer::drawPianoKeys(juce::Graphics& g, const RenderContext& ctx)
{
    PerfTimer timer("[PianoRollRenderer] drawPianoKeys");
    
    AppLogger::debug("[PianoRollRenderer] drawPianoKeys: pianoKeyWidth=" + juce::String(ctx.pianoKeyWidth)
        + ", minMidi=" + juce::String(ctx.minMidi) + ", maxMidi=" + juce::String(ctx.maxMidi)
        + ", scaleType=" + juce::String(ctx.scaleType) + ", scaleRootNote=" + juce::String(ctx.scaleRootNote));

    const int height = ctx.height;
    const int w = ctx.pianoKeyWidth;
    const float blackKeyWidthRatio = 0.6f;
    const float blackKeyW = w * blackKeyWidthRatio;
    static constexpr int kScaleTypeChromatic = 3;
    static constexpr float kOutOfScaleDimAmount = 0.12f;

    std::array<bool, 12> inScalePitchClass{};
    inScalePitchClass.fill(true);

    if (ctx.scaleType != kScaleTypeChromatic)
    {
        inScalePitchClass.fill(false);
        const int rootPc = juce::jlimit(0, 11, ctx.scaleRootNote);
        if (ctx.scaleType == 2) {
            static constexpr std::array<int, 7> kNaturalMinorIntervals{0, 2, 3, 5, 7, 8, 10};
            for (const int interval : kNaturalMinorIntervals)
                inScalePitchClass[(rootPc + interval) % 12] = true;
        } else {
            static constexpr std::array<int, 7> kMajorIntervals{0, 2, 4, 5, 7, 9, 11};
            for (const int interval : kMajorIntervals)
                inScalePitchClass[(rootPc + interval) % 12] = true;
        }
    }

    const auto isMidiInCurrentScale = [&inScalePitchClass](int midiNote) noexcept {
        const int pitchClass = ((midiNote % 12) + 12) % 12;
        return inScalePitchClass[static_cast<std::size_t>(pitchClass)];
    };

    g.setColour(UIColors::backgroundDark);
    g.fillRect(0, 0, w, height);

    juce::Colour cWhite1(0xFFF7F9F9);
    juce::Colour cWhite2(0xFFECF0F1);

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

            const juce::Colour whiteA = inScale ? cWhite1 : cWhite1.darker(kOutOfScaleDimAmount);
            const juce::Colour whiteB = inScale ? cWhite2 : cWhite2.darker(kOutOfScaleDimAmount);

            juce::ColourGradient grad(whiteA, 0.0f, y, whiteB, static_cast<float>(w), y, false);
            g.setGradientFill(grad);
            g.fillRect(keyRect);

            if (noteInOctave == 0)
            {
                g.setColour(juce::Colour(0xFF7F8C8D).withMultipliedAlpha(inScale ? 1.0f : 0.78f));
                g.setFont(UIColors::getLabelFont(10.0f).withStyle(juce::Font::bold));
                int octave = (drawMidi / 12) - 1;
                juce::String noteName = "C" + juce::String(octave);
                g.drawText(noteName, 0, static_cast<int>(y), w - 4, static_cast<int>(h), juce::Justification::centredRight);
            }
        } else {
            const bool inScale = isMidiInCurrentScale(drawMidi);

            juce::Rectangle<float> extensionRect(blackKeyW, y, static_cast<float>(w) - blackKeyW, drawH);

            const juce::Colour extensionA = inScale ? cWhite1 : cWhite1.darker(kOutOfScaleDimAmount);
            const juce::Colour extensionB = inScale ? cWhite2 : cWhite2.darker(kOutOfScaleDimAmount);
            juce::ColourGradient grad(extensionA, blackKeyW, y, extensionB, static_cast<float>(w), y, false);
            g.setGradientFill(grad);
            g.fillRect(extensionRect);

            g.setColour(UIColors::panelBorder);
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
            g.setColour(UIColors::panelBorder);
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
            ds.colour = juce::Colours::black.withAlpha(0.25f);
            ds.radius = 5;
            ds.offset = {0, 1};

            juce::Path shadowPath;
            shadowPath.addRoundedRectangle(keyRect, 2.0f);
            ds.drawForPath(g, shadowPath);

            juce::ColourGradient sideShadow(juce::Colours::black.withAlpha(0.2f), blackKeyW, keyY,
                                            juce::Colours::transparentBlack, blackKeyW + 1.25f, keyY, false);
            g.setGradientFill(sideShadow);
            g.fillRect(static_cast<int>(blackKeyW), static_cast<int>(keyY + 1.0f), 
                       static_cast<int>(1.25f), static_cast<int>(keyH - 1.0f));

            juce::Colour cTop(0xFF34495E);
            juce::Colour cBottom(0xFF1B2026);
            if (!inScale)
            {
                cTop = cTop.darker(kOutOfScaleDimAmount);
                cBottom = cBottom.darker(kOutOfScaleDimAmount);
            }

            juce::ColourGradient grad(cTop, 0.0f, keyRect.getY(),
                                      cBottom, 0.0f, keyRect.getBottom(), false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(keyRect, 2.0f);

            g.setColour(juce::Colours::white.withAlpha(0.2f));
            g.fillRect(keyRect.getX() + 2.0f, keyRect.getY(), keyRect.getWidth() - 4.0f, keyH * 0.15f);

            g.setColour(juce::Colours::black.withAlpha(0.6f));
            g.drawRoundedRectangle(keyRect.reduced(0.5f), 2.0f, 1.0f);
        }
    }

    g.setColour(UIColors::panelBorder);
    g.drawVerticalLine(w, 0.0f, static_cast<float>(height));
}

void PianoRollRenderer::drawNotes(juce::Graphics& g, const RenderContext& ctx,
                                   const std::vector<Note>& notes,
                                   double trackOffsetSeconds)
{
    PerfTimer timer("[PianoRollRenderer] drawNotes");
    
    if (notes.empty()) return;

    AppLogger::debug("[PianoRollRenderer] drawNotes: noteCount=" + juce::String(static_cast<int>(notes.size()))
        + ", trackOffsetSeconds=" + juce::String(trackOffsetSeconds));

    int selectedCount = 0;
    for (const auto& note : notes)
    {
        if (note.selected) ++selectedCount;
    }
    AppLogger::debug("[PianoRollRenderer] drawNotes: selectedCount=" + juce::String(selectedCount));

    for (const auto& note : notes)
    {
        float adjustedPitch = note.getAdjustedPitch();
        if (adjustedPitch <= 0.0f) continue;

        float midi = ctx.freqToMidi(adjustedPitch);
        float y = ctx.midiToY(midi) - (ctx.pixelsPerSemitone * 0.5f);
        float h = ctx.pixelsPerSemitone;

        double noteStartTime = note.startTime + trackOffsetSeconds;
        double noteEndTime = note.endTime + trackOffsetSeconds;

        int x1 = ctx.timeToX(noteStartTime);
        int x2 = ctx.timeToX(noteEndTime);
        float w = static_cast<float>(x2 - x1);

        juce::Colour noteColor = note.selected
            ? juce::Colour(0xFFE74C3C)
            : juce::Colour(0xFF87CEEB);

        g.setColour(noteColor.withAlpha(0.8f));
        g.fillRect(static_cast<float>(x1), y, w, h);

        g.setColour(noteColor.brighter(0.3f));
        g.drawRect(static_cast<float>(x1), y, w, h, 1.5f);
    }
}

void PianoRollRenderer::drawF0Curve(juce::Graphics& g,
                                     const std::vector<float>& f0,
                                     juce::Colour colour,
                                     float alpha,
                                     bool isThinLine,
                                     const RenderContext& ctx,
                                     std::shared_ptr<PitchCurve> currentCurve,
                                     const std::vector<uint8_t>* visibleMask)
{
    PerfTimer timer("[PianoRollRenderer] drawF0Curve");
    
    if (f0.empty()) return;

    AppLogger::debug("[PianoRollRenderer] drawF0Curve: f0.size=" + juce::String(static_cast<int>(f0.size()))
        + ", alpha=" + juce::String(alpha) + ", isThinLine=" + juce::String(isThinLine ? "true" : "false")
        + ", visibleMask=" + juce::String(visibleMask ? "provided" : "null"));

    const float lineWidth = isThinLine ? 1.3f : 2.2f;

    struct Segment {
        juce::Path path;
        std::size_t startIdx;
        std::size_t endIdx;
    };
    std::vector<Segment> segments;

    juce::Path currentPath;
    bool pathStarted = false;
    std::size_t segmentStart = 0;

    std::size_t stableFrameLength = f0.size();
    if (currentCurve != nullptr)
    {
        auto snapshot = currentCurve->getSnapshot();
        const std::size_t curveFrames = snapshot->size();
        if (curveFrames > 0)
        {
            stableFrameLength = curveFrames;
        }
    }

    const int contentStartX = ctx.pianoKeyWidth;
    const int contentEndX = ctx.width;

    double visibleStartTime = ctx.xToTime(contentStartX);
    double visibleEndTime = ctx.xToTime(contentEndX);

    std::size_t iStart = 0;
    std::size_t iEnd = f0.size();

    if (ctx.hopSize > 0 && ctx.f0SampleRate > 0.0)
    {
        const int marginFrames = 10;

        const double framesPerSecond = ctx.f0SampleRate / static_cast<double>(ctx.hopSize);
        const int startFrame = std::max(0, static_cast<int>(std::floor((visibleStartTime - ctx.trackOffsetSeconds) * framesPerSecond)) - marginFrames);
        const int endFrame = std::min(static_cast<int>(f0.size()), static_cast<int>(std::ceil((visibleEndTime - ctx.trackOffsetSeconds) * framesPerSecond)) + marginFrames);
        iStart = static_cast<std::size_t>(startFrame);
        iEnd = static_cast<std::size_t>(std::max(startFrame, endFrame));
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

        const double frameTime = static_cast<double>(i) * static_cast<double>(ctx.hopSize) / ctx.f0SampleRate;
        const double absoluteTime = frameTime + ctx.trackOffsetSeconds;
        const int x = ctx.timeToX(absoluteTime);

        if (x < contentStartX || x > contentEndX)
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

    juce::PathStrokeType strokeType(lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    const juce::Colour selectionColour(0xFFE74C3C);
    const float selectionLineWidth = 3.0f;

    for (const auto& seg : segments)
    {
        std::size_t segLen = seg.endIdx - seg.startIdx + 1;
        const int fadeFrames = 3;

        if (segLen <= static_cast<std::size_t>(fadeFrames * 2))
        {
            bool inSelection = ctx.hasF0Selection && 
                               static_cast<int>(seg.startIdx) >= ctx.f0SelectionStartFrame &&
                               static_cast<int>(seg.endIdx) <= ctx.f0SelectionEndFrame;
            if (inSelection) {
                g.setColour(selectionColour.withAlpha(alpha * 0.9f));
                juce::PathStrokeType selStroke(selectionLineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
                g.strokePath(seg.path, selStroke);
            } else {
                g.setColour(colour.withAlpha(alpha * 0.7f));
                g.strokePath(seg.path, strokeType);
            }
        } else {
            bool inSelection = ctx.hasF0Selection && 
                               static_cast<int>(seg.startIdx) >= ctx.f0SelectionStartFrame &&
                               static_cast<int>(seg.endIdx) <= ctx.f0SelectionEndFrame;
            if (inSelection) {
                g.setColour(selectionColour.withAlpha(alpha));
                juce::PathStrokeType selStroke(selectionLineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
                g.strokePath(seg.path, selStroke);
            } else {
                g.setColour(colour.withAlpha(alpha));
                g.strokePath(seg.path, strokeType);
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
                        const double frameTime = static_cast<double>(fadeStartIdx) * static_cast<double>(ctx.hopSize) / ctx.f0SampleRate;
                        const double absoluteTime = frameTime + ctx.trackOffsetSeconds;
                        const int x = ctx.timeToX(absoluteTime);

                        g.setColour(colour.withAlpha(fadeAlpha));
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
                        const double frameTime = static_cast<double>(fadeEndIdx) * static_cast<double>(ctx.hopSize) / ctx.f0SampleRate;
                        const double absoluteTime = frameTime + ctx.trackOffsetSeconds;
                        const int x = ctx.timeToX(absoluteTime);

                        g.setColour(colour.withAlpha(fadeAlpha));
                        g.fillEllipse(static_cast<float>(x) - lineWidth * 0.5f, y - lineWidth * 0.5f, lineWidth, lineWidth);
                    }
                }
            }
        }
    }
    
    AppLogger::debug("[PianoRollRenderer] drawF0Curve: completed, segments drawn=" 
        + juce::String(static_cast<int>(segments.size())));
}

} // namespace OpenTune
