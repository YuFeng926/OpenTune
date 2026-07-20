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

    // vocal-time-stretch 搂8.5 擂8.5 — projectTimelineTimeToContent returns OUTPUT time
    // inside the content, but Notes / PitchCurve / F0 timeline / WaveformMipmap
    // are all indexed by SOURCE time. Convert to source time via tauInverse.
    jassert(item.timeGrid);
    window.visibleContentStartTime = item.timeGrid->tauInverse(window.visibleContentStartTime);
    window.visibleContentEndTime   = item.timeGrid->tauInverse(window.visibleContentEndTime);
    return window;
}

static float clampF0VisualAlpha(float alpha) noexcept
{
    return juce::jlimit(0.0f, 1.0f, alpha);
}

static float calculateF0VisualEnergyAlpha(float energy,
                                          float minEnergy,
                                          float maxEnergy) noexcept
{
    static constexpr float kMinEnergyAlpha = 0.70f;
    static constexpr float kMaxEnergyAlpha = 1.00f;

    if (!std::isfinite(energy) || maxEnergy <= minEnergy + std::numeric_limits<float>::epsilon()) {
        return kMaxEnergyAlpha;
    }

    const float normalized = juce::jlimit(0.0f, 1.0f, (energy - minEnergy) / (maxEnergy - minEnergy));
    return kMinEnergyAlpha + (kMaxEnergyAlpha - kMinEnergyAlpha) * normalized;
}

static float f0VisualTargetPointSpacing(double framePixelSpacing) noexcept
{
    if (framePixelSpacing >= 1.05) {
        return 0.0f;
    }

    if (framePixelSpacing >= 0.50) {
        return 1.0f;
    }

    return 1.5f;
}

static void appendSmoothedF0Path(juce::Path& path,
                                 const std::vector<PianoRollRenderer::F0VisualPoint>& points,
                                 std::size_t startIndex,
                                 std::size_t endIndexInclusive)
{
    if (points.empty() || startIndex >= points.size()) {
        return;
    }

    endIndexInclusive = std::min(endIndexInclusive, points.size() - 1);
    if (endIndexInclusive <= startIndex) {
        const auto& point = points[startIndex];
        path.startNewSubPath(point.x - 0.01f, point.y);
        path.lineTo(point.x + 0.01f, point.y);
        return;
    }

    path.startNewSubPath(points[startIndex].x, points[startIndex].y);
    for (std::size_t i = startIndex + 1; i < endIndexInclusive; ++i) {
        const auto& control = points[i];
        const auto& next = points[i + 1];
        path.quadraticTo(control.x,
                         control.y,
                         (control.x + next.x) * 0.5f,
                         (control.y + next.y) * 0.5f);
    }

    const auto& last = points[endIndexInclusive];
    path.lineTo(last.x, last.y);
}

// vocal-time-stretch 搂8.5 鈥?convert a SOURCE-time anchor (Note.startTime,
// f0Timeline frame timestamp, WaveformMipmap peak) into screen X via the
// item's projection. Identity TimeGrid 鈫?degenerates to existing pipeline.
inline int sourceTimeToScreenX(double sourceTime,
                                const PianoRollRenderer::RenderContext& ctx,
                                const PianoRollRenderer::ContentRenderItem& item)
{
    jassert(item.timeGrid);
    const double outputTime = item.timeGrid->tauForward(sourceTime);
    const double timelineTime = item.projection.projectContentTimeToTimeline(outputTime);
    return ctx.coords.timeToX(timelineTime);
}

} // namespace

