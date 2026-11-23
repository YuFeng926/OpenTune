#include "PianoRollRenderer.h"
#include "../../../Utils/TimeCoordinate.h"
#include "PianoRollTimeMap.h"
#include "../UiAssets.h"
#include "../UIColors.h"
#include "../../../Utils/AppLogger.h"
#include "../../../Utils/NoteGeneratorTypes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace OpenTune {

// ============================================================================
// Visible time window helpers
// ============================================================================
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
    PianoRollRenderer::PlacedContent item,
    int startX, int endX)
{
    VisibleTimeWindow window;
    if (!item.placement.projection.isValid())
        return window;

    window.viewportStartX = startX;
    window.viewportEndX = endX;
    if (window.viewportEndX <= window.viewportStartX)
        return {};

    window.visibleStartTime = ctx.coords.xToTime(window.viewportStartX);
    window.visibleEndTime = ctx.coords.xToTime(window.viewportEndX);
    if (window.visibleEndTime <= window.visibleStartTime)
        return {};

    // vocal-time-stretch §8.5 — projectTimelineTimeToContent returns OUTPUT time
    // inside the content, but Notes / PitchCurve / F0 timeline / WaveformMipmap
    // are all indexed by SOURCE time. Convert to source time via tauInverse.
    jassert(item.content.committedSnapshot->timeGrid);
    const PianoRollTimeMap map(item.placement.projection,
                               *item.content.committedSnapshot->timeGrid, ctx.coords);
    window.visibleContentStartTime = map.timelineToSource(window.visibleStartTime);
    window.visibleContentEndTime = map.timelineToSource(window.visibleEndTime);
    return window;
}

// Damage-aware window for waveform/notes/unvoiced/anchors/F0.
VisibleTimeWindow computeVisibleTimeWindow(const PianoRollRenderer::RenderContext& ctx,
                                            PianoRollRenderer::PlacedContent item)
{
    const int startX = ctx.rasterBounds.isEmpty()
        ? ctx.contentStartX
        : std::max(ctx.rasterBounds.getX(), ctx.contentStartX);
    const int endX = ctx.rasterBounds.isEmpty()
        ? ctx.width
        : std::min(ctx.rasterBounds.getRight(), ctx.width);
    return computeTimeWindowFromXBounds(ctx, item, startX, endX);
}

// 帧下标随机访问迭代器：让 std::lower_bound/upper_bound 直接在 [0, totalFrames)
// 的帧号上二分，不构造容器；比较经单调不减的连续投影 X(f) 正向完成，不做时间反算。
struct FrameIndexIterator {
    using iterator_category = std::random_access_iterator_tag;
    using value_type = int;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = value_type;

    int frame = 0;

    reference operator*() const { return frame; }
    FrameIndexIterator& operator++() { ++frame; return *this; }
    bool operator==(const FrameIndexIterator& rhs) const { return frame == rhs.frame; }
    bool operator!=(const FrameIndexIterator& rhs) const { return frame != rhs.frame; }
    FrameIndexIterator& operator+=(difference_type n) { frame += static_cast<int>(n); return *this; }
    FrameIndexIterator operator+(difference_type n) const { return FrameIndexIterator{frame + static_cast<int>(n)}; }
    difference_type operator-(const FrameIndexIterator& rhs) const { return frame - rhs.frame; }
    bool operator<(const FrameIndexIterator& rhs) const { return frame < rhs.frame; }
};

// 全视口帧区间：对单调不减的连续投影 X(f) = sourceToXExact(timeAtFrame(f)) 在帧
// 下标上 std::lower_bound/upper_bound 二分（正向投影与视口边界比较），取视口
// [viewportLeftX, viewportRightX] 内首末帧后各外扩 marginFrames 帧并 clamp 到
// [0, totalFrames)。与原 rangeForTimesWithMargin 同口径，方向为正向投影。
static F0FrameRange visibleFrameRange(const PianoRollTimeMap& map,
                                      const F0Timeline& timeline,
                                      int totalFrames,
                                      int viewportLeftX,
                                      int viewportRightX)
{
    if (viewportRightX <= viewportLeftX)
        return {};

    constexpr int marginFrames = 10;
    const auto frameXExact = [&](int frame) {
        return map.sourceToXExact(timeline.timeAtFrame(frame));
    };
    const FrameIndexIterator frameBegin{0};
    const FrameIndexIterator frameEnd{totalFrames};
    const int firstVisible = static_cast<int>(
        std::lower_bound(frameBegin, frameEnd, static_cast<double>(viewportLeftX),
                         [&](int frame, double x) { return frameXExact(frame) < x; })
        - frameBegin);
    const int pastVisible = static_cast<int>(
        std::upper_bound(FrameIndexIterator{firstVisible}, frameEnd, static_cast<double>(viewportRightX),
                         [&](double x, int frame) { return x < frameXExact(frame); })
        - frameBegin);
    return {juce::jlimit(0, totalFrames, firstVisible - marginFrames),
            juce::jlimit(0, totalFrames, pastVisible + marginFrames)};
}

struct F0VisualPoint
{
    float x = 0.0f;
    float y = 0.0f;
    float levelHotMix = 0.0f;
    juce::Colour noteColour; // 音符实际颜色（考虑修正幅度）
};

struct F0VisualSegment
{
    std::vector<F0VisualPoint> points;
    // path 覆盖范围（points 内连续下标）：条带命中点段并前后各夹一个点，
    // 使描边跨越条带边界、边缘辉光仍覆盖 rasterBounds；渐变继续使用全部点。
    int pathFirst = 0;
    int pathLast = -1;
};

// 绘制条带（屏幕 X 域，含描边/辉光余量）：段级命中判定用。命中段保持全段
// 连续 X 投影与完整点列（渐变端点、色标口径与整绘一致），Y 与 path 构造只
// 覆盖条带周边；与条带无交集的段跳过全部逐帧几何。

static float smootherStep(float value) noexcept
{
    const float t = juce::jlimit(0.0f, 1.0f, value);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
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
                                PianoRollRenderer::PlacedContent item)
{
    jassert(item.content.committedSnapshot->timeGrid);
    const PianoRollTimeMap map(item.placement.projection,
                               *item.content.committedSnapshot->timeGrid, ctx.coords);
    return map.sourceToX(sourceTime);
}

