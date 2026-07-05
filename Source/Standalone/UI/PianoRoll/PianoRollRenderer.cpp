#include "PianoRollRenderer.h"
#include "../UiAssets.h"
#include "../UIColors.h"
#include "../../../Utils/AppLogger.h"
#include "../../../Utils/LegacyNoteGenerator.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>

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
    double visibleContentStartTime = 0.0;
    double visibleContentEndTime = 0.0;

    bool isValid() const
    {
        return viewportEndX > viewportStartX
            && visibleEndTime > visibleStartTime
            && visibleContentEndTime > visibleContentStartTime;
    }
};

VisibleTimeWindow computeVisibleTimeWindow(const PianoRollRenderer::RenderContext& ctx,
                                           const PianoRollRenderer::ContentRenderItem& item)
{
    VisibleTimeWindow window;
    if (!item.projection.isValid()) {
        return window;
    }

    window.viewportStartX = ctx.pianoKeyWidth;
    window.viewportEndX = ctx.width;
    if (window.viewportEndX <= window.viewportStartX)
        return {};

    window.visibleStartTime = ctx.coords.xToTime(window.viewportStartX);
    window.visibleEndTime = ctx.coords.xToTime(window.viewportEndX);
    if (window.visibleEndTime <= window.visibleStartTime)
        return {};

    window.visibleContentStartTime = item.projection.projectTimelineTimeToContent(window.visibleStartTime);
    window.visibleContentEndTime = item.projection.projectTimelineTimeToContent(window.visibleEndTime);

    // vocal-time-stretch 搂8.5 鈥?projectTimelineTimeToContent returns OUTPUT time
    // inside the content, but Notes / PitchCurve / F0 timeline / WaveformMipmap
    // are all indexed by SOURCE time. Convert to source time via tauInverse.
    jassert(ctx.timeGridSnapshot != nullptr);
    window.visibleContentStartTime = ctx.timeGridSnapshot->tauInverse(window.visibleContentStartTime);
    window.visibleContentEndTime   = ctx.timeGridSnapshot->tauInverse(window.visibleContentEndTime);
    return window;
}

// vocal-time-stretch 搂8.5 鈥?convert a SOURCE-time anchor (Note.startTime,
// f0Timeline frame timestamp, WaveformMipmap peak) into screen X via the
// item's projection. Identity TimeGrid 鈫?degenerates to existing pipeline.
inline int sourceTimeToScreenX(double sourceTime,
                                const PianoRollRenderer::RenderContext& ctx,
                                const PianoRollRenderer::ContentRenderItem& item)
{
    jassert(ctx.timeGridSnapshot != nullptr);
    const double outputTime = ctx.timeGridSnapshot->tauForward(sourceTime);
    const double timelineTime = item.projection.projectContentTimeToTimeline(outputTime);
    return ctx.coords.timeToX(timelineTime);
}

} // namespace

void PianoRollRenderer::drawUnvoicedFrameBands(juce::Graphics& g,
                                               const RenderContext& ctx,
                                               const ContentRenderItem& item)
{
    if (!ctx.showUnvoicedFrames || !item.f0LOD || item.f0Timeline.isEmpty())
        return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    const auto& level = item.f0LOD->selectBestLevel(
        ctx.pixelsPerSecond,
        item.pitchSnapshot->getSampleRate(),
        item.pitchSnapshot->getHopSize());

    const auto bandColour = UIColors::currentThemeId() == ThemeId::DarkBlueGrey
        ? UIColors::backgroundDark.withAlpha(0.28f)
        : UIColors::backgroundMedium.withAlpha(0.22f);
    g.setColour(bandColour);

    for (const auto& interval : level.unvoicedIntervals) {
        if (!interval.isValid())
            continue;

        const double intervalStartTime = item.f0Timeline.timeAtFrame(interval.startFrame);
        const double intervalEndTime = item.f0Timeline.timeAtFrame(interval.endFrameExclusive);

        if (intervalEndTime <= visibleWindow.visibleContentStartTime ||
            intervalStartTime >= visibleWindow.visibleContentEndTime)
            continue;

        const int x1 = sourceTimeToScreenX(intervalStartTime, ctx, item);
        const int x2 = sourceTimeToScreenX(intervalEndTime, ctx, item);

        if (x2 <= ctx.pianoKeyWidth || x1 >= ctx.width)
            continue;

        const float drawX = static_cast<float>(std::max(x1, ctx.pianoKeyWidth));
        const float drawW = static_cast<float>(std::min(x2, ctx.width)) - drawX;

        if (drawW > 0.5f) {
            g.fillRect(drawX, static_cast<float>(ctx.rulerHeight),
                       drawW, static_cast<float>(ctx.height - ctx.rulerHeight));
        }
    }
}