std::vector<PianoRollRenderer::F0VisualSegment> PianoRollRenderer::buildF0VisualSegments(
    const std::vector<float>& f0,
    const std::vector<float>* originalEnergy,
    const F0VisualBuildOptions& options,
    const F0FrameToX& frameToX,
    const F0FrameToY& frameToY)
{
    std::vector<F0VisualSegment> segments;
    if (f0.empty() || !frameToX || !frameToY) {
        return segments;
    }

    const int startFrame = juce::jlimit(0, static_cast<int>(f0.size()), options.startFrame);
    const int endFrameExclusive = juce::jlimit(startFrame, static_cast<int>(f0.size()), options.endFrameExclusive);
    if (endFrameExclusive <= startFrame) {
        return segments;
    }

    const bool hasEnergy = originalEnergy != nullptr && originalEnergy->size() == f0.size();

    float minEnergy = std::numeric_limits<float>::max();
    float maxEnergy = std::numeric_limits<float>::lowest();
    if (hasEnergy) {
        for (int frame = startFrame; frame < endFrameExclusive; ++frame) {
            const float frequency = f0[static_cast<std::size_t>(frame)];
            if (frequency < 20.0f || frequency > 2000.0f) {
                continue;
            }

            const float energy = (*originalEnergy)[static_cast<std::size_t>(frame)];
            if (std::isfinite(energy)) {
                minEnergy = std::min(minEnergy, energy);
                maxEnergy = std::max(maxEnergy, energy);
            }
        }
    }

    const float targetPointSpacing = f0VisualTargetPointSpacing(options.pixelsPerSecond * options.secondsPerFrame);

    struct BucketAccumulator {
        bool active = false;
        int frame = 0;
        float xSum = 0.0f;
        float ySum = 0.0f;
        float alphaSum = 0.0f;
        float weightSum = 0.0f;

        void clear() noexcept
        {
            active = false;
            frame = 0;
            xSum = 0.0f;
            ySum = 0.0f;
            alphaSum = 0.0f;
            weightSum = 0.0f;
        }
    };

    F0VisualSegment currentSegment;
    BucketAccumulator bucket;
    float bucketAnchorX = 0.0f;

    auto flushBucket = [&]() {
        if (!bucket.active || bucket.weightSum <= 0.0f) {
            bucket.clear();
            return;
        }

        currentSegment.points.push_back({
            bucket.frame,
            bucket.xSum / bucket.weightSum,
            bucket.ySum / bucket.weightSum,
            clampF0VisualAlpha(bucket.alphaSum / bucket.weightSum)
        });
        bucket.clear();
    };

    auto flushSegment = [&]() {
        flushBucket();
        if (!currentSegment.points.empty()) {
            segments.push_back(std::move(currentSegment));
            currentSegment = {};
        }
    };

    for (int frame = startFrame; frame < endFrameExclusive; ++frame) {
        const float frequency = f0[static_cast<std::size_t>(frame)];
        if (frequency < 20.0f || frequency > 2000.0f) {
            flushSegment();
            continue;
        }

        const float x = frameToX(frame);
        if (x < static_cast<float>(options.viewportStartX) || x > static_cast<float>(options.viewportEndX)) {
            flushSegment();
            continue;
        }

        const float y = frameToY(frame, frequency);
        const float energyAlpha = hasEnergy
            ? calculateF0VisualEnergyAlpha((*originalEnergy)[static_cast<std::size_t>(frame)], minEnergy, maxEnergy)
            : 1.0f;
        const float weight = juce::jmax(0.001f, energyAlpha);

        if (targetPointSpacing <= 0.0f) {
            flushBucket();
            currentSegment.points.push_back({ frame, x, y, energyAlpha });
            continue;
        }

        if (!bucket.active) {
            bucket.active = true;
            bucket.frame = frame;
            bucketAnchorX = x;
            bucket.weightSum = weight;
            bucket.xSum = x * weight;
            bucket.ySum = y * weight;
            bucket.alphaSum = energyAlpha * weight;
            continue;
        }

        if (std::abs(x - bucketAnchorX) < targetPointSpacing) {
            bucket.frame = frame;
            bucket.weightSum += weight;
            bucket.xSum += x * weight;
            bucket.ySum += y * weight;
            bucket.alphaSum += energyAlpha * weight;
            continue;
        }

        flushBucket();

        bucket.active = true;
        bucket.frame = frame;
        bucketAnchorX = x;
        bucket.weightSum = weight;
        bucket.xSum = x * weight;
        bucket.ySum = y * weight;
        bucket.alphaSum = energyAlpha * weight;
    }

    flushSegment();

    return segments;
}