// OpenDyne waveform blob：按 Note 的 F0Timeline 帧范围索引持久 originalEnergy，构建闭合波形 Path。
// 几何按投影后的实际像素列分组采样：同列帧合并为一组，每组保留首/最低/最高/末帧，
// 点数随屏幕像素间隔自适应。drawNotes 与选中高亮共用，保证 blob 几何完全一致。
static bool buildNoteBlobPath(
    const Note& note,
    const PianoRollRenderer::RenderContext& ctx,
    PianoRollRenderer::PlacedContent item,
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

    // F0 与 energy 是两条独立发布的数组（ARA 恢复分别解码、分别发布）：
    // 帧范围先与 energy 长度求交集，短 energy 不得越界采样。
    if (range.startFrame >= static_cast<int>(energy.size()))
        return false;
    const int frameCount = std::min(range.endFrameExclusive,
                                    static_cast<int>(energy.size())) - range.startFrame;

    // 一次投影所有帧：X(f) 单调不减，同列帧构成连续区间。
    std::vector<int> frameX(static_cast<size_t>(frameCount));
    std::vector<float> frameAmp(static_cast<size_t>(frameCount));
    for (int i = 0; i < frameCount; ++i)
    {
        const int frame = range.startFrame + i;
        const double sourceTime = timeline.timeAtFrame(frame);
        frameX[static_cast<size_t>(i)] = juce::jlimit(x1, x2, sourceTimeToScreenX(sourceTime, ctx, item));
        const float gainDb = volumeEnvelope ? volumeEnvelope->evalAt(sourceTime) : 0.0f;
        // 视觉范围 -14dB ~ +8dB：0dB=1.0 原始大小，超出后饱和（防遮挡/防消失）
        const float gainFactor = juce::jlimit(0.2f, 2.5f, std::pow(10.0f, gainDb / 20.0f));
        frameAmp[static_cast<size_t>(i)] = energy[static_cast<size_t>(frame)] / clipRefMag * gainFactor;
    }

    // 每列保留首帧、最低振幅帧、最高振幅帧、末帧（去重），顶边正序、底边逆序共用。
    std::vector<std::pair<float, float>> samples;  // (px, amplitude)
    for (int f = 0; f < frameCount;)
    {
        const int groupEnd = static_cast<int>(std::upper_bound(
            frameX.begin() + f, frameX.end(), frameX[static_cast<size_t>(f)]) - frameX.begin());

        int minIndex = f;
        int maxIndex = f;
        for (int i = f + 1; i < groupEnd; ++i)
        {
            if (frameAmp[static_cast<size_t>(i)] < frameAmp[static_cast<size_t>(minIndex)]) minIndex = i;
            if (frameAmp[static_cast<size_t>(i)] > frameAmp[static_cast<size_t>(maxIndex)]) maxIndex = i;
        }

        std::array<int, 4> picks { f, minIndex, maxIndex, groupEnd - 1 };
        std::sort(picks.begin(), picks.end());
        const auto pickEnd = std::unique(picks.begin(), picks.end());
        for (auto it = picks.begin(); it != pickEnd; ++it)
            samples.emplace_back(static_cast<float>(frameX[static_cast<size_t>(*it)]),
                                 frameAmp[static_cast<size_t>(*it)]);

        f = groupEnd;
    }

    juce::Path blob;
    blob.startNewSubPath(static_cast<float>(x1), centerY);
    for (const auto& sample : samples)
        blob.lineTo(sample.first, centerY - halfH * sample.second);
    blob.lineTo(static_cast<float>(x2), centerY);
    for (auto it = samples.rbegin(); it != samples.rend(); ++it)
        blob.lineTo(it->first, centerY + halfH * it->second);
    blob.closeSubPath();

    outPath = std::move(blob);
    return true;
}

/// Reference traversal + bounded geometry: one producer scan over the viewport
/// window computes the energy min/max reference（与整绘同源的着色参考，条带
/// 补绘共用，保证同一像素颜色一致）and captures continuous voiced runs
/// （帧范围 + 频率样本）。Only runs intersecting the draw band build geometry:
/// per-frame X projection stays full-run（渐变端点为真实端点、色标口径与整绘
/// 一致），while Y and path construction cover only the band neighbourhood
/// （条带命中点段前后各夹一个点，描边跨越条带边界，边缘辉光仍覆盖
/// rasterBounds）。run 内每帧一个点，一帧一点一一对应。
/// Producer receives a sink(startFrame, data, length, gain).
/// frameToColour maps globalFrame → colour (used for CorrectedF0 gradient tinting).
template <typename SpanProducer, typename FX, typename FY, typename FC>
static std::vector<F0VisualSegment> buildF0VisualSegments(
    const std::vector<float>* originalEnergy,
    int endFrameExclusive,
    FX&& frameToX,
    FY&& frameToY,
    SpanProducer&& emitSpans,
    FC&& frameToColour,
    float bandX0, float bandX1)
{
    std::vector<F0VisualSegment> segments;

    const bool hasEnergy = originalEnergy != nullptr
        && endFrameExclusive <= static_cast<int>(originalEnergy->size());

    float minEnergy = std::numeric_limits<float>::max();
    float maxEnergy = std::numeric_limits<float>::lowest();

    // ── 参考遍历（一次扫描）：能量 min/max 参考 + 连续有声段捕获 ──
    struct F0VoicedRun
    {
        int firstFrame = 0;
        int frameCount = 0;
        std::size_t sampleBegin = 0;
    };
    std::vector<F0VoicedRun> runs;
    std::vector<float> runFrequencies;
    int runFirstFrame = 0;
    int runFrameCount = 0;
    std::size_t runSampleBegin = 0;

    auto flushRun = [&]() {
        if (runFrameCount == 0)
            return;
        runs.push_back({ runFirstFrame, runFrameCount, runSampleBegin });
        runFrameCount = 0;
    };

    emitSpans([&](int start, const float* data, int length, float gain) {
        if (!data) {
            flushRun();
            return;
        }
        for (int i = 0; i < length; ++i) {
            const float frequency = data[i] * gain;
            if (frequency < 20.0f || frequency > 2000.0f) {
                flushRun();
                continue;
            }
            const int globalFrame = start + i;
            if (hasEnergy) {
                const float energy = (*originalEnergy)[static_cast<size_t>(globalFrame)];
                if (std::isfinite(energy)) {
                    minEnergy = std::min(minEnergy, energy);
                    maxEnergy = std::max(maxEnergy, energy);
                }
            }
            if (runFrameCount == 0) {
                runFirstFrame = globalFrame;
                runSampleBegin = runFrequencies.size();
            }
            ++runFrameCount;
            runFrequencies.push_back(frequency);
        }
    });
    flushRun();

    // run 内每帧一个点：X 连续投影，一帧一点一一对应；Y 占位待条带邻域回填。
    for (const auto& run : runs) {
        const float segX0 = frameToX(run.firstFrame);
        const float segX1 = frameToX(run.firstFrame + run.frameCount - 1);
        if (segX1 < bandX0 || segX0 > bandX1)
            continue;

        F0VisualSegment segment;
        segment.points.reserve(static_cast<std::size_t>(run.frameCount));
        for (int i = 0; i < run.frameCount; ++i) {
            const int frame = run.firstFrame + i;
            const float x = frameToX(frame);
            float hotMix = 0.0f;
            if (hasEnergy) {
                const float energy = (*originalEnergy)[static_cast<std::size_t>(frame)];
                const float normalized = juce::jlimit(0.0f, 1.0f,
                    (energy - minEnergy) / juce::jmax(1e-6f, maxEnergy - minEnergy));
                hotMix = 0.34f * smootherStep(normalized);
            }
            segment.points.push_back({x, 0.0f, hotMix, frameToColour(frame)});
        }

        // path 覆盖条带命中点段并前后各夹一个点：描边跨越条带边界，边缘辉光仍覆盖 rasterBounds。
        const auto first = std::lower_bound(segment.points.begin(), segment.points.end(), bandX0,
            [](const F0VisualPoint& point, float x) { return point.x < x; });
        const auto last = std::upper_bound(segment.points.begin(), segment.points.end(), bandX1,
            [](float x, const F0VisualPoint& point) { return x < point.x; });
        constexpr int neighbours = 1;
        segment.pathFirst = std::max(0, static_cast<int>(first - segment.points.begin()) - neighbours);
        segment.pathLast = std::min(static_cast<int>(segment.points.size()) - 1,
                                    static_cast<int>(last - segment.points.begin()) + neighbours - 1);

        for (int point = segment.pathFirst; point <= segment.pathLast; ++point) {
            const std::size_t sample = run.sampleBegin + static_cast<std::size_t>(point);
            segment.points[static_cast<std::size_t>(point)].y =
                frameToY(run.firstFrame + point, runFrequencies[sample]);
        }
        segments.push_back(std::move(segment));
    }

    return segments;
}

// Convenience overload for OriginalF0 / F0 selection: no per-frame colour callback.
template <typename SpanProducer, typename FX, typename FY>
static std::vector<F0VisualSegment> buildF0VisualSegments(
    const std::vector<float>* originalEnergy,
    int endFrameExclusive,
    FX&& frameToX,
    FY&& frameToY,
    SpanProducer&& emitSpans,
    float bandX0, float bandX1)
{
    return buildF0VisualSegments(originalEnergy, endFrameExclusive,
        std::forward<FX>(frameToX), std::forward<FY>(frameToY),
        std::forward<SpanProducer>(emitSpans),
        [](int) { return juce::Colours::grey; }, bandX0, bandX1);
}

} // namespace

