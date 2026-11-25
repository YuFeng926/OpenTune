#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace OpenTune {

// ============================================================================
// F0 可视段构建（直算原语，从渲染器抽出的共享实现）。
//
// 段发射/合并/长度判定语义属于本函数的窗口行为：producer 按当前请求窗口发射
// 帧数据（修正段守卫依赖窗口端点，逐条来自 PitchCurveSnapshot 的窗口化遍历），
// 有声段与能量参考在窗口内现场构建。设备无关缓存（F0VisualCacheEntry）只替换
// X 源（frameToX），不改变发射语义。
// ============================================================================

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

inline float smootherStep(float value) noexcept
{
    const float t = juce::jlimit(0.0f, 1.0f, value);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
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
                const float energy = (*originalEnergy)[static_cast<size_t>(frame)];
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

} // namespace OpenTune