void PianoRollRenderer::drawUnvoicedFrameBands(juce::Graphics& g,
                                               const RenderContext& ctx,
                                               const ContentRenderItem& item)
{
    if (!ctx.showUnvoicedFrames || item.f0Timeline.isEmpty())
        return;
    if (!item.pitchSnapshot || item.pitchSnapshot->size() == 0)
        return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    const auto& originalF0 = item.pitchSnapshot->getOriginalF0();
    if (originalF0.empty())
        return;

    const auto bandColour = UIColors::currentThemeId() == ThemeId::DarkBlueGrey
        ? UIColors::backgroundDark.withAlpha(0.28f)
        : UIColors::backgroundMedium.withAlpha(0.22f);
    g.setColour(bandColour);

    // Scan originalF0 for unvoiced intervals directly
    int unvoicedStart = -1;
    const int totalFrames = static_cast<int>(originalF0.size());
    for (int frame = 0; frame < totalFrames; ++frame) {
        const bool isVoiced = originalF0[static_cast<std::size_t>(frame)] > 0.0f;
        if (isVoiced) {
            if (unvoicedStart >= 0) {
                // End unvoiced interval
                const double intervalStartTime = item.f0Timeline.timeAtFrame(unvoicedStart);
                const double intervalEndTime = item.f0Timeline.timeAtFrame(frame);

                if (intervalEndTime > visibleWindow.visibleContentStartTime &&
                    intervalStartTime < visibleWindow.visibleContentEndTime) {
                    const int x1 = sourceTimeToScreenX(intervalStartTime, ctx, item);
                    const int x2 = sourceTimeToScreenX(intervalEndTime, ctx, item);

                    if (x2 > ctx.pianoKeyWidth && x1 < ctx.width) {
                        const float drawX = static_cast<float>(std::max(x1, ctx.pianoKeyWidth));
                        const float drawW = static_cast<float>(std::min(x2, ctx.width)) - drawX;
                        if (drawW > 0.5f) {
                            g.fillRect(drawX, static_cast<float>(ctx.rulerHeight),
                                       drawW, static_cast<float>(ctx.height - ctx.rulerHeight));
                        }
                    }
                }
                unvoicedStart = -1;
            }
        } else {
            if (unvoicedStart < 0) {
                unvoicedStart = frame;
            }
        }
    }
    // Handle trailing unvoiced interval
    if (unvoicedStart >= 0) {
        const double intervalStartTime = item.f0Timeline.timeAtFrame(unvoicedStart);
        const double intervalEndTime = item.f0Timeline.timeAtFrame(totalFrames);

        if (intervalEndTime > visibleWindow.visibleContentStartTime &&
            intervalStartTime < visibleWindow.visibleContentEndTime) {
            const int x1 = sourceTimeToScreenX(intervalStartTime, ctx, item);
            const int x2 = sourceTimeToScreenX(intervalEndTime, ctx, item);

            if (x2 > ctx.pianoKeyWidth && x1 < ctx.width) {
                const float drawX = static_cast<float>(std::max(x1, ctx.pianoKeyWidth));
                const float drawW = static_cast<float>(std::min(x2, ctx.width)) - drawX;
                if (drawW > 0.5f) {
                    g.fillRect(drawX, static_cast<float>(ctx.rulerHeight),
                               drawW, static_cast<float>(ctx.height - ctx.rulerHeight));
                }
            }
        }
    }
}

