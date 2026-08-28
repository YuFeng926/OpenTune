#include "PianoRollRenderer.h"
#include "../UiAssets.h"
#include "../UIColors.h"
#include "../../../Utils/AppLogger.h"
#include "../../../Utils/NoteGeneratorTypes.h"
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

// Core: project explicit X bounds through timeline → content → tauInverse(source).
static VisibleTimeWindow computeTimeWindowFromXBounds(
    const PianoRollRenderer::RenderContext& ctx,
    const PianoRollRenderer::ContentRenderItem& item,
    int startX, int endX)
{
    VisibleTimeWindow window;
    if (!item.projection.isValid())
        return window;

    window.viewportStartX = startX;
    window.viewportEndX = endX;
    if (window.viewportEndX <= window.viewportStartX)
        return {};

    window.visibleStartTime = ctx.coords.xToTime(window.viewportStartX);
    window.visibleEndTime = ctx.coords.xToTime(window.viewportEndX);
    if (window.visibleEndTime <= window.visibleStartTime)
        return {};

    window.visibleContentStartTime = item.projection.projectTimelineTimeToContent(window.visibleStartTime);
    window.visibleContentEndTime = item.projection.projectTimelineTimeToContent(window.visibleEndTime);

    // vocal-time-stretch §8.5 — projectTimelineTimeToContent returns OUTPUT time
    // inside the content, but Notes / PitchCurve / F0 timeline / WaveformMipmap
    // are all indexed by SOURCE time. Convert to source time via tauInverse.
    jassert(item.timeGrid);
    window.visibleContentStartTime = item.timeGrid->tauInverse(window.visibleContentStartTime);
    window.visibleContentEndTime   = item.timeGrid->tauInverse(window.visibleContentEndTime);
    return window;
}

// Damage-aware window for waveform/notes/unvoiced/anchors.
VisibleTimeWindow computeVisibleTimeWindow(const PianoRollRenderer::RenderContext& ctx,
                                            const PianoRollRenderer::ContentRenderItem& item)
{
    const int startX = ctx.rasterBounds.isEmpty()
        ? ctx.pianoKeyWidth
        : std::max(ctx.rasterBounds.getX(), ctx.pianoKeyWidth);
    const int endX = ctx.rasterBounds.isEmpty()
        ? ctx.width
        : std::min(ctx.rasterBounds.getRight(), ctx.width);
    return computeTimeWindowFromXBounds(ctx, item, startX, endX);
}

// Full-viewport window for F0 (always [pianoKeyWidth, ctx.width]).
VisibleTimeWindow computeFullViewportTimeWindow(const PianoRollRenderer::RenderContext& ctx,
                                                 const PianoRollRenderer::ContentRenderItem& item)
{
    return computeTimeWindowFromXBounds(ctx, item, ctx.pianoKeyWidth, ctx.width);
}

struct F0VisualPoint
{
    float x = 0.0f;
    float y = 0.0f;
    float levelHotMix = 0.0f;
};

struct F0VisualSegment
{
    std::vector<F0VisualPoint> points;
    bool useLinearPath = false;
};

struct F0VisualBuildOptions
{
    double pixelsPerSecond = 100.0;
    double secondsPerFrame = 0.01;
};

