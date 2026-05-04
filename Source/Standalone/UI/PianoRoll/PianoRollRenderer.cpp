#include "PianoRollRenderer.h"
#include "../UIColors.h"
#include "../../../Utils/AppLogger.h"
#include "../../../Utils/NoteGenerator.h"
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

VisibleTimeWindow computeVisibleTimeWindow(const PianoRollRenderer::RenderContext& ctx)
{
    VisibleTimeWindow window;
    if (!ctx.materializationProjection.isValid()) {
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

    window.visibleMaterializationStartTime = ctx.materializationProjection.projectTimelineTimeToMaterialization(window.visibleStartTime);
    window.visibleMaterializationEndTime = ctx.materializationProjection.projectTimelineTimeToMaterialization(window.visibleEndTime);
    return window;
}

bool isVoicedFrame(float frequencyHz) noexcept
{
    return frequencyHz > 0.0f;
}

} // namespace

void PianoRollRenderer::updateCorrectedF0Cache(std::shared_ptr<const PitchCurveSnapshot> snapshot)
{
    if (!snapshot) {
        correctedF0Cache_.clear();
        cachedSnapshot_.reset();
        return;
    }

    const int totalFrames = static_cast<int>(snapshot->size());
    if (totalFrames <= 0 || !snapshot->hasAnyCorrection()) {
        correctedF0Cache_.clear();
        cachedSnapshot_.reset();
        return;
    }

    correctedF0Cache_.assign(static_cast<std::size_t>(totalFrames), 0.0f);
    snapshot->renderCorrectedOnlyRange(0, totalFrames,
        [&](int offsetFrame, const float* data, int length) {
            std::copy(data, data + length, correctedF0Cache_.begin() + offsetFrame);
        });
    cachedSnapshot_ = snapshot;
}

void PianoRollRenderer::drawLanes(juce::Graphics& g, const RenderContext& ctx)
{
    const int w = ctx.width;
    const int h = ctx.height;
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
            if (isBlackKey)
            {
                if (UIColors::currentThemeId() == ThemeId::BlueBreeze)
                {
                    g.setColour(juce::Colour(BlueBreeze::Colors::GraphBgDeep).withAlpha(0.6f));
                } else {
                    g.setColour(UIColors::backgroundDark.withAlpha(0.3f));
                }
                g.fillRect(static_cast<float>(ctx.pianoKeyWidth), y, static_cast<float>(w - ctx.pianoKeyWidth), laneH);
            }

            // Scale-aware lane highlighting: tint in-scale lanes with scaleHighlight color
            if (ctx.scaleType != kScaleTypeChromatic)
            {
                const int pitchClass = ((midi % 12) + 12) % 12;
                if (inScalePitchClass[static_cast<std::size_t>(pitchClass)])
                {
                    g.setColour(UIColors::scaleHighlight.withMultipliedAlpha(0.65f));
                    g.fillRect(static_cast<float>(ctx.pianoKeyWidth), y, static_cast<float>(w - ctx.pianoKeyWidth), laneH);
                }
            }
        }

        g.setColour(UIColors::panelBorder.withAlpha(0.15f));
        g.drawLine(static_cast<float>(ctx.pianoKeyWidth), y, static_cast<float>(w), y, 1.0f);
    }
}