void PianoRollRenderer::drawWaveform(juce::Graphics& g,
                                      const RenderContext& ctx,
                                      const ContentRenderItem& item,
                                      const WaveformMipmap::Level& wfLevel,
                                      int wfLevelIndex)
{
    if (item.audioBuffer == nullptr || wfLevel.peaks.empty())
        return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    const int startX = visibleWindow.viewportStartX;
    const int endX = visibleWindow.viewportEndX;
    const int w = endX - startX;
    if (w <= 0) return;

    const double contentVisibleDuration = visibleWindow.visibleContentEndTime - visibleWindow.visibleContentStartTime;
    if (contentVisibleDuration <= 0.0) return;

    const float centerY = ctx.height / 2.0f;
    const float amplitudeScale = ctx.height / 2.0f;

    const int samplesPerPeak = WaveformMipmap::kSamplesPerPeak[wfLevelIndex];
    const double timePerPeak = static_cast<double>(samplesPerPeak) / WaveformMipmap::kBaseSampleRate;
    const int64_t numPeaks = static_cast<int64_t>(wfLevel.peaks.size());
    const int64_t builtPeaks = wfLevel.complete ? numPeaks : wfLevel.buildProgress;
    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
    const bool isOverdose = themeId == ThemeId::Overdose;

    juce::Path waveformPath;

    // vocal-time-stretch 搂8.5 (Phase H) 鈥?waveform stretching.
    // Invert the output 鈫?source mapping (tau_inverse) so the screen X axis
    // (output time) reads from the SOURCE peaks at the tau-inverted time.
    jassert(item.timeGrid);

    for (int x = startX; x < endX; ++x)
    {
        double matTime = item.projection.projectTimelineTimeToContent(ctx.coords.xToTime(x));
        matTime = item.timeGrid->tauInverse(matTime);

        // Aggregate all peaks covered by this pixel's time span
        double matTimeNext = item.projection.projectTimelineTimeToContent(ctx.coords.xToTime(x + 1));
        matTimeNext = item.timeGrid->tauInverse(matTimeNext);

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
            const auto& peak = wfLevel.peaks[static_cast<std::size_t>(i)];
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
            g.setColour(noteColor.withAlpha(0.90f));
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
            g.setColour(noteColor.withAlpha(0.90f));
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
            g.setColour(noteColor.withAlpha(0.90f));
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
    overlayItem.timeGrid = overlay.timeGrid;
    jassert(overlayItem.timeGrid);

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
    overlayItem.timeGrid = overlay.timeGrid;
    jassert(overlayItem.timeGrid);

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
void PianoRollRenderer::drawTimeGridAnchors(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item)
{
    jassert(item.timeGrid);

    const int contentTop    = ctx.rulerHeight;
    const int contentBottom = ctx.height;
    if (contentBottom <= contentTop) return;

    for (const auto& h : item.timeGrid->handles()) {
        const double timelineTime = item.projection.projectContentTimeToTimeline(h.output_seconds);
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
void PianoRollRenderer::drawTimeGridHandles(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item)
{
    jassert(item.timeGrid);
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

    for (const auto& h : item.timeGrid->handles()) {
        const bool selected = (ctx.timeGridSelectedHandleId == h.id);
        const bool hovered  = (ctx.timeGridHoveredHandleId  == h.id);
        const bool isAdditional = std::find(ctx.additionalSelectedHandleIds.begin(), ctx.additionalSelectedHandleIds.end(), h.id) != ctx.additionalSelectedHandleIds.end();
        if (!selected && !hovered && !isAdditional) continue;

        const double timelineTime = item.projection.projectContentTimeToTimeline(h.output_seconds);
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
    if (item.f0Timeline.isEmpty())
        return;

    if (!item.pitchSnapshot || item.pitchSnapshot->size() == 0)
        return;

    if (!ctx.showOriginalF0 && !ctx.showCorrectedF0)
        return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    const int marginFrames = 10;
    const auto visibleFrames = item.f0Timeline.rangeForTimesWithMargin(
        visibleWindow.visibleContentStartTime,
        visibleWindow.visibleContentEndTime,
        marginFrames);
    const auto iStart = std::min(static_cast<std::size_t>(visibleFrames.startFrame),
                                 item.pitchSnapshot->size());
    const auto iEnd = std::min(static_cast<std::size_t>(
        std::max(visibleFrames.startFrame, visibleFrames.endFrameExclusive)),
        item.pitchSnapshot->size());

    double secondsPerFrame = 0.01;
    if (item.f0Timeline.endFrameExclusive() > 1) {
        secondsPerFrame = item.f0Timeline.timeAtFrame(1) - item.f0Timeline.timeAtFrame(0);
    }

    F0VisualBuildOptions visualOptions;
    visualOptions.startFrame = static_cast<int>(iStart);
    visualOptions.endFrameExclusive = static_cast<int>(iEnd);
    visualOptions.viewportStartX = visibleWindow.viewportStartX;
    visualOptions.viewportEndX = visibleWindow.viewportEndX;
    visualOptions.pixelsPerSecond = ctx.pixelsPerSecond;
    visualOptions.secondsPerFrame = secondsPerFrame;

    const std::vector<float>* originalEnergy = &item.pitchSnapshot->getOriginalEnergy();
    if (originalEnergy->empty()) originalEnergy = nullptr;

    auto makeFrameToX = [&](int frame) -> float {
        return static_cast<float>(sourceTimeToScreenX(
            item.f0Timeline.timeAtFrame(frame), ctx, item));
    };

    auto makeFrameToY = [&](int, float frequency) -> float {
        return ctx.coords.freqToY(frequency);
    };

    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
    const bool isOverdose = themeId == ThemeId::Overdose;

    // Draw original F0 (thin, low alpha)
    if (ctx.showOriginalF0) {
        const auto& originalF0 = item.pitchSnapshot->getOriginalF0();
        const auto visualSegments = buildF0VisualSegments(
            originalF0, originalEnergy,
            visualOptions, makeFrameToX, makeFrameToY);

        const juce::Colour colour = UIColors::originalF0;
        const float alpha = 0.90f;

        const float lineWidth = isAurora ? 1.35f : ((isBlueBreeze || isOverdose) ? 1.15f : 1.25f);
        const juce::PathStrokeType strokeType(lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const float glowLineWidth = lineWidth + (isAurora ? 2.2f : 1.8f);
        const juce::PathStrokeType glowStrokeType(glowLineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const juce::PathStrokeType innerGlowStrokeType(lineWidth + 0.72f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const juce::PathStrokeType highlightStrokeType(juce::jmax(0.75f, lineWidth * 0.46f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        for (const auto& segment : visualSegments) {
            if (segment.points.empty()) continue;

            if (segment.points.size() == 1) {
                const auto& p = segment.points.front();
                juce::Path ptPath;
                ptPath.startNewSubPath(p.x - 0.01f, p.y);
                ptPath.lineTo(p.x + 0.01f, p.y);
                if (isAurora) {
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.080f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.22f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.96f));
                    g.strokePath(ptPath, strokeType);
                } else if (isBlueBreeze || isOverdose) {
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.055f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.14f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.96f));
                    g.strokePath(ptPath, strokeType);
                } else {
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha));
                    g.strokePath(ptPath, strokeType);
                }
                continue;
            }

            juce::Path runPath;
            appendSmoothedF0Path(runPath, segment.points, 0, segment.points.size() - 1);

            if (isAurora) {
                g.setColour(colour.withAlpha(alpha * 0.080f));
                g.strokePath(runPath, glowStrokeType);
                g.setColour(colour.withAlpha(alpha * 0.22f));
                g.strokePath(runPath, innerGlowStrokeType);
                g.setColour(colour.withAlpha(alpha * 0.96f));
                g.strokePath(runPath, strokeType);
                g.setColour(colour.brighter(0.30f).withAlpha(alpha * 0.18f));
                g.strokePath(runPath, highlightStrokeType);
            } else if (isBlueBreeze || isOverdose) {
                g.setColour(colour.withAlpha(alpha * 0.055f));
                g.strokePath(runPath, glowStrokeType);
                g.setColour(colour.withAlpha(alpha * 0.14f));
                g.strokePath(runPath, innerGlowStrokeType);
                g.setColour(colour.withAlpha(alpha * 0.96f));
                g.strokePath(runPath, strokeType);
                g.setColour(colour.brighter(0.16f).withAlpha(alpha * 0.12f));
                g.strokePath(runPath, highlightStrokeType);
            } else {
                g.setColour(colour.withAlpha(alpha));
                g.strokePath(runPath, strokeType);
            }
        }
    }

    // Draw corrected F0 (thicker, higher alpha) — lazily render correction layer
    if (ctx.showCorrectedF0 && item.pitchSnapshot->hasCorrectionLayer()) {
        const auto& originalF0 = item.pitchSnapshot->getOriginalF0();
        std::vector<float> correctedF0(originalF0.size(), 0.0f);
        item.pitchSnapshot->renderCorrectionLayerF0Range(
            visualOptions.startFrame, visualOptions.endFrameExclusive,
            [&](int frame, const float* data, int length) {
                for (int i = 0; i < length; ++i) {
                    const int f = frame + i;
                    if (f >= 0 && f < static_cast<int>(correctedF0.size()))
                        correctedF0[static_cast<std::size_t>(f)] = data[i];
                }
            });

        const auto visualSegments = buildF0VisualSegments(
            correctedF0, originalEnergy,
            visualOptions, makeFrameToX, makeFrameToY);

        const juce::Colour colour = UIColors::correctedF0;
        const float alpha = 0.85f;

        const float lineWidth = isAurora ? 2.25f : ((isBlueBreeze || isOverdose) ? 1.85f : 2.05f);
        const juce::PathStrokeType strokeType(lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const float glowLineWidth = lineWidth + (isAurora ? 2.7f : 1.8f);
        const juce::PathStrokeType glowStrokeType(glowLineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const juce::PathStrokeType innerGlowStrokeType(lineWidth + 0.95f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const juce::PathStrokeType highlightStrokeType(juce::jmax(0.75f, lineWidth * 0.46f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        for (const auto& segment : visualSegments) {
            if (segment.points.empty()) continue;

            if (segment.points.size() == 1) {
                const auto& p = segment.points.front();
                juce::Path ptPath;
                ptPath.startNewSubPath(p.x - 0.01f, p.y);
                ptPath.lineTo(p.x + 0.01f, p.y);
                if (isAurora) {
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.095f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.20f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.98f));
                    g.strokePath(ptPath, strokeType);
                } else if (isBlueBreeze || isOverdose) {
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.070f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.15f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha * 0.92f));
                    g.strokePath(ptPath, strokeType);
                } else {
                    g.setColour(colour.withAlpha(alpha * p.energyAlpha));
                    g.strokePath(ptPath, strokeType);
                }
                continue;
            }

            juce::Path runPath;
            appendSmoothedF0Path(runPath, segment.points, 0, segment.points.size() - 1);

            if (isAurora) {
                g.setColour(colour.withAlpha(alpha * 0.095f));
                g.strokePath(runPath, glowStrokeType);
                g.setColour(colour.withAlpha(alpha * 0.20f));
                g.strokePath(runPath, innerGlowStrokeType);
                g.setColour(colour.withAlpha(alpha * 0.98f));
                g.strokePath(runPath, strokeType);
                g.setColour(colour.brighter(0.20f).withAlpha(alpha * 0.15f));
                g.strokePath(runPath, highlightStrokeType);
            } else if (isBlueBreeze || isOverdose) {
                g.setColour(colour.withAlpha(alpha * 0.070f));
                g.strokePath(runPath, glowStrokeType);
                g.setColour(colour.withAlpha(alpha * 0.15f));
                g.strokePath(runPath, innerGlowStrokeType);
                g.setColour(colour.withAlpha(alpha * 0.92f));
                g.strokePath(runPath, strokeType);
                g.setColour(colour.brighter(0.16f).withAlpha(alpha * 0.12f));
                g.strokePath(runPath, highlightStrokeType);
            } else {
                g.setColour(colour.withAlpha(alpha));
                g.strokePath(runPath, strokeType);
            }
        }
    }
}

} // namespace OpenTune