void PianoRollRenderer::drawUnvoicedFrameBands(juce::Graphics& g,
                                               const RenderContext& ctx,
                                               PlacedContent item)
{
    if (!ctx.showUnvoicedFrames || item.content.f0Timeline.isEmpty())
        return;
    const auto& pitchSnapshot = item.content.ownerSnapshot->pitchCurve;
    if (!pitchSnapshot || pitchSnapshot->size() == 0)
        return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    const auto& originalF0 = pitchSnapshot->getOriginalF0();
    if (originalF0.empty())
        return;

    const auto bandColour = UIColors::currentThemeId() == ThemeId::DarkBlueGrey
        ? UIColors::backgroundDark.withAlpha(0.28f)
        : UIColors::backgroundMedium.withAlpha(0.22f);
    g.setColour(bandColour);

    // Scan only the visible F0 frame range for unvoiced intervals directly.
    int unvoicedStart = -1;
    const int totalFrames = static_cast<int>(originalF0.size());
    const auto visibleFrames = item.content.f0Timeline.rangeForTimes(
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
                const double intervalStartTime = item.content.f0Timeline.timeAtFrame(unvoicedStart);
                const double intervalEndTime = item.content.f0Timeline.timeAtFrame(frame);

                if (intervalEndTime > visibleWindow.visibleContentStartTime &&
                    intervalStartTime < visibleWindow.visibleContentEndTime) {
                    const int x1 = sourceTimeToScreenX(intervalStartTime, ctx, item);
                    const int x2 = sourceTimeToScreenX(intervalEndTime, ctx, item);

                    if (x2 > ctx.contentStartX && x1 < ctx.width) {
                        const float drawX = static_cast<float>(std::max(x1, ctx.contentStartX));
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
        const double intervalStartTime = item.content.f0Timeline.timeAtFrame(unvoicedStart);
        const double intervalEndTime = item.content.f0Timeline.timeAtFrame(endFrameExclusive);

        if (intervalEndTime > visibleWindow.visibleContentStartTime &&
            intervalStartTime < visibleWindow.visibleContentEndTime) {
            const int x1 = sourceTimeToScreenX(intervalStartTime, ctx, item);
            const int x2 = sourceTimeToScreenX(intervalEndTime, ctx, item);

            if (x2 > ctx.contentStartX && x1 < ctx.width) {
                const float drawX = static_cast<float>(std::max(x1, ctx.contentStartX));
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
                                      PlacedContent item,
                                      const WaveformMipmap& waveform)
{
    const double sampleRate = item.content.committedSnapshot->audioSampleRate;
    if (sampleRate <= 0.0)
        return;

    auto damageContext = ctx;
    if (!ctx.rasterBounds.isEmpty())
        damageContext.rasterBounds = ctx.rasterBounds.expanded(2, 0);
    const auto visibleWindow = computeVisibleTimeWindow(damageContext, item);
    if (!visibleWindow.isValid())
        return;

    const int startX = visibleWindow.viewportStartX;
    const int endX = visibleWindow.viewportEndX;
    const int w = endX - startX;
    if (w <= 0) return;

    const double contentVisibleDuration = visibleWindow.visibleContentEndTime - visibleWindow.visibleContentStartTime;
    if (contentVisibleDuration <= 0.0) return;

    // Clip pixel loop to segment timeline bounds. Without this, pixels outside
    // [timelineStartSeconds, timelineEndSeconds] get clamped by tauInverse to
    // boundary peaks, drawing a solid bar of waveform amplitude beyond the
    // segment edges (visible when T_start > 0, e.g. capture segments).
    const int segStartX = ctx.coords.timeToX(item.placement.projection.timelineStartSeconds);
    const int segEndX = ctx.coords.timeToX(item.placement.projection.timelineEndSeconds());
    const int drawStartX = std::max(startX, segStartX);
    const int drawEndX = std::min(endX, segEndX);
    if (drawEndX <= drawStartX) return;

    // 波形是时间参考，垂直居中在当前视口，不跟随音高滚动或纵向缩放。
    const float centerY = ctx.height / 2.0f;
    const float amplitudeScale = ctx.height / 2.0f;

    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
    const bool isOverdose = themeId == ThemeId::Overdose;

    juce::Path waveformPath;

    // Invert the output -> source mapping (tau_inverse) so the screen X axis
    // (output time) reads from the SOURCE peaks at the tau-inverted time.
    jassert(item.content.committedSnapshot->timeGrid);
    const PianoRollTimeMap map(item.placement.projection,
                               *item.content.committedSnapshot->timeGrid, ctx.coords);

    double sourceStart = map.xToSource(drawStartX);
    for (int x = drawStartX; x < drawEndX; ++x)
    {
        const double sourceEnd = map.xToSource(x + 1);
        const auto peak = waveform.queryExactPeak(
            TimeCoordinate::secondsToSamplesFloor(sourceStart, sampleRate),
            TimeCoordinate::secondsToSamplesCeil(sourceEnd, sampleRate));
        sourceStart = sourceEnd;
        if (!peak || peak->isZero())
            continue;

        const float yMin = centerY - peak->getMax() * amplitudeScale;
        const float yMax = centerY - peak->getMin() * amplitudeScale;
        const float fx = static_cast<float>(x) + 0.5f;
        const float padding = 0.5f * std::max(0.0f, 1.0f - (yMax - yMin));

        waveformPath.startNewSubPath(fx, yMin - padding);
        waveformPath.lineTo(fx, yMax + padding);
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

    const bool isBlueBreeze = UIColors::currentThemeId() == ThemeId::BlueBreeze;
    const bool isOverdose = UIColors::currentThemeId() == ThemeId::Overdose;
    const bool isLightTheme = isBlueBreeze || isOverdose;
    const auto& overdoseAtlas = UiAssets::get(UiAssetId::PianoKeyAtlas, g);
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

    // Compute effective note name display mode (zoom-adaptive downgrade)
    int effectiveNoteNameMode = static_cast<int>(ctx.noteNameMode); // 0=ShowAll, 1=COnly, 2=Hide
    if (effectiveNoteNameMode == 0 && ctx.pixelsPerSemitone < kShowAllLabelsMinPPS)
        effectiveNoteNameMode = 1; // downgrade to C-only
    if (effectiveNoteNameMode <= 1 && ctx.pixelsPerSemitone < kShowCOnlyMinPPS)
        effectiveNoteNameMode = 2; // downgrade to hidden

    // Note name 升降号选择沿用 scale 配置；键盘材质恒为物理黑白键。
    const bool useFlats = ctx.scaleType != kScaleTypeChromatic
        && kUseFlatsByRoot[juce::jlimit(0, 11, ctx.scaleRootNote)];

    const auto drawNoteLabel = [&](int drawMidi, float y, float h, bool isBlackKey) {
        const int noteInOctave = drawMidi % 12;
        if (effectiveNoteNameMode != 0
            && (effectiveNoteNameMode != 1 || noteInOctave != 0))
            return;

        g.setFont(UIColors::getUIFontExact(kNoteLabelFontSize, juce::Font::bold));
        const int octave = (drawMidi / 12) - 1;
        const char* name = useFlats ? kFlatNames[noteInOctave] : kSharpNames[noteInOctave];
        const juce::String noteName = juce::String(name) + juce::String(octave);
        const int tx = 0;
        const int ty = static_cast<int>(y);
        const int tw = w - 4;
        const int th = static_cast<int>(h);

        g.setColour(isBlackKey
            ? juce::Colours::white.withAlpha(0.7f)
            : juce::Colours::black.withAlpha(0.5f));
        for (int ox = -1; ox <= 1; ++ox)
            for (int oy = -1; oy <= 1; ++oy)
                if (ox != 0 || oy != 0)
                    g.drawText(noteName, tx + ox, ty + oy, tw, th, juce::Justification::centredRight);

        const juce::Colour labelColour = isBlackKey
            ? juce::Colour(0xFF2A2A2A)
            : (isLightTheme ? juce::Colour(0xFF25303A).withMultipliedAlpha(0.90f)
                            : juce::Colour(0xFFE0E0E0));
        g.setColour(labelColour);
        g.drawText(noteName, tx, ty, tw, th, juce::Justification::centredRight);
    };

    if (ctx.labelsOnly)
    {
        for (int midi = static_cast<int>(ctx.minMidi); midi <= static_cast<int>(ctx.maxMidi); ++midi)
        {
            const float y = ctx.coords.midiToY(static_cast<float>(midi));
            if (y < -50.0f || y > height + 50.0f)
                continue;
            const bool isBlackKey = isPhysicalBlackKey(midi);
            drawNoteLabel(midi, y, ctx.pixelsPerSemitone, isBlackKey);
        }
        return;
    }

    g.setColour(isLightTheme ? UIColors::keyBedWhite : UIColors::backgroundDark);
    g.fillRect(0, 0, w, height);

    juce::Colour cWhite1 = isLightTheme ? UIColors::keyBedWhite : juce::Colour(0xFFF7F9F9);
    juce::Colour cWhite2 = isOverdose ? juce::Colour { Overdose::Colors::KeyBedWhiteBottom }
                              : (isBlueBreeze ? juce::Colour { BlueBreeze::Colors::KeyBedBottom } : juce::Colour(0xFFECF0F1));

    juce::Colour cBlackBottom = isOverdose ? juce::Colour { Overdose::Colors::KeyBedBlackBottom }
                                  : (isBlueBreeze ? juce::Colour { BlueBreeze::Colors::KeyBlackBottom } : juce::Colour(0xFF1B2026));


    for (int midi = static_cast<int>(ctx.minMidi); midi <= static_cast<int>(ctx.maxMidi); ++midi)
    {
        int drawMidi = midi;
        float y = ctx.coords.midiToY(static_cast<float>(drawMidi));
        float h = ctx.pixelsPerSemitone;

        if (y < -50.0f || y > height + 50.0f) continue;

        const bool isBlackKey = isPhysicalBlackKey(drawMidi);

        float drawH = h + 1.0f;

        if (!isBlackKey)
        {
            // Physical white key: full-width rectangle
            juce::Rectangle<float> keyRect(0.0f, y, static_cast<float>(w), drawH);

            juce::ColourGradient grad(cWhite1,
                                      static_cast<float>(0),
                                      y,
                                      cWhite2.interpolatedWith(UIColors::keyBedDivider, isLightTheme ? 0.08f : 0.0f),
                                      static_cast<float>(0 + w),
                                      y + drawH,
                                      false);
            if (isLightTheme)
                grad.addColour(0.16, cWhite1.interpolatedWith(juce::Colours::white, 0.12f));
            g.setGradientFill(grad);
            g.fillRect(keyRect);

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
            drawNoteLabel(drawMidi, y, h, false);
        }
        else
        {
            // Physical black key: extension

            juce::Rectangle<float> extensionRect(blackKeyW, y, static_cast<float>(w) - blackKeyW, drawH);

            juce::ColourGradient grad(cWhite1,
                                      static_cast<float>(0) + blackKeyW,
                                      y,
                                      cWhite2.interpolatedWith(UIColors::keyBedDivider, isLightTheme ? 0.08f : 0.0f),
                                      static_cast<float>(0 + w),
                                      y + drawH,
                                      false);
            g.setGradientFill(grad);
            g.fillRect(extensionRect);

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

        const int noteInOctave = ((drawMidi % 12) + 12) % 12;
        const bool isBlackKey = isPhysicalBlackKey(drawMidi);

        if (isBlackKey)
        {
            float keyH = h * 0.8f;
            float keyY = y + (h - keyH) * 0.5f;

            juce::Rectangle<float> keyRect(0.0f, keyY, blackKeyW, keyH);

            // Physical black key material (atlas + gradient + shadow)
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
                    g.setColour(juce::Colour(Overdose::Colors::PanelBorder).withAlpha(0.32f));
                    g.drawRoundedRectangle(keyRect.reduced(0.5f), 2.0f, 1.0f);
                }
                else
                {
                    g.setColour(juce::Colours::black.withAlpha(isLightTheme ? 0.50f : 0.6f));
                    g.drawRoundedRectangle(keyRect.reduced(0.5f), 2.0f, 1.0f);
                }
            }

            // Note name labels for black keys (drawn on top of the black key body with outline)
            drawNoteLabel(drawMidi, y, h, true);
        }
    }

    g.setColour(isLightTheme ? UIColors::keyBedDivider.withAlpha(0.84f) : UIColors::panelBorder);
    g.drawVerticalLine(w, 0.0f, static_cast<float>(height));
}

struct NoteDisplayStyle
{
    juce::Colour colour;
    float bodyAlpha = 0.72f;
};

// HiFiGAN vocoder: colour each note relative to the nearest chromatic pitch of its raw OriginalF0.
static NoteDisplayStyle vocoderShiftedColour(
    juce::Colour base,
    const Note& note,
    const PitchCurveSnapshot& snap,
    const F0Timeline& tl)
{
    if (snap.isEmpty())
        return { base };

    const auto range = tl.rangeForTimes(note.startTime, note.endTime);
    const auto& originalF0 = snap.getOriginalF0();
    std::vector<float> voicedOriginalF0;
    voicedOriginalF0.reserve(static_cast<size_t>(range.endFrameExclusive - range.startFrame));
    for (int f = range.startFrame; f < range.endFrameExclusive; ++f) {
        if (f < 0 || f >= static_cast<int>(originalF0.size())) continue;
        const float original = originalF0[static_cast<size_t>(f)];
        if (original > 0.0f) voicedOriginalF0.push_back(original);
    }
    if (voicedOriginalF0.empty()) return { base };
    std::sort(voicedOriginalF0.begin(), voicedOriginalF0.end());
    const float standardMidi = std::round(PitchUtils::freqToMidi(
        voicedOriginalF0[voicedOriginalF0.size() / 2]));

    // Corrected segment F0 is in the Effective domain; compare it to the raw OriginalF0 standard pitch
    // so global pitch shift is visible as an up/down displacement from the original reference.
    float totalSignedCents = 0.0f;
    int count = 0;
    for (int f = range.startFrame; f < range.endFrameExclusive; ++f) {
        if (f < 0 || f >= static_cast<int>(snap.size())) continue;
        const float original = originalF0[static_cast<size_t>(f)];
        if (original <= 0.0f) continue;
        float corrected = 0.0f;
        for (const auto& seg : snap.getCorrectionSegments()) {
            if (f >= seg.startFrame && f < seg.endFrame) {
                const int off = f - seg.startFrame;
                if (off < static_cast<int>(seg.f0Data.size()) && seg.f0Data[off] > 0.0f)
                    corrected = seg.f0Data[off];
                break;
            }
        }
        if (corrected <= 0.0f) continue;
        const float cents = 100.0f * (PitchUtils::freqToMidi(corrected) - standardMidi);
        totalSignedCents += cents;
        ++count;
    }
    if (count == 0) return { base };

    const float avgSignedCents = totalSignedCents / static_cast<float>(count);
    if (avgSignedCents == 0.0f) return { base };
    const float avgDistanceCents = std::abs(avgSignedCents);

    constexpr float kMinCents = 75.0f;
    constexpr float kMaxCents = 300.0f;
    const float t = juce::jlimit(0.0f, 1.0f, (avgDistanceCents - kMinCents) / (kMaxCents - kMinCents));

    static const juce::Colour kWarmTarget(0xFFFFA040);
    static const juce::Colour kCoolTarget(0xFF40A0FF);

    const auto target = avgSignedCents < 0.0f
        ? base.interpolatedWith(kWarmTarget, t)
        : base.interpolatedWith(kCoolTarget, t);

    const auto colour = base.interpolatedWith(target, t);
    const float blueShiftMix = avgSignedCents > 0.0f ? t * t : 0.0f;
    return { colour, 0.72f - 0.18f * blueShiftMix };
}

// Compute the final display colour for a note, fully reusing drawNotes logic:
// base = EQ active ? placement.displayColour.darker(0.30f) : placement.displayColour
// if vocoder-needed, shift via vocoderShiftedColour; otherwise return base.
static NoteDisplayStyle computeNoteDisplayStyle(
    PianoRollRenderer::PlacedContent item,
    const Note& note)
{
    const auto base = (note.eq.has_value() && note.eq->active)
        ? item.placement.displayColour.darker(0.30f)
        : item.placement.displayColour;
    const auto& pitchSnapshot = item.content.ownerSnapshot->pitchCurve;
    if (pitchSnapshot
        && pitchSnapshot->noteNeedsVocoder(note.startTime, note.endTime, item.content.f0Timeline))
        return vocoderShiftedColour(base, note, *pitchSnapshot, item.content.f0Timeline);
    return { base };
}

void PianoRollRenderer::drawNotes(juce::Graphics& g,
                                  const RenderContext& ctx,
                                  PlacedContent item)
{
    const auto& notes = *item.content.displayNotes;
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
    if (item.content.notesPrimaryScheme)
    {
        const auto& pitchSnapshot = item.content.ownerSnapshot->pitchCurve;
        // blob 是主音符图形：只依赖持久 originalEnergy + F0Timeline，不依赖可选 PCM/mipmap
        if (!pitchSnapshot || item.content.f0Timeline.isEmpty())
            return;

        // clip 级振幅参考值随能量一起发布在快照内，绘制期不再扫描整段 energy
        const float clipRefMag = pitchSnapshot->getClipRefMag();
        if (clipRefMag <= 0.0f)
            return;  // 空能量/全静音 clip，跳过 blob 绘制
        const auto& energy = pitchSnapshot->getOriginalEnergy();

        // blob 音量包络由 owner 在构建 item 时决定（含拖拽预览）
        const AutomationLane* volumeEnvelope = item.content.volumeEnvelope;

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

            const auto drawColour = computeNoteDisplayStyle(item, note).colour;

            // 顶边 + 底边闭合 Path：X 用 source-time→timeline→screen 投影
            juce::Path blob;
            if (!buildNoteBlobPath(note, ctx, item, energy, item.content.f0Timeline,
                                   clipRefMag, volumeEnvelope, centerY, halfH, x1, x2, blob))
                continue;

            // 能量自适应纵向渐变：像素列分组共享渐变，过渡带亮度和宽窄均随局部 energy 缩放
            {
                const auto fillTop = drawColour.darker(0.22f).withAlpha(0.32f);
                const auto fillBottom = drawColour.darker(0.22f).withAlpha(0.32f);
                const auto glowCore = drawColour
                    .interpolatedWith(juce::Colours::white, 0.88f)
                    .withAlpha(0.95f);

                g.saveState();
                g.reduceClipRegion(blob);

                // 原始列相位锚在音符端点 x(j)=x1+2j，damage 只裁填充范围；
                // 相邻列取到同一能量帧时合并为一段渐变，不再每 2 逻辑像素重建渐变。
                const int fillStart = std::max(x1, visibleWindow.viewportStartX);
                const int fillEnd = std::min(x2, visibleWindow.viewportEndX);
                if (fillEnd > fillStart)
                {
                    const int totalColumns = (x2 - x1 + 1) / 2;
                    // 组尾搜索上界 = 最后一个待填充列（左边界 < fillEnd）；相位仍锚 x1。
                    const int fillLimit = std::min(totalColumns, (fillEnd - x1 + 1) / 2);
                    const auto baselineFrame = [&](int column) {
                        const float x = static_cast<float>(x1 + 2 * column);
                        const float srcTime = static_cast<float>(note.startTime
                            + (note.endTime - note.startTime)
                              * ((x - static_cast<float>(x1))
                                 / static_cast<float>(x2 - x1)));
                        return item.content.f0Timeline.frameAtOrBefore(srcTime);
                    };

                    int column = (fillStart - x1) / 2;
                    while (column < fillLimit)
                    {
                        const int frame = baselineFrame(column);
                        const float normEnergy = (frame >= 0
                            && frame < static_cast<int>(energy.size()))
                            ? juce::jlimit(0.0f, 1.0f,
                                energy[static_cast<size_t>(frame)] / clipRefMag)
                            : 0.0f;

                        int nextColumn = column + 1;
                        while (nextColumn < fillLimit && baselineFrame(nextColumn) == frame)
                            ++nextColumn;

                        const float segLeft = static_cast<float>(x1 + 2 * column);
                        const float segRight = static_cast<float>(std::min(x2, x1 + 2 * nextColumn));
                        const float drawLeft = std::max(segLeft, static_cast<float>(fillStart));
                        const float drawRight = std::min(segRight, static_cast<float>(fillEnd));
                        if (drawRight > drawLeft)
                        {
                            const auto glowTrans = drawColour
                                .interpolatedWith(juce::Colours::white, 0.35f * normEnergy)
                                .withAlpha(0.65f + 0.05f * normEnergy);

                            // 过渡带宽窄自适应：normEnergy=0 → 亮带20%（窄），=1 → 亮带60%（宽）
                            const float bw = 0.20f + 0.40f * normEnergy;
                            const float tLo = 0.50f - bw * 0.5f;
                            const float tHi = 0.50f + bw * 0.5f;
                            const float trLo = tLo * 0.5f;
                            const float trHi = tHi + (1.0f - tHi) * 0.5f;

                            const float rectH = halfH;
                            juce::ColourGradient col(fillTop, segLeft, centerY - rectH,
                                                     fillBottom, segLeft, centerY + rectH, false);
                            col.addColour(trLo, fillTop);
                            col.addColour(tLo,  glowTrans);
                            col.addColour(0.50f, glowCore);
                            col.addColour(tHi,  glowTrans);
                            col.addColour(trHi, fillBottom);

                            g.setGradientFill(col);
                            g.fillRect(drawLeft, centerY - rectH, drawRight - drawLeft, rectH * 2.0f);
                        }

                        column = nextColumn;
                    }
                }

                g.restoreState();
            }

            g.setColour(drawColour.brighter(0.45f).withAlpha(0.55f));
            g.strokePath(blob, juce::PathStrokeType(0.9f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

            if (note.eq.has_value() && note.eq->active)
            {
                g.setColour(item.placement.displayColour.withAlpha(0.85f));
                g.strokePath(blob, juce::PathStrokeType(2.0f,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
            }
        }

        return;
    }

    // ── OpenTune 矩形音符（使用 track theme colour 联动） ───────
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

        int x1 = sourceTimeToScreenX(note.startTime, ctx, item);
        int x2 = sourceTimeToScreenX(note.endTime,   ctx, item);
        if (x2 <= visibleWindow.viewportStartX || x1 >= visibleWindow.viewportEndX)
            continue;

        float w = std::max(1.0f, static_cast<float>(x2 - x1));
        auto noteBounds = juce::Rectangle<float>(static_cast<float>(x1), y, w, h);

        const auto noteStyle = computeNoteDisplayStyle(item, note);
        const auto& drawColor = noteStyle.colour;

        if (isAurora)
        {
            g.setColour(drawColor.withAlpha(noteStyle.bodyAlpha));
            g.fillRect(noteBounds);

            auto topSheenBounds = noteBounds.withHeight(juce::jmin(noteBounds.getHeight() * 0.42f, 7.0f));
            juce::ColourGradient topSheen(drawColor.brighter(0.58f).withAlpha(0.12f),
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
                g.setColour(item.placement.displayColour.withAlpha(0.85f));
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
            g.setColour(drawColor.withAlpha(noteStyle.bodyAlpha));
            g.fillRect(noteBounds);

            auto topSheenBounds = noteBounds.withHeight(juce::jmin(noteBounds.getHeight() * 0.42f, 6.0f));
            juce::ColourGradient topSheen(drawColor.brighter(0.42f).withAlpha(0.10f),
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
                g.setColour(item.placement.displayColour.withAlpha(0.85f));
                g.drawRect(noteBounds.expanded(1.5f), 2.0f);
            }
        }
        else
        {
            g.setColour(drawColor.withAlpha(noteStyle.bodyAlpha));
            g.fillRect(noteBounds);

            g.setColour(UIColors::noteBlockBorder.withAlpha(0.50f));
            g.drawRect(noteBounds, 1.0f);

            if (note.eq.has_value() && note.eq->active)
            {
                g.setColour(item.placement.displayColour.withAlpha(0.85f));
                g.drawRect(noteBounds.expanded(1.0f), 2.0f);
            }
        }
    }

}

void PianoRollRenderer::drawSelectedNoteHighlights(juce::Graphics& g,
                                                    const RenderContext& ctx,
                                                    PlacedContent item,
                                                    const std::vector<int>& selectedNoteIndices,
                                                    bool asPlayheadHighlight)
{
    const auto& notes = *item.content.displayNotes;
    if (notes.empty() || selectedNoteIndices.empty())
        return;

    const auto visibleWindow = computeVisibleTimeWindow(ctx, item);
    if (!visibleWindow.isValid())
        return;

    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;

    // ── OpenDyne：选中高亮直接叠加在波形 blob 上（blob 几何与 drawNotes 完全一致） ──
    if (item.content.notesPrimaryScheme)
    {
        const auto& pitchSnapshot = item.content.ownerSnapshot->pitchCurve;
        // blob 是主音符图形：只依赖持久 originalEnergy + F0Timeline，不依赖可选 PCM/mipmap
        if (!pitchSnapshot || item.content.f0Timeline.isEmpty())
            return;

        const auto& energy = pitchSnapshot->getOriginalEnergy();

        // blob 音量包络由 owner 在构建 item 时决定（含拖拽预览），与 drawNotes 一致
        const AutomationLane* volumeEnvelope = item.content.volumeEnvelope;

        // clip 级振幅参考值：与 drawNotes 一致，读取快照参考值
        const float clipRefMag = pitchSnapshot->getClipRefMag();
        if (clipRefMag <= 0.0f)
            return;  // 空能量/全静音 clip，无 blob 可高亮

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
            if (!buildNoteBlobPath(note, ctx, item, energy, item.content.f0Timeline,
                                   clipRefMag, volumeEnvelope, centerY, halfH, x1, x2, blob))
                continue;

            // 播放头高亮采用主题强调色，与真实选中态的轨道色描边区分。
            const auto highlightColour = asPlayheadHighlight
                ? UIColors::accent
                : item.placement.displayColour;
            g.setColour(asPlayheadHighlight
                ? highlightColour.withAlpha(0.16f)
                : highlightColour.brighter(0.55f).withAlpha(0.45f));
            g.fillPath(blob);

            g.setColour(asPlayheadHighlight
                ? highlightColour.withAlpha(0.95f)
                : highlightColour.brighter(0.75f).withAlpha(0.90f));
            g.strokePath(blob, juce::PathStrokeType(asPlayheadHighlight ? 1.4f : 1.8f,
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

        // 播放头高亮用主题强调色，真实选中态继续使用轨道色。
        const auto highlightColour = asPlayheadHighlight
            ? UIColors::accent
            : item.placement.displayColour;
        g.setColour(asPlayheadHighlight
            ? highlightColour.withAlpha(0.16f)
            : highlightColour.brighter(0.55f).withAlpha(isAurora ? 0.30f : 0.25f));
        g.fillRect(noteBounds);

        g.setColour(asPlayheadHighlight
            ? highlightColour.withAlpha(0.95f)
            : highlightColour.brighter(0.75f).withAlpha(isAurora ? 0.90f : 0.85f));
        g.drawRect(noteBounds, asPlayheadHighlight ? 1.4f : (isAurora ? 1.5f : 1.2f));
    }
}
// ============================================================================
// TimeGrid Anchors (cached slot — neutral lines, no interaction)
// ============================================================================
void PianoRollRenderer::drawTimeGridAnchors(juce::Graphics& g, const RenderContext& ctx, PlacedContent item)
{
    jassert(item.content.committedSnapshot->timeGrid);

    const int contentTop    = ctx.rulerHeight;
    const int contentBottom = ctx.height;
    if (contentBottom <= contentTop) return;

    const auto& grid = *item.content.committedSnapshot->timeGrid;
    for (const auto& h : grid.handles()) {
        const double timelineTime = item.placement.projection.projectContentTimeToTimeline(h.output_seconds);
        const int x = ctx.coords.timeToX(timelineTime);
        if (x < ctx.contentStartX || x >= ctx.width) continue;

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
void PianoRollRenderer::drawTimeGridHandles(juce::Graphics& g, const RenderContext& ctx,
                                            const ContentTimelineProjection& projection,
                                            const TimeGridSnapshot& grid)
{
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

    for (const auto& h : grid.handles()) {
        const bool selected = (ctx.timeGridSelectedHandleId == h.id);
        const bool hovered  = (ctx.timeGridHoveredHandleId  == h.id);
        const bool isAdditional = std::find(ctx.additionalSelectedHandleIds.begin(), ctx.additionalSelectedHandleIds.end(), h.id) != ctx.additionalSelectedHandleIds.end();
        if (!selected && !hovered && !isAdditional) continue;

        const double timelineTime = projection.projectContentTimeToTimeline(h.output_seconds);
        const int x = ctx.coords.timeToX(timelineTime);
        if (x < ctx.contentStartX || x >= ctx.width) continue;

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
                                                  PlacedContent item)
{
    if (item.content.f0Timeline.isEmpty()) return;
    const auto& pitchSnapshot = item.content.ownerSnapshot->pitchCurve;
    if (!pitchSnapshot || pitchSnapshot->size() == 0) return;
    if (item.content.notesPrimaryScheme) return;   // OpenDyne：notes-primary 无 F0 框选
    if (!ctx.showOriginalF0) return;
    if (!ctx.hasF0Selection || ctx.f0SelectionRanges.empty()) return;

    if (!item.placement.projection.isValid()) return;

    const std::vector<float>* originalEnergy = &pitchSnapshot->getOriginalEnergy();
    if (originalEnergy->empty()) originalEnergy = nullptr;

    const PianoRollTimeMap map(item.placement.projection,
                               *item.content.committedSnapshot->timeGrid, ctx.coords);
    const auto makeFrameToX = [&](int frame) -> float {
        return static_cast<float>(map.sourceToXExact(item.content.f0Timeline.timeAtFrame(frame)));
    };

    auto makeFrameToY = [&](int, float frequency) -> float {
        return ctx.coords.freqToY(frequency);
    };

    // 帧区间：全视口 [contentStartX, width] 上对单调不减的连续投影 X(f) 正向二分，
    // 首末帧各外扩 marginFrames 并 clamp；不做时间反算。
    const auto [startFrame, endFrame] = visibleFrameRange(map, item.content.f0Timeline,
        static_cast<int>(pitchSnapshot->size()), ctx.contentStartX, ctx.width);

    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
    const bool isOverdose = themeId == ThemeId::Overdose;

    const juce::Colour selectionColour = ctx.originalF0Colour.brighter(isAurora ? 0.18f : 0.14f);
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

        const auto& originalF0 = pitchSnapshot->getOriginalF0();
        const int origEnd = std::min(selEnd, static_cast<int>(originalF0.size()));
        if (origEnd <= selStart) continue;

        auto selectionProducer = [&](auto&& sink) {
            sink(selStart, originalF0.data() + selStart, origEnd - selStart, 1.0f);
        };

        // 条带 = 全域：所有段命中，行为与基线一致。
        const auto visualSegments = buildF0VisualSegments(
            originalEnergy, origEnd, makeFrameToX, makeFrameToY, selectionProducer,
            std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max());

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
            appendLinearF0Path(runPath, segment.points, segment.pathFirst, segment.pathLast);

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
                                     PlacedContent item)
{
    if (item.content.f0Timeline.isEmpty())
        return;

    const auto& pitchSnapshot = item.content.ownerSnapshot->pitchCurve;
    if (!pitchSnapshot || pitchSnapshot->size() == 0)
        return;

    if (!ctx.showOriginalF0 && !ctx.showCorrectedF0)
        return;

    // 绘制条带（屏幕 X 域，扩描边/辉光余量）决定段级命中域；全视口帧区间固定
    // 着色参考，命中段渐变使用真实端点，折线只覆盖条带周边且条带内与整绘一致。
    // X 域扩边先于时间映射——TimeGrid 压缩下按帧数换算会低估帧到条带边界的跨屏距离。
    constexpr float kStrokeGlowPx = 3.0f;  // Aurora 修正曲线 glow 半宽 2.475px 为全部 F0 线宽上界
    // 不钳到视口：中心在视口外半段的 glow 仍覆盖视口边缘列，段命中判定允许越界
    const float bandX0 = static_cast<float>(
        (ctx.rasterBounds.isEmpty() ? ctx.contentStartX : ctx.rasterBounds.getX())
            - static_cast<int>(kStrokeGlowPx));
    const float bandX1 = static_cast<float>(
        (ctx.rasterBounds.isEmpty() ? ctx.width : std::min(ctx.rasterBounds.getRight(), ctx.width))
            + static_cast<int>(kStrokeGlowPx));

    const std::vector<float>* originalEnergy = &pitchSnapshot->getOriginalEnergy();
    if (originalEnergy->empty()) originalEnergy = nullptr;

    const PianoRollTimeMap map(item.placement.projection,
                               *item.content.committedSnapshot->timeGrid, ctx.coords);
    const auto makeFrameToX = [&](int frame) -> float {
        return static_cast<float>(map.sourceToXExact(item.content.f0Timeline.timeAtFrame(frame)));
    };

    auto makeFrameToY = [&](int, float frequency) -> float {
        return ctx.coords.freqToY(frequency);
    };

    // 帧区间：全视口 [contentStartX, width] 上对单调不减的连续投影 X(f) 正向二分，
    // 首末帧各外扩 marginFrames 并 clamp；不做时间反算。
    const auto [startFrame, endFrame] = visibleFrameRange(map, item.content.f0Timeline,
        static_cast<int>(pitchSnapshot->size()), ctx.contentStartX, ctx.width);

    const auto themeId = UIColors::currentThemeId();
    const bool isAurora = themeId == ThemeId::Aurora;
    const bool isBlueBreeze = themeId == ThemeId::BlueBreeze;
    const bool isOverdose = themeId == ThemeId::Overdose;

    const auto makeFrameToColour = [&]() {
        const auto& notes = *item.content.displayNotes;
        return [&, noteIt = notes.begin(), noteEnd = notes.end(), hasNoteColour = false,
                currentNoteColour = item.placement.displayColour](int globalFrame) mutable -> juce::Colour {
            const double timeSec = item.content.f0Timeline.timeAtFrame(globalFrame);
            while (noteIt != noteEnd && noteIt->endTime <= timeSec) {
                ++noteIt;
                hasNoteColour = false;
            }
            if (noteIt != noteEnd && noteIt->startTime <= timeSec) {
                if (!hasNoteColour) {
                    currentNoteColour = computeNoteDisplayStyle(item, *noteIt).colour;
                    hasNoteColour = true;
                }
                return currentNoteColour;
            }
            return item.placement.displayColour;
        };
    };

    struct OklabColour { float l, a, b; };
    const auto toOklab = [](juce::Colour colour) {
        const auto linearise = [](float channel) {
            const float srgb = channel / 255.0f;
            return srgb <= 0.04045f ? srgb / 12.92f
                                    : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
        };
        const float r = linearise(static_cast<float>(colour.getRed()));
        const float g = linearise(static_cast<float>(colour.getGreen()));
        const float b = linearise(static_cast<float>(colour.getBlue()));
        const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
        const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
        const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
        return OklabColour {
            0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s
        };
    };
    const auto curveContrastColour = [&](juce::Colour noteColour, juce::Colour curveColour, float opacity) {
        const auto noteLab = toOklab(noteColour);
        const auto curveLab = toOklab(curveColour);
        const float deltaL = noteLab.l - curveLab.l;
        const float deltaA = noteLab.a - curveLab.a;
        const float deltaB = noteLab.b - curveLab.b;
        // Only add the outline when the main curve is perceptually close to its note fill.
        const bool lowContrast = std::sqrt(deltaL * deltaL + deltaA * deltaA + deltaB * deltaB) < 0.08f;
        const juce::Colour outline = noteLab.l < 0.55f
            ? juce::Colour(0xFFE8EDF2)
            : juce::Colour(0xFF101820);
        return outline.withAlpha(lowContrast ? opacity : 0.0f);
    };
    // 渐变色标稀疏（防长段 O(n²)：ColourGradient::addColour 顺序扫描插入，色标数
    // 决定其复杂度）：首末色标固定在位置 0 与 1（真实端点色，窄段不产生 <1 的
    // 末端位置或同位重复色标）；内部色标按 ≥2px 屏幕间隔统一采样，2px 内的颜色
    // 迁移由下一个色标承接。几何点列不变，只影响渐变色标数。
    const auto buildSparseGradient = [](const auto& points, float leftX, float rightX,
                                        auto&& colourAtPoint) {
        juce::ColourGradient gradient;
        gradient.isRadial = false;
        gradient.point1 = { leftX, 0.0f };
        gradient.point2 = { rightX, 0.0f };
        gradient.addColour(0.0, colourAtPoint(points.front()));
        const float span = rightX - leftX;
        if (span > 0.0f) {
            float lastStopX = points.front().x;
            for (std::size_t i = 1; i + 1 < points.size(); ++i) {
                const auto& point = points[i];
                if (point.x - lastStopX >= 2.0f) {
                    gradient.addColour(static_cast<double>((point.x - leftX) / span),
                                       colourAtPoint(point));
                    lastStopX = point.x;
                }
            }
        }
        gradient.addColour(1.0, colourAtPoint(points.back()));
        return gradient;
    };
    const auto buildContrastGradient = [&](const auto& points, float leftX, float rightX, float opacity,
                                           const auto& curveColourAtPoint) {
        return buildSparseGradient(points, leftX, rightX, [&](const auto& point) {
            return curveContrastColour(point.noteColour, curveColourAtPoint(point), opacity);
        });
    };

    static const juce::Colour kLevelHotGold { 0xFFFFC24A };
    const auto blendLevelHotColour = [&](float hm) {
        const float hotMix = juce::jlimit(0.0f, 0.42f, hm);
        return ctx.correctedF0Colour.interpolatedWith(kLevelHotGold, hotMix);
    };

    // Draw original F0 (thin) — single contiguous span producer
    if (ctx.showOriginalF0 && !item.content.notesPrimaryScheme) {
        const auto& originalF0 = pitchSnapshot->getOriginalF0();
        const int origStart = startFrame;
        const int origEnd = std::min(endFrame, static_cast<int>(originalF0.size()));

        auto originalProducer = [&](auto&& sink) {
            if (origStart < origEnd)
                sink(origStart, originalF0.data() + origStart, origEnd - origStart, 1.0f);
        };

        auto originalFrameToColour = makeFrameToColour();
        const auto visualSegments = buildF0VisualSegments(
            originalEnergy, origEnd, makeFrameToX, makeFrameToY, originalProducer,
            originalFrameToColour, bandX0, bandX1);

        const juce::Colour colour = ctx.originalF0Colour;
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
                    g.setColour(curveContrastColour(p.noteColour, colour, 0.46f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(colour.withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                } else if (isBlueBreeze || isOverdose) {
                    g.setColour(colour.withAlpha(alpha * 0.055f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(curveContrastColour(p.noteColour, colour, 0.40f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(colour.withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                } else {
                    g.setColour(curveContrastColour(p.noteColour, colour, 0.38f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(colour.withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                }
                continue;
            }

            juce::Path runPath;
            appendLinearF0Path(runPath, segment.points, segment.pathFirst, segment.pathLast);

            if (isAurora) {
                g.setColour(colour.withAlpha(alpha * 0.080f));
                g.strokePath(runPath, glowStrokeType);
                const auto edgeGradient = buildContrastGradient(segment.points, segment.points.front().x,
                    segment.points.back().x, 0.46f, [&](const auto&) { return ctx.originalF0Colour; });
                g.setGradientFill(edgeGradient);
                g.strokePath(runPath, innerGlowStrokeType);
                g.setColour(colour.withAlpha(1.0f));
                g.strokePath(runPath, strokeType);
            } else if (isBlueBreeze || isOverdose) {
                g.setColour(colour.withAlpha(alpha * 0.055f));
                g.strokePath(runPath, glowStrokeType);
                const auto edgeGradient = buildContrastGradient(segment.points, segment.points.front().x,
                    segment.points.back().x, 0.40f, [&](const auto&) { return ctx.originalF0Colour; });
                g.setGradientFill(edgeGradient);
                g.strokePath(runPath, innerGlowStrokeType);
                g.setColour(colour.withAlpha(1.0f));
                g.strokePath(runPath, strokeType);
            } else {
                const auto edgeGradient = buildContrastGradient(segment.points, segment.points.front().x,
                    segment.points.back().x, 0.38f, [&](const auto&) { return ctx.originalF0Colour; });
                g.setGradientFill(edgeGradient);
                g.strokePath(runPath, innerGlowStrokeType);
                g.setColour(colour.withAlpha(1.0f));
                g.strokePath(runPath, strokeType);
            }
        }
    }

    // Draw corrected F0 (thicker).
    // 拖拽预览 item 已携带临时 snapshot（noteDrag.previewSnapshot）：其 pitchCurve
    // 是 scratch + applyCorrectionToRange 的烘焙结果，OpenTune 的 shouldDrawCorrected
    // 由该临时 curve 的 correction layer 自然成立，无需任何覆盖注入。
    // OpenDyne（notesPrimaryScheme）：显示有效F0（修正段+回退OriginalF0）；
    // OpenTune：仅 correction segment 覆盖的区域（绘制的音符范围）派生CorrectedF0，
    // 其余区域只显示OriginalF0红色细线。
    if (ctx.showCorrectedF0) {
        const bool shouldDrawCorrected = item.content.notesPrimaryScheme
            ? pitchSnapshot->hasOriginalF0Data()
            : pitchSnapshot->hasCorrectionLayer();
        if (shouldDrawCorrected) {
        auto correctedProducer = [&](auto&& sink) {
            if (item.content.notesPrimaryScheme)
                item.content.ownerSnapshot->forEachEffectiveF0Span(startFrame, endFrame, sink);
            else
                pitchSnapshot->forEachCorrectionF0Span(startFrame, endFrame,
                    [&](int spanStartFrame, const float* values, int count) {
                        sink(spanStartFrame, values, count, 1.0f);
                    });
        };

        auto frameToColour = makeFrameToColour();

        const auto visualSegments = buildF0VisualSegments(
            originalEnergy, endFrame, makeFrameToX, makeFrameToY, correctedProducer,
            frameToColour, bandX0, bandX1);

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
                    g.setColour(blendLevelHotColour(p.levelHotMix).withAlpha(alpha * 0.095f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(curveContrastColour(p.noteColour,
                        ctx.correctedF0Colour, 0.46f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(blendLevelHotColour(p.levelHotMix).withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                } else if (isBlueBreeze || isOverdose) {
                    g.setColour(blendLevelHotColour(p.levelHotMix).withAlpha(alpha * 0.070f));
                    g.strokePath(ptPath, glowStrokeType);
                    g.setColour(curveContrastColour(p.noteColour,
                        ctx.correctedF0Colour, 0.40f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(blendLevelHotColour(p.levelHotMix).withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                } else {
                    g.setColour(curveContrastColour(p.noteColour,
                        ctx.correctedF0Colour, 0.38f));
                    g.strokePath(ptPath, innerGlowStrokeType);
                    g.setColour(blendLevelHotColour(p.levelHotMix).withAlpha(1.0f));
                    g.strokePath(ptPath, strokeType);
                }
                continue;
            }

            juce::Path runPath;
            appendLinearF0Path(runPath, segment.points, segment.pathFirst, segment.pathLast);

            if (isAurora) {
                const auto& pts = segment.points;
                const float leftX = pts.front().x;
                const float rightX = pts.back().x;

                auto buildGradient = [&](float alphaScale, bool brighter = false) {
                    return buildSparseGradient(pts, leftX, rightX, [&](const auto& pt) {
                        const juce::Colour c = brighter
                            ? blendLevelHotColour(pt.levelHotMix).brighter(0.20f)
                            : blendLevelHotColour(pt.levelHotMix);
                        return c.withAlpha(alpha * alphaScale);
                    });
                };

                g.setGradientFill(buildGradient(0.095f));
                g.strokePath(runPath, glowStrokeType);
                g.setGradientFill(buildContrastGradient(pts, leftX, rightX, 0.46f,
                    [&](const auto&) { return ctx.correctedF0Colour; }));
                g.strokePath(runPath, innerGlowStrokeType);
                g.setGradientFill(buildGradient(1.0f));
                g.strokePath(runPath, strokeType);
                g.setGradientFill(buildGradient(0.15f, true));
                g.strokePath(runPath, highlightStrokeType);
            } else if (isBlueBreeze || isOverdose) {
                const auto& pts = segment.points;
                const float leftX = pts.front().x;
                const float rightX = pts.back().x;

                auto buildGradient = [&](float alphaScale, bool brighter = false) {
                    return buildSparseGradient(pts, leftX, rightX, [&](const auto& pt) {
                        const juce::Colour c = brighter
                            ? blendLevelHotColour(pt.levelHotMix).brighter(0.16f)
                            : blendLevelHotColour(pt.levelHotMix);
                        return c.withAlpha(alpha * alphaScale);
                    });
                };

                g.setGradientFill(buildGradient(0.070f));
                g.strokePath(runPath, glowStrokeType);
                g.setGradientFill(buildContrastGradient(pts, leftX, rightX, 0.40f,
                    [&](const auto&) { return ctx.correctedF0Colour; }));
                g.strokePath(runPath, innerGlowStrokeType);
                g.setGradientFill(buildGradient(1.0f));
                g.strokePath(runPath, strokeType);
                g.setGradientFill(buildGradient(0.12f, true));
                g.strokePath(runPath, highlightStrokeType);
            } else {
                const auto& pts = segment.points;
                const float leftX = pts.front().x;
                const float rightX = pts.back().x;

                const auto mainGradient = buildSparseGradient(pts, leftX, rightX, [&](const auto& pt) {
                    return blendLevelHotColour(pt.levelHotMix).withAlpha(1.0f);
                });
                g.setGradientFill(buildContrastGradient(pts, leftX, rightX, 0.38f,
                    [&](const auto&) { return ctx.correctedF0Colour; }));
                g.strokePath(runPath, innerGlowStrokeType);
                g.setGradientFill(mainGradient);
                g.strokePath(runPath, strokeType);

                // OpenDyne：极细橘红描边叠加在 corrected F0 上
                if (item.content.notesPrimaryScheme) {
                    static const juce::Colour kOpenDyneAccentStroke { 0xFFE85D2A }; // 橘红
                    g.setColour(kOpenDyneAccentStroke.withAlpha(0.65f));
                    g.strokePath(runPath, juce::PathStrokeType(0.55f,
                                                              juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));
                }
            }
        }
        }
    }
}

} // namespace OpenTune