static float smootherStep(float value) noexcept
{
    const float t = juce::jlimit(0.0f, 1.0f, value);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
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
                                 const std::vector<F0VisualPoint>& points,
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

static void appendLinearF0Path(juce::Path& path,
                               const std::vector<F0VisualPoint>& points,
                               std::size_t startIndex,
                               std::size_t endIndexInclusive)
{
    if (points.empty() || startIndex >= points.size())
        return;

    endIndexInclusive = std::min(endIndexInclusive, points.size() - 1);
    if (endIndexInclusive <= startIndex) {
        const auto& point = points[startIndex];
        path.startNewSubPath(point.x - 0.01f, point.y);
        path.lineTo(point.x + 0.01f, point.y);
        return;
    }

    path.startNewSubPath(points[startIndex].x, points[startIndex].y);
    for (std::size_t i = startIndex + 1; i <= endIndexInclusive; ++i) {
        path.lineTo(points[i].x, points[i].y);
    }
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

// OpenDyne waveform blob：按 Note 的 F0Timeline 帧范围索引持久 originalEnergy，构建闭合波形 Path。
// drawNotes 与选中高亮共用，保证 blob 几何完全一致。
static bool buildNoteBlobPath(
    const Note& note,
    const PianoRollRenderer::RenderContext& ctx,
    const PianoRollRenderer::ContentRenderItem& item,
    const std::vector<float>& energy,
    const F0Timeline& timeline,
    float clipRefMag,
    const AutomationLane* volumeEnvelope,
    float centerY,
    float halfH,
    int x1,
    int x2,
    juce::Path& outPath)
{
    const auto range = timeline.rangeForTimes(note.startTime, note.endTime);
    if (range.isEmpty())
        return false;

    juce::Path blob;
    blob.startNewSubPath(static_cast<float>(x1), centerY);

    for (int frame = range.startFrame; frame < range.endFrameExclusive; ++frame)
    {
        const float normMag = energy[static_cast<size_t>(frame)] / clipRefMag;
        const double sourceTime = timeline.timeAtFrame(frame);
        const float px = static_cast<float>(juce::jlimit(x1, x2, sourceTimeToScreenX(sourceTime, ctx, item)));
        const float gainDb = volumeEnvelope ? volumeEnvelope->evalAt(sourceTime) : 0.0f;
        // 视觉范围 -14dB ~ +8dB：0dB=1.0 原始大小，超出后饱和（防遮挡/防消失）
        const float gainFactor = juce::jlimit(0.2f, 2.5f, std::pow(10.0f, gainDb / 20.0f));
        const float topY = centerY - halfH * normMag * gainFactor;
        blob.lineTo(px, topY);
    }
    blob.lineTo(static_cast<float>(x2), centerY);

    for (int frame = range.endFrameExclusive; frame-- > range.startFrame;)
    {
        const float normMag = energy[static_cast<size_t>(frame)] / clipRefMag;
        const double sourceTime = timeline.timeAtFrame(frame);
        const float px = static_cast<float>(juce::jlimit(x1, x2, sourceTimeToScreenX(sourceTime, ctx, item)));
        const float gainDb = volumeEnvelope ? volumeEnvelope->evalAt(sourceTime) : 0.0f;
        // 视觉范围 -14dB ~ +8dB：0dB=1.0 原始大小，超出后饱和（防遮挡/防消失）
        const float gainFactor = juce::jlimit(0.2f, 2.5f, std::pow(10.0f, gainDb / 20.0f));
        const float bottomY = centerY + halfH * normMag * gainFactor;
        blob.lineTo(px, bottomY);
    }
    blob.closeSubPath();

    outPath = std::move(blob);
    return true;
}

/// Span-stream F0 visual builder: replays the span producer twice.
/// Pass 1 computes energy min/max across valid F0 frequencies.
/// Pass 2 builds visual segments with bucket min/max envelope for LOD, linear path for downsampled, Bézier for full-resolution.
/// Producer receives a sink(startFrame, data, length, gain).
template <typename SpanProducer, typename FX, typename FY>
static std::vector<F0VisualSegment> buildF0VisualSegments(
    const std::vector<float>* originalEnergy,
    int endFrameExclusive,
    const F0VisualBuildOptions& options,
    FX&& frameToX,
    FY&& frameToY,
    SpanProducer&& emitSpans)
{
    std::vector<F0VisualSegment> segments;

    const bool hasEnergy = originalEnergy != nullptr
        && endFrameExclusive <= static_cast<int>(originalEnergy->size());

    float minEnergy = std::numeric_limits<float>::max();
    float maxEnergy = std::numeric_limits<float>::lowest();
    if (hasEnergy) {
        emitSpans([&](int start, const float* data, int length, float gain) {
            if (!data) return;
            for (int i = 0; i < length; ++i) {
                const float frequency = data[i] * gain;
                if (frequency < 20.0f || frequency > 2000.0f) continue;
                const int globalFrame = start + i;
                const float energy = (*originalEnergy)[static_cast<size_t>(globalFrame)];
                if (std::isfinite(energy)) {
                    minEnergy = std::min(minEnergy, energy);
                    maxEnergy = std::max(maxEnergy, energy);
                }
            }
        });
    }

    const float targetPointSpacing = f0VisualTargetPointSpacing(options.pixelsPerSecond * options.secondsPerFrame);

    struct BucketAccumulator {
        bool active = false;
        float yMin = 0.0f;
        float yMax = 0.0f;
        float hotMixAccum = 0.0f;
        int pointCount = 0;

        void clear() noexcept {
            active = false;
            yMin = std::numeric_limits<float>::max();
            yMax = std::numeric_limits<float>::lowest();
            hotMixAccum = 0.0f;
            pointCount = 0;
        }
    };

    F0VisualSegment currentSegment;
    BucketAccumulator bucket;
    float bucketAnchorX = 0.0f;

    auto flushBucket = [&]() {
        if (!bucket.active) {
            bucket.clear();
            return;
        }
        const float avgHotMix = bucket.hotMixAccum / static_cast<float>(bucket.pointCount);
        currentSegment.points.push_back({ bucketAnchorX, bucket.yMin, avgHotMix });
        if (bucket.yMin != bucket.yMax) {
            currentSegment.points.push_back({ bucketAnchorX, bucket.yMax, avgHotMix });
        }
        bucket.clear();
    };

    auto flushSegment = [&]() {
        flushBucket();
        if (!currentSegment.points.empty()) {
            currentSegment.useLinearPath = (targetPointSpacing > 0.0f);
            segments.push_back(std::move(currentSegment));
            currentSegment = {};
        }
    };

    emitSpans([&](int start, const float* data, int length, float gain) {
        if (!data) {
            flushSegment();
            return;
        }
        for (int i = 0; i < length; ++i) {
            const float frequency = data[i] * gain;
            if (frequency < 20.0f || frequency > 2000.0f) {
                flushSegment();
                continue;
            }

            const int globalFrame = start + i;
            const float x = frameToX(globalFrame);

            const float y = frameToY(globalFrame, frequency);
            float levelHotMix = 0.0f;
            if (hasEnergy) {
                const float energy = (*originalEnergy)[static_cast<size_t>(globalFrame)];
                const float normalizedEnergy = juce::jlimit(0.0f, 1.0f,
                    (energy - minEnergy) / juce::jmax(1e-6f, maxEnergy - minEnergy));
                static constexpr float kMaxHotMix = 0.34f;
                levelHotMix = kMaxHotMix * smootherStep(normalizedEnergy);
            }

            if (targetPointSpacing <= 0.0f) {
                flushBucket();
                currentSegment.points.push_back({ x, y, levelHotMix });
                continue;
            }

            if (!bucket.active) {
                bucket.active = true;
                bucketAnchorX = x;
                bucket.yMin = y;
                bucket.yMax = y;
                bucket.hotMixAccum = levelHotMix;
                bucket.pointCount = 1;
                continue;
            }

            if (std::abs(x - bucketAnchorX) < targetPointSpacing) {
                bucket.yMin = std::min(bucket.yMin, y);
                bucket.yMax = std::max(bucket.yMax, y);
                bucket.hotMixAccum += levelHotMix;
                ++bucket.pointCount;
                continue;
            }

            flushBucket();

            bucket.active = true;
            bucketAnchorX = x;
            bucket.yMin = y;
            bucket.yMax = y;
            bucket.hotMixAccum = levelHotMix;
            bucket.pointCount = 1;
        }
    });

    flushSegment();

    return segments;
}

} // namespace

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

    // Scan only the visible F0 frame range for unvoiced intervals directly.
    int unvoicedStart = -1;
    const int totalFrames = static_cast<int>(originalF0.size());
    const auto visibleFrames = item.f0Timeline.rangeForTimes(
        visibleWindow.visibleContentStartTime,
        visibleWindow.visibleContentEndTime);
    const int startFrame = visibleFrames.startFrame;
    const int endFrameExclusive = std::min(visibleFrames.endFrameExclusive, totalFrames);
    if (endFrameExclusive <= startFrame)
        return;

    for (int frame = startFrame; frame < endFrameExclusive; ++frame) {
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
        const double intervalEndTime = item.f0Timeline.timeAtFrame(endFrameExclusive);

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
    if (wfLevel.peaks.empty())
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
    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
    const bool isOverdose = themeId == ThemeId::Overdose;

    juce::Path waveformPath;

    // Invert the output -> source mapping (tau_inverse) so the screen X axis
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

        if (idxStart >= numPeaks || idxStart < 0)
            continue;

        float aggMin = 0.0f;
        float aggMax = 0.0f;
        bool hasData = false;

        for (int64_t i = idxStart; i < idxEnd && i < numPeaks; ++i)
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
            g.setColour(waveformColour.withAlpha(0.12f));
            g.strokePath(waveformPath, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(waveformColour.withAlpha(0.26f));
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
    static constexpr float kNoteLabelFontSize = 12.0f;
    static constexpr float kShowAllLabelsMinPPS = 14.0f;
    static constexpr float kShowCOnlyMinPPS = 12.0f;

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

    // EqualSpacing: shift keyboard up half a key so key centers align with grid lines (matching note rendering)
    const float keyYOffset = (ctx.coords.gridStyle == PianoGridStyle::EqualSpacing)
                             ? -(ctx.pixelsPerSemitone * 0.5f) : 0.0f;

    for (int midi = static_cast<int>(ctx.minMidi); midi <= static_cast<int>(ctx.maxMidi); ++midi)
    {
        int drawMidi = midi;
        float y = ctx.coords.midiToY(static_cast<float>(drawMidi)) + keyYOffset;
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
                g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultSansSerifFontName(), "Bold", kNoteLabelFontSize)));
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
        float y = ctx.coords.midiToY(static_cast<float>(drawMidi)) + keyYOffset;
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
        float y = ctx.coords.midiToY(static_cast<float>(drawMidi)) + keyYOffset;
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

                if (isOverdose)
                {
                    // 深蓝紫黑键 + 粉色高光边
                    g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.32f));
                    g.drawRoundedRectangle(keyRect.reduced(0.5f), 2.0f, 1.0f);
                }
                else
                {
                    g.setColour(juce::Colours::black.withAlpha(isLightTheme ? 0.50f : 0.6f));
                    g.drawRoundedRectangle(keyRect.reduced(0.5f), 2.0f, 1.0f);
                }
            }

            // Scale highlight overlay on in-scale black keys (reduced alpha)
            if (inScale && ctx.scaleType != kScaleTypeChromatic)
            {
                g.setColour(isLightTheme ? UIColors::scaleHighlight.withMultipliedAlpha(0.22f)
                                         : UIColors::scaleHighlight.withMultipliedAlpha(0.5f));
                g.fillRoundedRectangle(keyRect, 2.0f);
            }

            // Note name labels for black keys (drawn on top of the black key body with outline)
            if (effectiveNoteNameMode == 0)
            {
                g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultSansSerifFontName(), "Bold", kNoteLabelFontSize)));
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
    const auto& notes = *item.displayNotes;
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

    // ── OpenDyne waveform blob 模式 ──────────────────────────────
    if (item.notesPrimaryScheme)
    {
        // blob 是主音符图形：只依赖持久 originalEnergy + F0Timeline，不依赖可选 PCM/mipmap
        if (item.pitchSnapshot == nullptr
            || item.pitchSnapshot->getOriginalEnergy().empty()
            || item.f0Timeline.isEmpty())
            return;

        const auto& energy = item.pitchSnapshot->getOriginalEnergy();

        // blob 音量缩放：拖拽预览包络优先，否则回退到已提交音量包络
        const AutomationLane* volumeEnvelope = nullptr;
        if (item.ownerSnapshot != nullptr) {
            volumeEnvelope = (item.active && volumePreviewEnvelope_ != nullptr)
                ? volumePreviewEnvelope_ : &item.ownerSnapshot->volumeEnvelope;
        }

        // ── clip 级振幅参考值：扫描全部 energy 帧，取最大值 ──
        float clipRefMag = 0.0f;
        for (const float e : energy) {
            if (e > clipRefMag) clipRefMag = e;
        }
        if (clipRefMag <= 0.0f)
            return;  // 静音 clip，跳过 blob 绘制

        for (auto noteIt = firstVisibleNote; noteIt != lastVisibleNote; ++noteIt)
        {
            const auto& note = *noteIt;
            float adjustedPitch = note.getAdjustedPitch();
            if (adjustedPitch <= 0.0f) continue;

            float midi = ctx.coords.freqToMidi(adjustedPitch);
            float centerY = ctx.coords.midiToY(midi);
            // 正常响度音频在 clipRefMag=1.0 时占 3 个 key（1.5 semitones × 2）
            constexpr float kBlobHalfKeys = 1.5f;
            const float halfH = ctx.pixelsPerSemitone * kBlobHalfKeys;

            int x1 = sourceTimeToScreenX(note.startTime, ctx, item);
            int x2 = sourceTimeToScreenX(note.endTime,   ctx, item);
            if (x2 <= visibleWindow.viewportStartX || x1 >= visibleWindow.viewportEndX)
                continue;

            const auto noteColour = note.eq.has_value() && note.eq->active
                ? item.displayColour.darker(0.30f)
                : item.displayColour;

            // 顶边 + 底边闭合 Path：X 用 source-time→timeline→screen 投影
            juce::Path blob;
            if (!buildNoteBlobPath(note, ctx, item, energy, item.f0Timeline,
                                   clipRefMag, volumeEnvelope, centerY, halfH, x1, x2, blob))
                continue;

            // 能量自适应纵向渐变：列式渲染，过渡带亮度和宽窄均随局部 energy 缩放
            {
                const auto fillTop = noteColour.darker(0.22f).withAlpha(0.32f);
                const auto fillBottom = noteColour.darker(0.22f).withAlpha(0.32f);
                const auto glowCore = noteColour
                    .interpolatedWith(juce::Colours::white, 0.88f)
                    .withAlpha(0.95f);

                g.saveState();
                g.reduceClipRegion(blob);

                constexpr float kStep = 2.0f;
                for (float x = static_cast<float>(x1); x < static_cast<float>(x2); x += kStep)
                {
                    const float srcTime = static_cast<float>(note.startTime
                        + (note.endTime - note.startTime)
                          * ((x - static_cast<float>(x1))
                             / static_cast<float>(x2 - x1)));
                    const int frame = item.f0Timeline.frameAtOrBefore(srcTime);
                    const float normEnergy = (frame >= 0
                        && frame < static_cast<int>(energy.size()))
                        ? juce::jlimit(0.0f, 1.0f,
                            energy[static_cast<size_t>(frame)] / clipRefMag)
                        : 0.0f;

                    const auto glowTrans = noteColour
                        .interpolatedWith(juce::Colours::white, 0.35f * normEnergy)
                        .withAlpha(0.65f + 0.05f * normEnergy);

                    // 过渡带宽窄自适应：normEnergy=0 → 亮带20%（窄），=1 → 亮带60%（宽）
                    const float bw = 0.20f + 0.40f * normEnergy;
                    const float tLo = 0.50f - bw * 0.5f;
                    const float tHi = 0.50f + bw * 0.5f;
                    const float trLo = tLo * 0.5f;
                    const float trHi = tHi + (1.0f - tHi) * 0.5f;

                    const float rectH = halfH;
                    juce::ColourGradient col(fillTop, x, centerY - rectH,
                                             fillBottom, x, centerY + rectH, false);
                    col.addColour(trLo, fillTop);
                    col.addColour(tLo,  glowTrans);
                    col.addColour(0.50f, glowCore);
                    col.addColour(tHi,  glowTrans);
                    col.addColour(trHi, fillBottom);

                    g.setGradientFill(col);
                    g.fillRect(x, centerY - rectH, kStep, rectH * 2.0f);
                }

                g.restoreState();
            }

            g.setColour(noteColour.brighter(0.45f).withAlpha(0.55f));
            g.strokePath(blob, juce::PathStrokeType(0.9f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

            if (note.eq.has_value() && note.eq->active)
            {
                g.setColour(item.displayColour.withAlpha(0.85f));
                g.strokePath(blob, juce::PathStrokeType(2.0f,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
            }

            // ── HiFiGAN vocoder glow on blob ──
            if (item.pitchSnapshot != nullptr
                && item.pitchSnapshot->noteNeedsVocoder(note.startTime, note.endTime, item.f0Timeline))
            {
                g.saveState();
                g.reduceClipRegion(blob);

                const auto glowColour = item.displayColour.brighter(0.4f);
                juce::ColourGradient glow(glowColour.withAlpha(0.18f),
                                          static_cast<float>(x1), centerY - halfH,
                                          juce::Colours::transparentWhite,
                                          static_cast<float>(x2), centerY + halfH,
                                          false);
                g.setGradientFill(glow);
                g.fillPath(blob);

                g.setColour(item.displayColour.brighter(0.5f).withAlpha(0.28f));
                g.strokePath(blob, juce::PathStrokeType(1.2f,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));

                g.restoreState();
            }
        }

        return;
    }

    // ── OpenTune 矩形音符（使用 track theme colour 联动） ───────
    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
    const bool isOverdose = themeId == ThemeId::Overdose;
    constexpr float kNoteBodyFillAlpha = 0.72f;

    for (auto noteIt = firstVisibleNote; noteIt != lastVisibleNote; ++noteIt)
    {
        const auto& note = *noteIt;
        float adjustedPitch = note.getAdjustedPitch();
        if (adjustedPitch <= 0.0f) continue;

        float midi = ctx.coords.freqToMidi(adjustedPitch);
        float y = ctx.coords.midiToY(midi) - (ctx.pixelsPerSemitone * 0.5f);
        float h = ctx.pixelsPerSemitone;

        int x1 = sourceTimeToScreenX(note.startTime, ctx, item);
        int x2 = sourceTimeToScreenX(note.endTime,   ctx, item);
        if (x2 <= visibleWindow.viewportStartX || x1 >= visibleWindow.viewportEndX)
            continue;

        float w = std::max(1.0f, static_cast<float>(x2 - x1));
        auto noteBounds = juce::Rectangle<float>(static_cast<float>(x1), y, w, h);

        const auto noteColor = note.eq.has_value() && note.eq->active
            ? item.displayColour.darker(0.30f)
            : item.displayColour;

        if (isAurora)
        {
            g.setColour(noteColor.withAlpha(kNoteBodyFillAlpha));
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

            if (note.eq.has_value() && note.eq->active)
            {
                g.setColour(item.displayColour.withAlpha(0.85f));
                g.drawRect(noteBounds.expanded(1.0f), 2.0f);
            }

            g.setColour(UIColors::glassHighlight.withAlpha(0.14f));
            g.drawLine(noteBounds.getX() + 1.0f,
                       noteBounds.getY() + 1.0f,
                       noteBounds.getRight() - 1.0f,
                       noteBounds.getY() + 1.0f,
                       1.0f);
        }
        else if (isBlueBreeze || isOverdose)
        {
            g.setColour(noteColor.withAlpha(kNoteBodyFillAlpha));
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

            if (note.eq.has_value() && note.eq->active)
            {
                g.setColour(item.displayColour.withAlpha(0.85f));
                g.drawRect(noteBounds.expanded(1.5f), 2.0f);
            }
        }
        else
        {
            g.setColour(noteColor.withAlpha(kNoteBodyFillAlpha));
            g.fillRect(noteBounds);

            g.setColour(UIColors::noteBlockBorder.withAlpha(0.50f));
            g.drawRect(noteBounds, 1.0f);

            if (note.eq.has_value() && note.eq->active)
            {
                g.setColour(item.displayColour.withAlpha(0.85f));
                g.drawRect(noteBounds.expanded(1.0f), 2.0f);
            }
        }

        // ── HiFiGAN vocoder glow: subtle top-left light source ──
        if (item.pitchSnapshot != nullptr
            && item.pitchSnapshot->noteNeedsVocoder(note.startTime, note.endTime, item.f0Timeline))
        {
            g.saveState();
            g.reduceClipRegion(noteBounds.toType<int>());

            const auto glowColour = item.displayColour.brighter(0.4f);
            juce::ColourGradient glow(glowColour.withAlpha(0.20f),
                                      noteBounds.getX(), noteBounds.getY(),
                                      juce::Colours::transparentWhite,
                                      noteBounds.getRight(), noteBounds.getBottom(),
                                      false);
            g.setGradientFill(glow);
            g.fillRect(noteBounds);

            g.setColour(item.displayColour.brighter(0.6f).withAlpha(0.32f));
            g.drawLine(noteBounds.getX() + 0.5f, noteBounds.getY() + 0.5f,
                       noteBounds.getRight() - 0.5f, noteBounds.getY() + 0.5f, 1.2f);
            g.drawLine(noteBounds.getX() + 0.5f, noteBounds.getY() + 0.5f,
                       noteBounds.getX() + 0.5f, noteBounds.getBottom() - 0.5f, 1.2f);

            g.restoreState();
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

    // ── OpenDyne：选中高亮直接叠加在波形 blob 上（blob 几何与 drawNotes 完全一致） ──
    if (item.notesPrimaryScheme)
    {
        // blob 是主音符图形：只依赖持久 originalEnergy + F0Timeline，不依赖可选 PCM/mipmap
        if (item.pitchSnapshot == nullptr
            || item.pitchSnapshot->getOriginalEnergy().empty()
            || item.f0Timeline.isEmpty())
            return;

        const auto& energy = item.pitchSnapshot->getOriginalEnergy();

        // blob 音量缩放：与 drawNotes 一致——拖拽预览包络优先，否则回退到已提交音量包络
        const AutomationLane* volumeEnvelope = nullptr;
        if (item.ownerSnapshot != nullptr) {
            volumeEnvelope = (item.active && volumePreviewEnvelope_ != nullptr)
                ? volumePreviewEnvelope_ : &item.ownerSnapshot->volumeEnvelope;
        }

        // clip 级振幅参考值：与 drawNotes 一致
        float clipRefMag = 0.0f;
        for (const float e : energy) {
            if (e > clipRefMag) clipRefMag = e;
        }
        if (clipRefMag <= 0.0f)
            return;  // 静音 clip，无 blob 可高亮

        constexpr float kBlobHalfKeys = 1.5f;
        const float halfH = ctx.pixelsPerSemitone * kBlobHalfKeys;

        for (int idx : selectedNoteIndices)
        {
            if (idx < 0 || idx >= static_cast<int>(notes.size()))
                continue;

            const auto& note = notes[static_cast<size_t>(idx)];
            float adjustedPitch = note.getAdjustedPitch();
            if (adjustedPitch <= 0.0f)
                continue;

            float midi = ctx.coords.freqToMidi(adjustedPitch);
            float centerY = ctx.coords.midiToY(midi);

            int x1 = sourceTimeToScreenX(note.startTime, ctx, item);
            int x2 = sourceTimeToScreenX(note.endTime, ctx, item);
            if (x2 <= visibleWindow.viewportStartX || x1 >= visibleWindow.viewportEndX)
                continue;

            // Clip to content viewport to prevent drawing into piano key area
            x1 = juce::jmax(x1, visibleWindow.viewportStartX);
            x2 = juce::jmin(x2, visibleWindow.viewportEndX);

            juce::Path blob;
            if (!buildNoteBlobPath(note, ctx, item, energy, item.f0Timeline,
                                   clipRefMag, volumeEnvelope, centerY, halfH, x1, x2, blob))
                continue;

            // 选中态：透明选区填充 + 高亮描边；中心高光由内容层绘制，F0 保持前景。
            g.setColour(item.displayColour.brighter(0.55f).withAlpha(0.45f));
            g.fillPath(blob);

            g.setColour(item.displayColour.brighter(0.75f).withAlpha(0.90f));
            g.strokePath(blob, juce::PathStrokeType(1.8f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
        }
        return;
    }

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

        // Selection highlight: semi-transparent tint + brighter border (track theme colour)
        g.setColour(item.displayColour.brighter(0.55f).withAlpha(isAurora ? 0.30f : 0.25f));
        g.fillRect(noteBounds);

        g.setColour(item.displayColour.brighter(0.75f).withAlpha(isAurora ? 0.90f : 0.85f));
        g.drawRect(noteBounds, isAurora ? 1.5f : 1.2f);
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

        const float alpha = h.isEndpoint() ? 0.3f : 0.4f;
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
        if (h.isEndpoint()) {
            col = col.withAlpha(0.45f);
        } else if (h.confidence == Confidence::High) {
            col = kHighConfidenceColour;
        }

        const bool isHigh = (!h.isEndpoint() && h.confidence == Confidence::High);
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

void PianoRollRenderer::drawF0SelectionHighlight(juce::Graphics& g,
                                                  const RenderContext& ctx,
                                                  const ContentRenderItem& item)
{
    if (item.f0Timeline.isEmpty()) return;
    if (!item.pitchSnapshot || item.pitchSnapshot->size() == 0) return;
    if (item.notesPrimaryScheme) return;   // OpenDyne：notes-primary 无 F0 框选
    if (!ctx.showOriginalF0) return;
    if (!ctx.hasF0Selection || ctx.f0SelectionRanges.empty()) return;

    const auto visibleWindow = computeFullViewportTimeWindow(ctx, item);
    if (!visibleWindow.isValid()) return;

    const int marginFrames = 10;
    const auto visibleFrames = item.f0Timeline.rangeForTimesWithMargin(
        visibleWindow.visibleContentStartTime,
        visibleWindow.visibleContentEndTime,
        marginFrames);
    const int startFrame = static_cast<int>(std::min(static_cast<std::size_t>(visibleFrames.startFrame),
                                                     item.pitchSnapshot->size()));
    const int endFrame = static_cast<int>(std::min(static_cast<std::size_t>(
        std::max(visibleFrames.startFrame, visibleFrames.endFrameExclusive)),
        item.pitchSnapshot->size()));

    double secondsPerFrame = 0.01;
    if (item.f0Timeline.endFrameExclusive() > 1) {
        secondsPerFrame = item.f0Timeline.timeAtFrame(1) - item.f0Timeline.timeAtFrame(0);
    }

    F0VisualBuildOptions visualOptions;
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

    const juce::Colour selectionColour = UIColors::originalF0.brighter(isAurora ? 0.18f : 0.14f);
    const float baseLineWidth = isAurora ? 1.35f : ((isBlueBreeze || isOverdose) ? 1.15f : 1.25f);
    const float selectionLineWidth = baseLineWidth + (isAurora ? 0.85f : 0.65f);
    const juce::PathStrokeType selectionStrokeType(selectionLineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    const float glowLineWidth = selectionLineWidth + (isAurora ? 2.2f : 1.8f);
    const juce::PathStrokeType glowStrokeType(glowLineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    // 每个选中范围独立裁剪到可见窗口并绘制
    for (const auto& [selStartFrame, selEndFrame] : ctx.f0SelectionRanges) {
        const int selStart = std::max(selStartFrame, startFrame);
        const int selEnd = std::min(selEndFrame, endFrame);
        if (selEnd <= selStart) continue;

        const auto& originalF0 = item.pitchSnapshot->getOriginalF0();
        const int origEnd = std::min(selEnd, static_cast<int>(originalF0.size()));
        if (origEnd <= selStart) continue;

        auto selectionProducer = [&](auto&& sink) {
            sink(selStart, originalF0.data() + selStart, origEnd - selStart, 1.0f);
        };

        const auto visualSegments = buildF0VisualSegments(
            originalEnergy, origEnd, visualOptions, makeFrameToX, makeFrameToY, selectionProducer);

        for (const auto& segment : visualSegments) {
            if (segment.points.empty()) continue;

            if (segment.points.size() == 1) {
                const auto& p = segment.points.front();
                juce::Path ptPath;
                ptPath.startNewSubPath(p.x - 0.01f, p.y);
                ptPath.lineTo(p.x + 0.01f, p.y);
                if (isAurora) {
                    g.setColour(selectionColour.withAlpha(0.10f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(selectionColour.withAlpha(0.96f));
                    g.strokePath(ptPath, selectionStrokeType);
                } else if (isBlueBreeze || isOverdose) {
                    g.setColour(selectionColour.withAlpha(0.075f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(selectionColour.withAlpha(0.96f));
                    g.strokePath(ptPath, selectionStrokeType);
                } else {
                    g.setColour(selectionColour.withAlpha(0.96f));
                    g.strokePath(ptPath, selectionStrokeType);
                }
                continue;
            }

            juce::Path runPath;
            if (segment.useLinearPath) {
                appendLinearF0Path(runPath, segment.points, 0, segment.points.size() - 1);
            } else {
                appendSmoothedF0Path(runPath, segment.points, 0, segment.points.size() - 1);
            }

            if (isAurora) {
                g.setColour(selectionColour.withAlpha(0.10f));
                g.strokePath(runPath, glowStrokeType);
                g.setColour(selectionColour.withAlpha(0.96f));
                g.strokePath(runPath, selectionStrokeType);
            } else if (isBlueBreeze || isOverdose) {
                g.setColour(selectionColour.withAlpha(0.075f));
                g.strokePath(runPath, glowStrokeType);
                g.setColour(selectionColour.withAlpha(0.96f));
                g.strokePath(runPath, selectionStrokeType);
            } else {
                g.setColour(selectionColour.withAlpha(0.96f));
                g.strokePath(runPath, selectionStrokeType);
            }
        }
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

    const auto visibleWindow = computeFullViewportTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    const int marginFrames = 10;
    const auto visibleFrames = item.f0Timeline.rangeForTimesWithMargin(
        visibleWindow.visibleContentStartTime,
        visibleWindow.visibleContentEndTime,
        marginFrames);
    const int startFrame = static_cast<int>(std::min(static_cast<std::size_t>(visibleFrames.startFrame),
                                                     item.pitchSnapshot->size()));
    const int endFrame = static_cast<int>(std::min(static_cast<std::size_t>(
        std::max(visibleFrames.startFrame, visibleFrames.endFrameExclusive)),
        item.pitchSnapshot->size()));

    double secondsPerFrame = 0.01;
    if (item.f0Timeline.endFrameExclusive() > 1) {
        secondsPerFrame = item.f0Timeline.timeAtFrame(1) - item.f0Timeline.timeAtFrame(0);
    }

    F0VisualBuildOptions visualOptions;
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

    // Draw original F0 (thin) — single contiguous span producer
    if (ctx.showOriginalF0 && !item.notesPrimaryScheme) {
        const auto& originalF0 = item.pitchSnapshot->getOriginalF0();
        const int origStart = startFrame;
        const int origEnd = std::min(endFrame, static_cast<int>(originalF0.size()));

        auto originalProducer = [&](auto&& sink) {
            if (origStart < origEnd)
                sink(origStart, originalF0.data() + origStart, origEnd - origStart, 1.0f);
        };

        const auto visualSegments = buildF0VisualSegments(
            originalEnergy, origEnd, visualOptions, makeFrameToX, makeFrameToY, originalProducer);

        const juce::Colour colour = UIColors::originalF0;
        const float alpha = 1.0f;

        const float lineWidth = isAurora ? 1.35f : ((isBlueBreeze || isOverdose) ? 1.15f : 1.25f);
        const juce::PathStrokeType strokeType(lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const float glowLineWidth = lineWidth + (isAurora ? 2.2f : 1.8f);
        const juce::PathStrokeType glowStrokeType(glowLineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const juce::PathStrokeType innerGlowStrokeType(lineWidth + 0.72f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        for (const auto& segment : visualSegments) {
            if (segment.points.empty()) continue;

            if (segment.points.size() == 1) {
                const auto& p = segment.points.front();
                juce::Path ptPath;
                ptPath.startNewSubPath(p.x - 0.01f, p.y);
                ptPath.lineTo(p.x + 0.01f, p.y);
                if (isAurora) {
                    g.setColour(colour.withAlpha(alpha * 0.080f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(colour.withAlpha(alpha * 0.22f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(colour.withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                } else if (isBlueBreeze || isOverdose) {
                    g.setColour(colour.withAlpha(alpha * 0.055f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(colour.withAlpha(alpha * 0.14f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(colour.withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                } else {
                    g.setColour(colour.withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                }
                continue;
            }

            juce::Path runPath;
            if (segment.useLinearPath) {
                appendLinearF0Path(runPath, segment.points, 0, segment.points.size() - 1);
            } else {
                appendSmoothedF0Path(runPath, segment.points, 0, segment.points.size() - 1);
            }

            if (isAurora) {
                g.setColour(colour.withAlpha(alpha * 0.080f));
                g.strokePath(runPath, glowStrokeType);
                g.setColour(colour.withAlpha(alpha * 0.22f));
                g.strokePath(runPath, innerGlowStrokeType);
                g.setColour(colour.withAlpha(1.0f));
                g.strokePath(runPath, strokeType);
            } else if (isBlueBreeze || isOverdose) {
                g.setColour(colour.withAlpha(alpha * 0.055f));
                g.strokePath(runPath, glowStrokeType);
                g.setColour(colour.withAlpha(alpha * 0.14f));
                g.strokePath(runPath, innerGlowStrokeType);
                g.setColour(colour.withAlpha(1.0f));
                g.strokePath(runPath, strokeType);
            } else {
                g.setColour(colour.withAlpha(1.0f));
                g.strokePath(runPath, strokeType);
            }
        }
    }

    // Draw effective corrected F0 (thicker).
    // 拖拽预览 item 已携带临时 snapshot（noteDrag.previewSnapshot）：其 pitchCurve
    // 是 clone + applyCorrectionToRange 的烘焙结果，OpenTune 的 shouldDrawCorrected
    // 由该临时 curve 的 correction layer 自然成立，无需任何覆盖注入。
    // OpenDyne（notesPrimaryScheme）：显示有效F0（修正段+回退OriginalF0）；
    // OpenTune：仅存在修正层/非恒等pitchShift 时才显示CorrectedF0，
    // 无修正时只显示OriginalF0红色细线。
    if (ctx.showCorrectedF0 && item.ownerSnapshot) {
        const bool hasCorrections = item.pitchSnapshot->hasCorrectionLayer()
            || !item.ownerSnapshot->pitchShiftSettings.isIdentity();
        const bool shouldDrawCorrected = item.notesPrimaryScheme
            ? item.pitchSnapshot->hasOriginalF0Data()
            : hasCorrections;
        if (shouldDrawCorrected) {
        auto correctedProducer = [&](auto&& sink) {
            item.ownerSnapshot->forEachEffectiveF0Span(startFrame, endFrame, sink);
        };

        const auto visualSegments = buildF0VisualSegments(
            originalEnergy, endFrame, visualOptions, makeFrameToX, makeFrameToY, correctedProducer);

        const juce::Colour colour = UIColors::correctedF0;
        static const juce::Colour kLevelHotGold { 0xFFFFC24A };
        static const juce::Colour kOpenDyneBrightCurve { 0xFFFF9C1A }; // 暗轨→深黄橙（原亮金降饱和）
        static const juce::Colour kOpenDyneDarkCurve   { 0xFF196FC4 }; // 亮轨→主题蓝
        const auto blendLevelHotColour = [&](juce::Colour base, float hm) {
            if (!item.notesPrimaryScheme)
                return base.interpolatedWith(kLevelHotGold,
                    juce::jlimit(0.0f, 0.42f, hm));
            // OpenDyne：HSL 感知亮度决定对比色基调，再叠 levelHotMix 暖化
            const float lum = (0.299f * item.displayColour.getRed()
                             + 0.587f * item.displayColour.getGreen()
                             + 0.114f * item.displayColour.getBlue()) / 255.0f;
            const auto contrastBase = lum < 0.5f
                ? kOpenDyneDarkCurve : kOpenDyneBrightCurve;
            return contrastBase.interpolatedWith(kLevelHotGold,
                juce::jlimit(0.0f, 0.42f, hm));
        };
        const float alpha = 1.0f;

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
                    g.setColour(blendLevelHotColour(colour, p.levelHotMix).withAlpha(alpha * 0.095f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(blendLevelHotColour(colour, p.levelHotMix).withAlpha(alpha * 0.20f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(blendLevelHotColour(colour, p.levelHotMix).withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                } else if (isBlueBreeze || isOverdose) {
                    g.setColour(blendLevelHotColour(colour, p.levelHotMix).withAlpha(alpha * 0.070f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(blendLevelHotColour(colour, p.levelHotMix).withAlpha(alpha * 0.15f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(blendLevelHotColour(colour, p.levelHotMix).withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                } else {
                    g.setColour(blendLevelHotColour(colour, p.levelHotMix).withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                }
                continue;
            }

            juce::Path runPath;
            if (segment.useLinearPath) {
                appendLinearF0Path(runPath, segment.points, 0, segment.points.size() - 1);
            } else {
                appendSmoothedF0Path(runPath, segment.points, 0, segment.points.size() - 1);
            }

            if (isAurora) {
                const auto& pts = segment.points;
                const float leftX = pts.front().x;
                const float rightX = pts.back().x;
                const float xRange = juce::jmax(1.0f, rightX - leftX);

                auto buildGradient = [&](float alphaScale, bool brighter = false) {
                    juce::ColourGradient grad;
                    grad.isRadial = false;
                    grad.point1 = { leftX, 0.0f };
                    grad.point2 = { rightX, 0.0f };
                    for (const auto& pt : pts) {
                        const juce::Colour c = brighter
                            ? blendLevelHotColour(colour, pt.levelHotMix).brighter(0.20f)
                            : blendLevelHotColour(colour, pt.levelHotMix);
                        const double pos = juce::jlimit(0.0, 1.0, static_cast<double>((pt.x - leftX) / xRange));
                        grad.addColour(pos, c.withAlpha(alpha * alphaScale));
                    }
                    return grad;
                };

                g.setGradientFill(buildGradient(0.095f));
                g.strokePath(runPath, glowStrokeType);
                g.setGradientFill(buildGradient(0.20f));
                g.strokePath(runPath, innerGlowStrokeType);
                g.setGradientFill(buildGradient(1.0f));
                g.strokePath(runPath, strokeType);
                g.setGradientFill(buildGradient(0.15f, true));
                g.strokePath(runPath, highlightStrokeType);
            } else if (isBlueBreeze || isOverdose) {
                const auto& pts = segment.points;
                const float leftX = pts.front().x;
                const float rightX = pts.back().x;
                const float xRange = juce::jmax(1.0f, rightX - leftX);

                auto buildGradient = [&](float alphaScale, bool brighter = false) {
                    juce::ColourGradient grad;
                    grad.isRadial = false;
                    grad.point1 = { leftX, 0.0f };
                    grad.point2 = { rightX, 0.0f };
                    for (const auto& pt : pts) {
                        const juce::Colour c = brighter
                            ? blendLevelHotColour(colour, pt.levelHotMix).brighter(0.16f)
                            : blendLevelHotColour(colour, pt.levelHotMix);
                        const double pos = juce::jlimit(0.0, 1.0, static_cast<double>((pt.x - leftX) / xRange));
                        grad.addColour(pos, c.withAlpha(alpha * alphaScale));
                    }
                    return grad;
                };

                g.setGradientFill(buildGradient(0.070f));
                g.strokePath(runPath, glowStrokeType);
                g.setGradientFill(buildGradient(0.15f));
                g.strokePath(runPath, innerGlowStrokeType);
                g.setGradientFill(buildGradient(1.0f));
                g.strokePath(runPath, strokeType);
                g.setGradientFill(buildGradient(0.12f, true));
                g.strokePath(runPath, highlightStrokeType);
            } else {
                const auto& pts = segment.points;
                const float leftX = pts.front().x;
                const float rightX = pts.back().x;
                const float xRange = juce::jmax(1.0f, rightX - leftX);

                juce::ColourGradient grad;
                grad.isRadial = false;
                grad.point1 = { leftX, 0.0f };
                grad.point2 = { rightX, 0.0f };
                for (const auto& pt : pts) {
                    const juce::Colour c = blendLevelHotColour(colour, pt.levelHotMix);
                    const double pos = juce::jlimit(0.0, 1.0, static_cast<double>((pt.x - leftX) / xRange));
                    grad.addColour(pos, c.withAlpha(1.0f));
                }
                g.setGradientFill(grad);
                g.strokePath(runPath, strokeType);

                // OpenDyne：极细橘红描边叠加在 corrected F0 上
                if (item.notesPrimaryScheme) {
                    static const juce::Colour kOpenDyneAccentStroke { 0xFFE85D2A }; // 橘红
                    g.setColour(kOpenDyneAccentStroke.withAlpha(0.65f));
                    g.strokePath(runPath, juce::PathStrokeType(0.55f,
                                                              juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));
                }
            }
        }

        // ── HiFiGAN vocoder glow: extra wide, low-alpha pass ──
        if (item.pitchSnapshot != nullptr) {
            bool hasVocoder = false;
            for (int f = startFrame; f < endFrame; ++f) {
                if (item.pitchSnapshot->isVocoderFrame(f)) { hasVocoder = true; break; }
            }
            if (hasVocoder) {
                const float extraGlowWidth = lineWidth + 2.5f;
                const juce::PathStrokeType extraGlowStroke(extraGlowWidth,
                    juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

                for (const auto& segment : visualSegments) {
                    if (segment.points.empty()) continue;
                    juce::Path glowPath;
                    if (segment.points.size() == 1) {
                        const auto& p = segment.points.front();
                        glowPath.startNewSubPath(p.x - 0.01f, p.y);
                        glowPath.lineTo(p.x + 0.01f, p.y);
                    } else if (segment.useLinearPath) {
                        appendLinearF0Path(glowPath, segment.points, 0, segment.points.size() - 1);
                    } else {
                        appendSmoothedF0Path(glowPath, segment.points, 0, segment.points.size() - 1);
                    }
                    g.setColour(colour.withAlpha(0.14f));
                    g.strokePath(glowPath, extraGlowStroke);
                }
            }
        }
        }
    }
}

} // namespace OpenTune