void PianoRollRenderer::drawUnvoicedFrameBands(juce::Graphics& g, const RenderContext& ctx)
{
    if (!ctx.showUnvoicedFrames || ctx.pitchSnapshot == nullptr || ctx.f0Timeline.isEmpty()) {
        return;
    }

    const auto& originalF0 = ctx.pitchSnapshot->getOriginalF0();
    if (originalF0.empty()) {
        return;
    }

    const auto visibleWindow = computeVisibleTimeWindow(ctx);
    if (!visibleWindow.isValid()) {
        return;
    }

    const auto visibleFrames = ctx.f0Timeline.rangeForTimesWithMargin(visibleWindow.visibleMaterializationStartTime,
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

        const double startSeconds = ctx.materializationProjection.projectMaterializationTimeToTimeline(
            ctx.f0Timeline.timeAtFrame(startFrame));
        const double endSeconds = ctx.materializationProjection.projectMaterializationTimeToTimeline(
            ctx.f0Timeline.timeAtFrame(endFrameExclusive));
        const int x1 = std::max(ctx.pianoKeyWidth, ctx.timeToX(startSeconds));
        const int x2 = std::min(ctx.width, ctx.timeToX(endSeconds));
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

void PianoRollRenderer::drawWaveform(juce::Graphics& g, const RenderContext& ctx)
{
    if (!ctx.hasUserAudio || !waveformMipmap_)
        return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx);
    if (!visibleWindow.isValid())
        return;

    const int startX = visibleWindow.viewportStartX;
    const int endX = visibleWindow.viewportEndX;
    const int w = endX - startX;
    if (w <= 0) return;

    const int levelIndex = waveformMipmap_->selectBestLevelIndex(ctx.pixelsPerSecond);
    const auto& level = waveformMipmap_->getLevel(levelIndex);
    
    if (level.peaks.empty())
        return;

    const double materializationVisibleDuration = visibleWindow.visibleMaterializationEndTime - visibleWindow.visibleMaterializationStartTime;
    if (materializationVisibleDuration <= 0.0) return;

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
        const double time = ctx.materializationProjection.projectTimelineTimeToMaterialization(ctx.xToTime(x));
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
    constexpr int inset = 12;
    auto bounds = juce::Rectangle<int>(0, 0, ctx.width, ctx.height).reduced(inset);
    auto rulerArea = bounds.removeFromTop(ctx.rulerHeight);

    const int rulerTop = rulerArea.getY();
    const int rulerBottom = rulerArea.getBottom();

    g.setColour(UIColors::backgroundMedium);
    g.fillRect(rulerArea);

    g.setColour(UIColors::panelBorder);
    g.drawLine(0.0f, static_cast<float>(rulerBottom), static_cast<float>(ctx.width), static_cast<float>(rulerBottom), 1.0f);

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
    const auto themeId = UIColors::currentThemeId();

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
            g.setColour(themeId == ThemeId::DarkBlueGrey ? UIColors::panelBorder.withAlpha(0.12f) : UIColors::panelBorder.withAlpha(0.25f));
            g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(ctx.height));
        }
    }
}