void PianoRollRenderer::drawWaveform(juce::Graphics& g,
                                     const RenderContext& ctx,
                                     const ContentRenderItem& item)
{
    if (item.audioBuffer == nullptr || item.waveformSnapshot.peaks.empty())
        return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    const int startX = visibleWindow.viewportStartX;
    const int endX = visibleWindow.viewportEndX;
    const int w = endX - startX;
    if (w <= 0) return;

    const auto& level = item.waveformSnapshot;
    
    if (level.peaks.empty())
        return;

    const double contentVisibleDuration = visibleWindow.visibleContentEndTime - visibleWindow.visibleContentStartTime;
    if (contentVisibleDuration <= 0.0) return;

    const float centerY = ctx.height / 2.0f;
    const float amplitudeScale = ctx.height / 2.0f;

    const int samplesPerPeak = level.samplesPerPeak;
    const double timePerPeak = static_cast<double>(samplesPerPeak) / WaveformMipmap::kBaseSampleRate;
    const int64_t numPeaks = static_cast<int64_t>(level.peaks.size());
    const int64_t builtPeaks = level.complete ? numPeaks : level.buildProgress;
    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
    const bool isOverdose = themeId == ThemeId::Overdose;

    juce::Path waveformPath;

    // vocal-time-stretch 搂8.5 (Phase H) 鈥?waveform stretching.
    // Invert the output 鈫?source mapping (tau_inverse) so the screen X axis
    // (output time) reads from the SOURCE peaks at the tau-inverted time.
    jassert(ctx.timeGridSnapshot != nullptr);

    for (int x = startX; x < endX; ++x)
    {
        double matTime = item.projection.projectTimelineTimeToContent(ctx.coords.xToTime(x));
        matTime = ctx.timeGridSnapshot->tauInverse(matTime);

        // Aggregate all peaks covered by this pixel's time span
        double matTimeNext = item.projection.projectTimelineTimeToContent(ctx.coords.xToTime(x + 1));
        matTimeNext = ctx.timeGridSnapshot->tauInverse(matTimeNext);

        int64_t idxStart = static_cast<int64_t>(matTime / timePerPeak);
        int64_t idxEnd = static_cast<int64_t>(matTimeNext / timePerPeak);
        if (idxEnd <= idxStart)
            idxEnd = idxStart + 1;

        if (idxStart >= builtPeaks || idxStart < 0)
            continue;

        float aggMin = 0.0f;
        float aggMax = 0.0f;
        bool hasData = false;

        for (int64_t i = idxStart; i < idxEnd && i < builtPeaks; ++i)
        {
            if (i < 0) continue;
            const auto& peak = level.peaks[static_cast<std::size_t>(i)];
            if (peak.isZero()) continue;
            if (!hasData) {
                aggMin = peak.getMin();
                aggMax = peak.getMax();
                hasData = true;
            } else {
                aggMin = std::min(aggMin, peak.getMin());
                aggMax = std::max(aggMax, peak.getMax());
            }
        }

        if (!hasData)
            continue;

        const float yMin = centerY - aggMax * amplitudeScale;
        const float yMax = centerY - aggMin * amplitudeScale;
        const float fx = static_cast<float>(x) + 0.5f;

        waveformPath.startNewSubPath(fx, yMin);
        waveformPath.lineTo(fx, yMax);
    }

    if (!waveformPath.isEmpty())
    {
        if (isAurora)
        {
            const auto waveformColour = UIColors::pianoRollWaveform.brighter(0.08f);
            g.setColour(waveformColour.withAlpha(0.24f));
            g.strokePath(waveformPath, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(waveformColour.withAlpha(0.52f));
            g.strokePath(waveformPath, juce::PathStrokeType(1.25f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else if (isBlueBreeze || isOverdose)
        {
            const auto waveformColour = UIColors::pianoRollWaveform;
            g.setColour(waveformColour.withAlpha(0.13f));
            g.strokePath(waveformPath, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(waveformColour.withAlpha(0.24f));
            g.strokePath(waveformPath, juce::PathStrokeType(1.05f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else
        {
            g.setColour(UIColors::waveformFill.withAlpha(0.20f));
            g.strokePath(waveformPath, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
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
    const bool isOverdose = UIColors::currentThemeId() == ThemeId::Overdose;
    const bool isLightTheme = isBlueBreeze || isOverdose;
    const float outOfScaleDimAmount = isLightTheme ? 0.06f : kOutOfScaleDimAmount;
    const auto& overdoseAtlas = UiAssets::get(UiAssetId::PianoKeyAtlas);
    const bool hasOverdoseAtlas = isOverdose && overdoseAtlas.isValid() && overdoseAtlas.getHeight() >= 5;
    const int overdoseAtlasSliceHeight = hasOverdoseAtlas ? overdoseAtlas.getHeight() / 5 : 0;

    const auto overdoseAtlasSliceIndex = [](int noteInOctave) noexcept -> int {
        switch (noteInOctave)
        {
            case 1:  return 0;
            case 3:  return 1;
            case 6:  return 2;
            case 8:  return 3;
            case 10: return 4;
            default: return 0;
        }
    };

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

    g.setColour(isLightTheme ? UIColors::keyBedWhite : UIColors::backgroundDark);
    g.fillRect(0, 0, w, height);

    juce::Colour cWhite1 = isLightTheme ? UIColors::keyBedWhite : juce::Colour(0xFFF7F9F9);
    juce::Colour cWhite2 = isOverdose ? juce::Colour { Overdose::Colors::KeyBedWhiteBottom }
                          : (isBlueBreeze ? juce::Colour { BlueBreeze::Colors::KeyBedBottom } : juce::Colour(0xFFECF0F1));

    juce::Colour cBlackBottom = isOverdose ? juce::Colour { Overdose::Colors::KeyBedBlackBottom }
                             : (isBlueBreeze ? juce::Colour { BlueBreeze::Colors::KeyBlackBottom } : juce::Colour(0xFF1B2026));

    juce::Colour keyPressedGlowColor = isOverdose ? juce::Colour { Overdose::Colors::KeyPressedGlow }
                                    : (isBlueBreeze ? juce::Colour { BlueBreeze::Colors::KeyPressedGlow } : juce::Colour(0x500078D7));

    for (int midi = static_cast<int>(ctx.minMidi); midi <= static_cast<int>(ctx.maxMidi); ++midi)
    {
        int drawMidi = midi;
        float y = ctx.coords.midiToY(static_cast<float>(drawMidi));
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
                                       whiteB.interpolatedWith(UIColors::keyBedDivider, isLightTheme ? 0.08f : 0.0f),
                                      static_cast<float>(0 + w),
                                      y + drawH,
                                      false);
            if (isLightTheme)
                grad.addColour(0.16, whiteA.interpolatedWith(juce::Colours::white, 0.12f));
            g.setGradientFill(grad);
            g.fillRect(keyRect);

            // Scale highlight overlay on in-scale white keys
            if (inScale && ctx.scaleType != kScaleTypeChromatic)
            {
                g.setColour(isLightTheme ? UIColors::scaleHighlight.withMultipliedAlpha(0.18f) : UIColors::scaleHighlight);
                g.fillRect(keyRect);
            }

            // Pressed key highlight
            if (drawMidi == ctx.pressedPianoKey)
            {
                g.setColour(isLightTheme ? keyPressedGlowColor.withAlpha(0.22f)
                                         : juce::Colour(0x500078D7));
                g.fillRect(keyRect);
            }

            if (isLightTheme)
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

                const juce::Colour noteLabelColour = isLightTheme
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
                                      extensionB.interpolatedWith(UIColors::keyBedDivider, isLightTheme ? 0.08f : 0.0f),
                                      static_cast<float>(0 + w),
                                      y + drawH,
                                      false);
            g.setGradientFill(grad);
            g.fillRect(extensionRect);

            // Scale highlight overlay on in-scale black key extension area
            if (inScale && ctx.scaleType != kScaleTypeChromatic)
            {
                g.setColour(isLightTheme ? UIColors::scaleHighlight.withMultipliedAlpha(0.18f) : UIColors::scaleHighlight);
                g.fillRect(extensionRect);
            }

            g.setColour(UIColors::panelBorder.withAlpha(isLightTheme ? 0.22f : 1.0f));
            g.drawLine(blackKeyW, y + h * 0.5f, static_cast<float>(w), y + h * 0.5f, 1.0f);
        }
    }

    for (int midi = static_cast<int>(ctx.minMidi); midi <= static_cast<int>(ctx.maxMidi); ++midi)
    {
        int drawMidi = midi;
        float y = ctx.coords.midiToY(static_cast<float>(drawMidi));
        float h = ctx.pixelsPerSemitone;
        if (y < -50.0f || y > height + 50.0f) continue;

        int noteInOctave = drawMidi % 12;
        if (noteInOctave == 5 || noteInOctave == 0)
        {
            g.setColour(isLightTheme ? UIColors::keyBedDivider.withAlpha(0.36f) : UIColors::panelBorder);
            g.drawLine(0.0f, y + h, static_cast<float>(w), y + h, 1.0f);
        }
    }

    for (int midi = static_cast<int>(ctx.minMidi); midi <= static_cast<int>(ctx.maxMidi); ++midi)
    {
        int drawMidi = midi;
        float y = ctx.coords.midiToY(static_cast<float>(drawMidi));
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
            const bool useOverdoseAtlas = hasOverdoseAtlas && overdoseAtlasSliceHeight > 0;

            if (useOverdoseAtlas)
            {
                const auto sliceIndex = overdoseAtlasSliceIndex(noteInOctave);
                UiAssets::drawAssetSliceStretch(g,
                                               UiAssetId::PianoKeyAtlas,
                                               keyRect,
                                               { 0, sliceIndex * overdoseAtlasSliceHeight,
                                                 overdoseAtlas.getWidth(), overdoseAtlasSliceHeight });
            }
            else
            {
                juce::DropShadow ds;
                ds.colour = juce::Colours::black.withAlpha(isLightTheme ? 0.28f : 0.25f);
                ds.radius = isLightTheme ? 6 : 5;
                ds.offset = {0, 1};

                juce::Path shadowPath;
                shadowPath.addRoundedRectangle(keyRect, 2.0f);
                ds.drawForPath(g, shadowPath);

                juce::ColourGradient sideShadow(juce::Colours::black.withAlpha(isLightTheme ? 0.12f : 0.2f), blackKeyW, keyY,
                                                juce::Colours::transparentBlack, blackKeyW + 1.25f, keyY, false);
                g.setGradientFill(sideShadow);
                g.fillRect(static_cast<int>(blackKeyW), static_cast<int>(keyY + 1.0f),
                           static_cast<int>(1.25f), static_cast<int>(keyH - 1.0f));

                juce::Colour cTop = isLightTheme ? UIColors::keyBedBlack : juce::Colour(0xFF34495E);
                juce::Colour cBottom = cBlackBottom;
                if (!inScale)
                {
                    cTop = cTop.darker(outOfScaleDimAmount);
                    cBottom = cBottom.darker(outOfScaleDimAmount);
                }

                juce::ColourGradient grad(cTop, 0.0f, keyRect.getY(),
                                          cBottom, 0.0f, keyRect.getBottom(), false);
                g.setGradientFill(grad);
                g.fillRoundedRectangle(keyRect, 2.0f);

                if (isLightTheme)
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

                g.setColour(juce::Colours::white.withAlpha(isLightTheme ? 0.14f : 0.2f));
                g.fillRect(keyRect.getX() + 2.0f, keyRect.getY(), keyRect.getWidth() - 4.0f, keyH * 0.15f);

                g.setColour(juce::Colours::black.withAlpha(isLightTheme ? 0.50f : 0.6f));
                g.drawRoundedRectangle(keyRect.reduced(0.5f), 2.0f, 1.0f);
            }

            // Scale highlight overlay on in-scale black keys (reduced alpha)
            if (inScale && ctx.scaleType != kScaleTypeChromatic)
            {
                g.setColour(isLightTheme ? UIColors::scaleHighlight.withMultipliedAlpha(0.22f)
                                         : UIColors::scaleHighlight.withMultipliedAlpha(0.5f));
                g.fillRoundedRectangle(keyRect, 2.0f);
            }

            // Pressed key highlight for black keys
            if (drawMidi == ctx.pressedPianoKey)
            {
                g.setColour(isLightTheme ? keyPressedGlowColor.withAlpha(0.30f)
                                         : juce::Colour(0x500078D7));
                g.fillRoundedRectangle(keyRect, 2.0f);
            }

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

    g.setColour(isLightTheme ? UIColors::keyBedDivider.withAlpha(0.84f) : UIColors::panelBorder);
    g.drawVerticalLine(w, 0.0f, static_cast<float>(height));
}

void PianoRollRenderer::drawNotes(juce::Graphics& g,
                                  const RenderContext& ctx,
                                  const ContentRenderItem& item)
{
    const auto& notes = item.displayNotes;
    if (notes.empty()) return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    auto firstVisibleNote = std::lower_bound(
        notes.begin(),
        notes.end(),
        visibleWindow.visibleContentStartTime,
        [](const Note& note, double visibleContentStartTime) {
            return note.startTime < visibleContentStartTime;
        });

    if (firstVisibleNote != notes.begin())
    {
        const auto previousVisibleNote = std::prev(firstVisibleNote);
        if (previousVisibleNote->endTime > visibleWindow.visibleContentStartTime)
            firstVisibleNote = previousVisibleNote;
    }

    const auto lastVisibleNote = std::lower_bound(
        firstVisibleNote,
        notes.end(),
        visibleWindow.visibleContentEndTime,
        [](const Note& note, double visibleContentEndTime) {
            return note.startTime < visibleContentEndTime;
        });

    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
    const bool isOverdose = themeId == ThemeId::Overdose;

    for (auto noteIt = firstVisibleNote; noteIt != lastVisibleNote; ++noteIt)
    {
        const auto& note = *noteIt;
        float adjustedPitch = note.getAdjustedPitch();
        if (adjustedPitch <= 0.0f) continue;

        float midi = ctx.coords.freqToMidi(adjustedPitch);
        float y = ctx.coords.midiToY(midi) - (ctx.pixelsPerSemitone * 0.5f);
        float h = ctx.pixelsPerSemitone;

        // 搂8.5 鈥?note.startTime/endTime are SOURCE time; project through 蟿
        // so a stretched segment renders at its correct visual width.
        int x1 = sourceTimeToScreenX(note.startTime, ctx, item);
        int x2 = sourceTimeToScreenX(note.endTime,   ctx, item);
        if (x2 <= visibleWindow.viewportStartX || x1 >= visibleWindow.viewportEndX)
            continue;

        float w = std::max(1.0f, static_cast<float>(x2 - x1));
        auto noteBounds = juce::Rectangle<float>(static_cast<float>(x1), y, w, h);

        const auto noteColor = UIColors::noteBlock;

        if (isAurora)
        {
            g.setColour(noteColor.withAlpha(0.34f));
            g.fillRect(noteBounds);

            auto topSheenBounds = noteBounds.withHeight(juce::jmin(noteBounds.getHeight() * 0.42f, 7.0f));
            juce::ColourGradient topSheen(noteColor.brighter(0.58f).withAlpha(0.12f),
                                          topSheenBounds.getX(),
                                          topSheenBounds.getY(),
                                          juce::Colours::transparentWhite,
                                          topSheenBounds.getX(),
                                          topSheenBounds.getBottom(),
                                          false);
            g.setGradientFill(topSheen);
            g.fillRect(topSheenBounds);

            g.setColour(UIColors::noteBlockBorder.withAlpha(0.56f));
            g.drawRect(noteBounds, 1.0f);

            g.setColour(UIColors::glassHighlight.withAlpha(0.14f));
            g.drawLine(noteBounds.getX() + 1.0f,
                       noteBounds.getY() + 1.0f,
                       noteBounds.getRight() - 1.0f,
                       noteBounds.getY() + 1.0f,
                       1.0f);
        }
        else if (isBlueBreeze || isOverdose)
        {
            g.setColour(noteColor.withAlpha(0.28f));
            g.fillRect(noteBounds);

            auto topSheenBounds = noteBounds.withHeight(juce::jmin(noteBounds.getHeight() * 0.42f, 6.0f));
            juce::ColourGradient topSheen(noteColor.brighter(0.42f).withAlpha(0.10f),
                                          topSheenBounds.getX(),
                                          topSheenBounds.getY(),
                                          juce::Colours::transparentWhite,
                                          topSheenBounds.getX(),
                                          topSheenBounds.getBottom(),
                                          false);
            g.setGradientFill(topSheen);
            g.fillRect(topSheenBounds);

            g.setColour(UIColors::noteBlockBorder.withAlpha(0.08f));
            g.drawRect(noteBounds.expanded(1.0f, 0.5f), 2.0f);

            g.setColour(UIColors::noteBlockBorder.withAlpha(0.48f));
            g.drawRect(noteBounds, 0.9f);
        }
        else
        {
            g.setColour(noteColor.withAlpha(0.30f));
            g.fillRect(noteBounds);

            g.setColour(UIColors::noteBlockBorder.withAlpha(0.50f));
            g.drawRect(noteBounds, 1.0f);
        }
    }
}

void PianoRollRenderer::drawSelectedNoteHighlights(juce::Graphics& g,
                                                    const RenderContext& ctx,
                                                    const std::vector<Note>& notes,
                                                    const std::vector<int>& selectedNoteIndices,
                                                    const ContentRenderItem& item)
{
    if (notes.empty() || selectedNoteIndices.empty())
        return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;

    for (int idx : selectedNoteIndices)
    {
        if (idx < 0 || idx >= static_cast<int>(notes.size()))
            continue;

        const auto& note = notes[static_cast<size_t>(idx)];
        float adjustedPitch = note.getAdjustedPitch();
        if (adjustedPitch <= 0.0f)
            continue;

        float midi = ctx.coords.freqToMidi(adjustedPitch);
        float y = ctx.coords.midiToY(midi) - (ctx.pixelsPerSemitone * 0.5f);
        float h = ctx.pixelsPerSemitone;

        int x1 = sourceTimeToScreenX(note.startTime, ctx, item);
        int x2 = sourceTimeToScreenX(note.endTime, ctx, item);
        if (x2 <= visibleWindow.viewportStartX || x1 >= visibleWindow.viewportEndX)
            continue;

        // Clip to content viewport to prevent drawing into piano key area
        x1 = juce::jmax(x1, visibleWindow.viewportStartX);
        x2 = juce::jmin(x2, visibleWindow.viewportEndX);

        float w = std::max(1.0f, static_cast<float>(x2 - x1));
        auto noteBounds = juce::Rectangle<float>(static_cast<float>(x1), y, w, h);

        // Selection highlight: semi-transparent tint + brighter border
        g.setColour(UIColors::noteBlockSelected.withAlpha(isAurora ? 0.15f : 0.12f));
        g.fillRect(noteBounds);

        g.setColour(UIColors::noteBlockSelected.withAlpha(isAurora ? 0.72f : 0.60f));
        g.drawRect(noteBounds, isAurora ? 1.35f : 1.1f);
    }
}

void PianoRollRenderer::drawGhostNotes(juce::Graphics& g, const RenderContext& ctx, const ReferenceOverlay& overlay)
{
    if (overlay.ghostNotes.empty())
        return;

    static constexpr float kDashLengths[] = { 2.0f, 4.0f };
    const juce::Colour fillColour = overlay.ghostColour.withMultipliedAlpha(overlay.ghostOpacity);
    const juce::Colour borderColour = overlay.ghostColour.withMultipliedAlpha(overlay.ghostOpacity * 0.7f);

    ContentRenderItem overlayItem;
    overlayItem.projection = overlay.sourceProjection;

    for (const auto& note : overlay.ghostNotes)
    {
        const float adjustedPitch = note.getAdjustedPitch();
        if (adjustedPitch <= 0.0f)
            continue;

        const int x1 = sourceTimeToScreenX(note.startTime, ctx, overlayItem);
        const int x2 = sourceTimeToScreenX(note.endTime, ctx, overlayItem);
        if (x2 <= ctx.pianoKeyWidth || x1 >= ctx.width)
            continue;

        const float midi = ctx.coords.freqToMidi(adjustedPitch);
        const float y = ctx.coords.midiToY(midi) - (ctx.pixelsPerSemitone * 0.5f);
        const float w = std::max(1.0f, static_cast<float>(x2 - x1));
        const float h = ctx.pixelsPerSemitone;
        const auto noteBounds = juce::Rectangle<float>(static_cast<float>(x1), y, w, h);

        g.setColour(fillColour);
        g.fillRect(noteBounds);

        g.setColour(borderColour);
        g.drawDashedLine(
            juce::Line<float>(noteBounds.getX(), noteBounds.getY(),
                              noteBounds.getRight(), noteBounds.getY()),
            kDashLengths, 2, 1.0f);
        g.drawDashedLine(
            juce::Line<float>(noteBounds.getX(), noteBounds.getBottom(),
                              noteBounds.getRight(), noteBounds.getBottom()),
            kDashLengths, 2, 1.0f);
        g.drawDashedLine(
            juce::Line<float>(noteBounds.getX(), noteBounds.getY(),
                              noteBounds.getX(), noteBounds.getBottom()),
            kDashLengths, 2, 1.0f);
        g.drawDashedLine(
            juce::Line<float>(noteBounds.getRight(), noteBounds.getY(),
                              noteBounds.getRight(), noteBounds.getBottom()),
            kDashLengths, 2, 1.0f);
    }
}

void PianoRollRenderer::drawGhostAnchors(juce::Graphics& g, const RenderContext& ctx, const ReferenceOverlay& overlay)
{
    if (overlay.ghostAnchors.empty())
        return;

    static constexpr float kDashLengths[] = { 2.0f, 4.0f };
    const float yTop = ctx.coords.midiToY(ctx.minMidi);
    const float yBottom = ctx.coords.midiToY(ctx.maxMidi);

    ContentRenderItem overlayItem;
    overlayItem.projection = overlay.sourceProjection;

    for (const auto& anchor : overlay.ghostAnchors)
    {
        const int x = sourceTimeToScreenX(anchor.sourceSeconds, ctx, overlayItem);
        if (x < ctx.pianoKeyWidth || x >= ctx.width)
            continue;

        const float alpha = overlay.ghostOpacity * std::min(1.0f, anchor.strength);
        g.setColour(overlay.ghostColour.withMultipliedAlpha(alpha));
        g.drawDashedLine(
            juce::Line<float>(static_cast<float>(x), yTop,
                              static_cast<float>(x), yBottom),
            kDashLengths, 2, 1.0f);
    }
}

// ============================================================================
// TimeGrid Anchors (cached slot — neutral lines, no interaction)
// ============================================================================
void PianoRollRenderer::drawTimeGridAnchors(juce::Graphics& g, const RenderContext& ctx)
{
    if (ctx.timeGridSnapshot == nullptr) return;

    const int contentTop    = ctx.rulerHeight;
    const int contentBottom = ctx.height;
    if (contentBottom <= contentTop) return;

    for (const auto& h : ctx.timeGridSnapshot->handles()) {
        const double timelineTime = ctx.activeProjection.projectContentTimeToTimeline(h.output_seconds);
        const int x = ctx.coords.timeToX(timelineTime);
        if (x < ctx.pianoKeyWidth || x >= ctx.width) continue;

        const float alpha = h.locked ? 0.3f : 0.4f;
        g.setColour(juce::Colours::white.withAlpha(alpha));
        g.drawLine(static_cast<float>(x),
                   static_cast<float>(contentTop),
                   static_cast<float>(x),
                   static_cast<float>(contentBottom),
                   1.0f);
    }
}

// ============================================================================
// TimeGrid Handles (overlay — hover/selected/drag affordances)
// ============================================================================
void PianoRollRenderer::drawTimeGridHandles(juce::Graphics& g, const RenderContext& ctx)
{
    if (ctx.timeGridSnapshot == nullptr) return;
    if (!ctx.isTimeView()) return;

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
            case HandleKind::ReferenceAuto: return juce::Colour::fromRGB(255, 196, 87);
        }
        return juce::Colours::white;
    };

    const juce::Colour kHighConfidenceColour = juce::Colour::fromRGB(0xE0, 0xB0, 0x40);

    for (const auto& h : ctx.timeGridSnapshot->handles()) {
        const bool selected = (ctx.timeGridSelectedHandleId == h.id);
        const bool hovered  = (ctx.timeGridHoveredHandleId  == h.id);
        const bool isAdditional = std::find(ctx.additionalSelectedHandleIds.begin(), ctx.additionalSelectedHandleIds.end(), h.id) != ctx.additionalSelectedHandleIds.end();
        if (!selected && !hovered && !isAdditional) continue;

        const double timelineTime = ctx.activeProjection.projectContentTimeToTimeline(h.output_seconds);
        const int x = ctx.coords.timeToX(timelineTime);
        if (x < ctx.pianoKeyWidth || x >= ctx.width) continue;

        juce::Colour col = colorForKind(h.kind);
        if (h.locked) {
            col = col.withAlpha(0.45f);
        } else if (h.confidence == Confidence::High) {
            col = kHighConfidenceColour;
        }

        const bool isHigh = (!h.locked && h.confidence == Confidence::High);
        const float baseThickness = isHigh ? 1.5f : 1.0f;
        const float lineThickness = (selected ? 2.0f : (hovered ? 1.5f : baseThickness));

        g.setColour(col.withMultipliedAlpha(selected ? 1.0f : (hovered ? 0.85f : 0.65f)));
        g.drawLine(static_cast<float>(x),
                   static_cast<float>(contentTop),
                   static_cast<float>(x),
                   static_cast<float>(contentBottom),
                   lineThickness);

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

void PianoRollRenderer::drawF0Curve(juce::Graphics& g,
                                     const RenderContext& ctx,
                                     const ContentRenderItem& item)
{
    if (item.f0LOD == nullptr || item.f0Timeline.isEmpty())
        return;

    if (item.f0LOD->isEmpty())
        return;

    if (!ctx.showOriginalF0 && !ctx.showCorrectedF0)
        return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    // 选择当前缩放级别的最佳 LOD
    const auto& lodLevel = item.f0LOD->selectBestLevel(
        ctx.pixelsPerSecond,
        item.pitchSnapshot->getSampleRate(),
        item.pitchSnapshot->getHopSize());

    // 绘制 voiced runs 的辅助函数
    auto drawVoicedRuns = [&](const std::vector<F0VisualLOD::VoicedRun>& runs,
                              juce::Colour colour, float strokeWidth) {
        juce::Path path;
        for (const auto& run : runs) {
            if (!run.isValid()) continue;
            if (run.f0Values.empty()) continue;

            bool subPathStarted = false;
            for (size_t i = 0; i < run.f0Values.size(); ++i) {
                const int frame = run.startFrame + static_cast<int>(i) * lodLevel.framesPerPoint;
                const float f0 = run.f0Values[i];
                if (f0 <= 0.0f) continue;

                const double timePos = item.f0Timeline.timeAtFrame(frame);
                const int x = sourceTimeToScreenX(timePos, ctx, item);
                if (x < ctx.pianoKeyWidth || x >= ctx.width) continue;

                const float y = ctx.coords.freqToY(f0);
                if (!subPathStarted) {
                    path.startNewSubPath(static_cast<float>(x), y);
                    subPathStarted = true;
                } else {
                    path.lineTo(static_cast<float>(x), y);
                }
            }
        }
        if (!path.isEmpty()) {
            g.setColour(colour);
            g.strokePath(path, juce::PathStrokeType(strokeWidth,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    };

    // Draw original F0
    if (ctx.showOriginalF0) {
        drawVoicedRuns(lodLevel.originalVoicedRuns,
                       UIColors::originalF0.withAlpha(0.35f), 1.2f);
    }

    // Draw corrected F0 segments
    if (ctx.showCorrectedF0 && !lodLevel.correctedVoicedRuns.empty()) {
        drawVoicedRuns(lodLevel.correctedVoicedRuns,
                       UIColors::correctedF0.withAlpha(0.85f), 1.8f);
    }
}

} // namespace OpenTune