void PianoRollRenderer::drawChunkBoundaries(juce::Graphics& g, const RenderContext& ctx)
{
    if (!ctx.showChunkBoundaries || ctx.chunkBoundaries.size() < 3) {
        return;
    }

    static constexpr float dashLengths[] { 4.0f, 4.0f };
    g.setColour(UIColors::accent.withAlpha(0.75f));

    for (std::size_t index = 1; index + 1 < ctx.chunkBoundaries.size(); ++index) {
        const double absoluteSeconds = ctx.materializationProjection.projectMaterializationTimeToTimeline(ctx.chunkBoundaries[index]);
        const int x = ctx.timeToX(absoluteSeconds);
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

            // Scale highlight overlay on in-scale white keys
            if (inScale && ctx.scaleType != kScaleTypeChromatic)
            {
                g.setColour(UIColors::scaleHighlight);
                g.fillRect(keyRect);
            }

            // Pressed key highlight
            if (drawMidi == ctx.pressedPianoKey)
            {
                g.setColour(juce::Colour(0x500078D7));
                g.fillRect(keyRect);
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

                g.setColour(juce::Colour(0xFFE0E0E0).withMultipliedAlpha(inScale ? 1.0f : 0.78f));
                g.drawText(noteName, tx, ty, tw, th, juce::Justification::centredRight);
            }
        } else {
            const bool inScale = isMidiInCurrentScale(drawMidi);

            juce::Rectangle<float> extensionRect(blackKeyW, y, static_cast<float>(w) - blackKeyW, drawH);

            const juce::Colour extensionA = inScale ? cWhite1 : cWhite1.darker(kOutOfScaleDimAmount);
            const juce::Colour extensionB = inScale ? cWhite2 : cWhite2.darker(kOutOfScaleDimAmount);
            juce::ColourGradient grad(extensionA, blackKeyW, y, extensionB, static_cast<float>(w), y, false);
            g.setGradientFill(grad);
            g.fillRect(extensionRect);

            // Scale highlight overlay on in-scale black key extension area
            if (inScale && ctx.scaleType != kScaleTypeChromatic)
            {
                g.setColour(UIColors::scaleHighlight);
                g.fillRect(extensionRect);
            }

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

            // Scale highlight overlay on in-scale black keys (reduced alpha)
            if (inScale && ctx.scaleType != kScaleTypeChromatic)
            {
                g.setColour(UIColors::scaleHighlight.withMultipliedAlpha(0.5f));
                g.fillRoundedRectangle(keyRect, 2.0f);
            }

            // Pressed key highlight for black keys
            if (drawMidi == ctx.pressedPianoKey)
            {
                g.setColour(juce::Colour(0x500078D7));
                g.fillRoundedRectangle(keyRect, 2.0f);
            }

            g.setColour(juce::Colours::white.withAlpha(0.2f));
            g.fillRect(keyRect.getX() + 2.0f, keyRect.getY(), keyRect.getWidth() - 4.0f, keyH * 0.15f);

            g.setColour(juce::Colours::black.withAlpha(0.6f));
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

    g.setColour(UIColors::panelBorder);
    g.drawVerticalLine(w, 0.0f, static_cast<float>(height));
}

void PianoRollRenderer::drawNotes(juce::Graphics& g, const RenderContext& ctx,
                                  const std::vector<Note>& notes)
{
    if (notes.empty()) return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx);
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

    for (auto noteIt = firstVisibleNote; noteIt != lastVisibleNote; ++noteIt)
    {
        const auto& note = *noteIt;
        float adjustedPitch = note.getAdjustedPitch();
        if (adjustedPitch <= 0.0f) continue;

        float midi = ctx.freqToMidi(adjustedPitch);
        float y = ctx.midiToY(midi) - (ctx.pixelsPerSemitone * 0.5f);
        float h = ctx.pixelsPerSemitone;

        double noteStartTime = ctx.materializationProjection.projectMaterializationTimeToTimeline(note.startTime);
        double noteEndTime = ctx.materializationProjection.projectMaterializationTimeToTimeline(note.endTime);

        int x1 = ctx.timeToX(noteStartTime);
        int x2 = ctx.timeToX(noteEndTime);
        if (x2 <= visibleWindow.viewportStartX || x1 >= visibleWindow.viewportEndX)
            continue;

        float w = std::max(1.0f, static_cast<float>(x2 - x1));

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
    if (f0.empty()) return;

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

    const auto visibleWindow = computeVisibleTimeWindow(ctx);
    if (!visibleWindow.isValid())
        return;

    const int viewportStartX = visibleWindow.viewportStartX;
    const int viewportEndX = visibleWindow.viewportEndX;

    std::size_t iStart = 0;
    std::size_t iEnd = static_cast<std::size_t>(ctx.f0Timeline.endFrameExclusive());

    if (!ctx.f0Timeline.isEmpty())
    {
        const int marginFrames = 10;

        const auto visibleFrames = ctx.f0Timeline.rangeForTimesWithMargin(visibleWindow.visibleMaterializationStartTime,
                                                                         visibleWindow.visibleMaterializationEndTime,
                                                                         marginFrames);
        iStart = static_cast<std::size_t>(visibleFrames.startFrame);
        iEnd = static_cast<std::size_t>(std::max(visibleFrames.startFrame, visibleFrames.endFrameExclusive));
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

        const double absoluteTime = ctx.materializationProjection.projectMaterializationTimeToTimeline(ctx.f0Timeline.timeAtFrame(static_cast<int>(i)));
        const int x = ctx.timeToX(absoluteTime);

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
                                static_cast<int>(seg.endIdx) < ctx.f0SelectionEndFrameExclusive;
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
                               static_cast<int>(seg.endIdx) < ctx.f0SelectionEndFrameExclusive;
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
                        const double absoluteTime = ctx.materializationProjection.projectMaterializationTimeToTimeline(ctx.f0Timeline.timeAtFrame(static_cast<int>(fadeStartIdx)));
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
                        const double absoluteTime = ctx.materializationProjection.projectMaterializationTimeToTimeline(ctx.f0Timeline.timeAtFrame(static_cast<int>(fadeEndIdx)));
                        const int x = ctx.timeToX(absoluteTime);

                        g.setColour(colour.withAlpha(fadeAlpha));
                        g.fillEllipse(static_cast<float>(x) - lineWidth * 0.5f, y - lineWidth * 0.5f, lineWidth, lineWidth);
                    }
                }
            }
        }
    }
}

} // namespace OpenTune
