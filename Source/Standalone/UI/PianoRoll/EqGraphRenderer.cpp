/**
 * EQ Graph Renderer — 动态滤波器列表曲线渲染引擎（实现）
 *
 * 视觉数学全部从 SRC (EQGraphWidget.cpp) 迁移，严禁调用 NoteEqProcessor/DSP。
 * 所有循环按 settings_.filters.size() 动态迭代，不假设固定 5 段。
 */

#include "EqGraphRenderer.h"
#include "../AuroraTheme.h"

#include <cmath>
#include <algorithm>
#include <numeric>

namespace OpenTune {

// ============================================================================
// 颜色常量
// ============================================================================

juce::Colour EqGraphRenderer::backgroundColor() { return juce::Colour(Aurora::Colors::PianoRollBg); }
juce::Colour EqGraphRenderer::gridMajorColor() { return juce::Colour(0x596D7681u); }
juce::Colour EqGraphRenderer::axisLabelColor() { return juce::Colour(0xFFAAB4C0u); }
juce::Colour EqGraphRenderer::combinedCurveColor() { return juce::Colour::fromRGB(255, 200, 72); }
juce::Colour EqGraphRenderer::hudBgColor() { return juce::Colour::fromRGBA(12, 17, 24, 224); }
juce::Colour EqGraphRenderer::hudTextColor() { return juce::Colour::fromRGB(230, 236, 243); }

// 按 EqFilterType 映射 5 色调色板
static const std::array<juce::Colour, 5> kTypePalette = {
    juce::Colour::fromRGB(255, 190, 66),   // LowCut  - 金橙
    juce::Colour::fromRGB(59, 213, 255),   // LowShelf - 青
    juce::Colour::fromRGB(157, 116, 255),  // Peak    - 紫
    juce::Colour::fromRGB(255, 111, 184),  // HighShelf - 粉
    juce::Colour::fromRGB(83, 232, 158),   // HighCut - 绿
};

juce::Colour EqGraphRenderer::bandColor(int filterIndex) const
{
    if (filterIndex < 0 || filterIndex >= static_cast<int>(settings_.filters.size()))
        return juce::Colours::grey;
    return kTypePalette[static_cast<size_t>(settings_.filters[filterIndex].type)];
}

// ============================================================================
// 坐标映射
// ============================================================================

float EqGraphRenderer::freqToX(double frequencyHz) const
{
    const double minFreq = previewMinFreq_;
    const double maxFreq = previewMaxFreq_;
    const double norm = std::clamp(std::log(frequencyHz / minFreq) / std::log(maxFreq / minFreq), 0.0, 1.0);
    return static_cast<float>(graphBounds_.getX() + norm * graphBounds_.getWidth());
}

double EqGraphRenderer::xToFreq(float x) const
{
    const double minFreq = previewMinFreq_;
    const double maxFreq = previewMaxFreq_;
    const double norm = std::clamp(
        static_cast<double>(x - graphBounds_.getX()) / std::max(1.0, static_cast<double>(graphBounds_.getWidth())),
        0.0, 1.0);
    return minFreq * std::pow(maxFreq / minFreq, norm);
}

float EqGraphRenderer::gainToY(double gainDb) const
{
    const double norm = std::clamp((gainDb + gainRangeDb_) / (2.0 * gainRangeDb_), 0.0, 1.0);
    return static_cast<float>(graphBounds_.getBottom() - norm * graphBounds_.getHeight());
}

double EqGraphRenderer::yToGain(float y) const
{
    const double norm = std::clamp(
        static_cast<double>(graphBounds_.getBottom() - y) / std::max(1.0, static_cast<double>(graphBounds_.getHeight())),
        0.0, 1.0);
    return -gainRangeDb_ + norm * 2.0 * gainRangeDb_;
}

// ============================================================================
// 视觉响应计算 — 使用动态 filter 的 type/frequency/gain/q
// ============================================================================

double EqGraphRenderer::logGaussian(double frequencyHz, double centerHz, double widthOctaves)
{
    const double octaves = std::log2(frequencyHz / centerHz);
    const double width = std::max(0.05, widthOctaves);
    return std::exp(-(octaves * octaves) / (2.0 * width * width));
}

double EqGraphRenderer::normToFrequency(double norm) const
{
    return previewMinFreq_ * std::pow(previewMaxFreq_ / previewMinFreq_, norm);
}

double EqGraphRenderer::filterResponseDb(int filterIndex, double frequencyHz) const
{
    if (filterIndex < 0 || filterIndex >= static_cast<int>(settings_.filters.size()))
        return 0.0;

    const auto& f = settings_.filters[filterIndex];

    switch (f.type)
    {
    case EqFilterType::LowCut: {
        const double ratio = static_cast<double>(f.frequencyHz) / frequencyHz;
        return -10.0 * std::log10(1.0 + std::pow(ratio, 2.0 * kFixedShelfCutRatio));
    }
    case EqFilterType::LowShelf: {
        const double t = 1.0 / (1.0 + std::pow(frequencyHz / static_cast<double>(f.frequencyHz), kFixedShelfCutRatio));
        return static_cast<double>(f.gainDb) * t;
    }
    case EqFilterType::Peak: {
        const double peakWidthOctaves = 0.42 / std::sqrt(std::max(0.25, static_cast<double>(f.q)));
        return static_cast<double>(f.gainDb) * logGaussian(frequencyHz, f.frequencyHz, peakWidthOctaves);
    }
    case EqFilterType::HighShelf: {
        const double t = 1.0 / (1.0 + std::pow(static_cast<double>(f.frequencyHz) / frequencyHz, kFixedShelfCutRatio));
        return static_cast<double>(f.gainDb) * t;
    }
    case EqFilterType::HighCut: {
        const double ratio = frequencyHz / static_cast<double>(f.frequencyHz);
        return -10.0 * std::log10(1.0 + std::pow(ratio, 2.0 * kFixedShelfCutRatio));
    }
    }
    return 0.0;
}

double EqGraphRenderer::combinedResponseDb(double frequencyHz) const
{
    double total = 0.0;
    for (int i = 0; i < static_cast<int>(settings_.filters.size()); ++i)
        total += filterResponseDb(i, frequencyHz);
    return total;
}

// ============================================================================
// 自适应对数响应采样
// ============================================================================

std::vector<EqGraphRenderer::ResponseSample> EqGraphRenderer::adaptiveLogResponseSamples(
    const std::function<double(double)>& responseDb) const
{
    std::vector<ResponseSample> samples;
    samples.reserve(kBaseSegments * 3 + 1);

    const auto sampleFromNorm = [&](double norm, double gainDb) -> ResponseSample {
        return { norm, gainDb };
    };

    std::function<void(double, double, double, double, int)> addSegment =
        [&](double a, double b, double ya, double yb, int depth) -> void {
        const double mid = (a + b) * 0.5;
        const double ym = responseDb(normToFrequency(mid));
        const double linearMid = (ya + yb) * 0.5;
        if (std::abs(ym - linearMid) > kCurvatureDb && depth < kMaxSubdivisionDepth)
        {
            addSegment(a, mid, ya, ym, depth + 1);
            addSegment(mid, b, ym, yb, depth + 1);
            return;
        }
        samples.push_back(sampleFromNorm(a, ya));
    };

    double a = 0.0;
    double ya = responseDb(normToFrequency(a));
    for (int i = 1; i <= kBaseSegments; ++i)
    {
        const double b = static_cast<double>(i) / static_cast<double>(kBaseSegments);
        const double yb = responseDb(normToFrequency(b));
        addSegment(a, b, ya, yb, 0);
        a = b;
        ya = yb;
    }
    samples.push_back(sampleFromNorm(1.0, ya));
    return samples;
}

// ============================================================================
// Fritsch-Carlson 单调三次插值
// ============================================================================

void EqGraphRenderer::monotonicCubicPath(juce::Path& path,
                                         const std::vector<ResponseSample>& samples,
                                         juce::Rectangle<float> graphBounds,
                                         double gainRangeDb)
{
    if (samples.size() < 2)
        return;

    const double twoRange = 2.0 * gainRangeDb;

    auto toPixel = [&](double norm, double gainDb) -> juce::Point<float> {
        const float x = static_cast<float>(graphBounds.getX() + norm * graphBounds.getWidth());
        const double gainNorm = (gainDb + gainRangeDb) / twoRange;
        const float y = static_cast<float>(graphBounds.getBottom() - gainNorm * graphBounds.getHeight());
        return { x, y };
    };

    auto isInside = [&](double gainDb) {
        return gainDb >= -gainRangeDb && gainDb <= gainRangeDb;
    };

    auto edgePoint = [&](const ResponseSample& a, const ResponseSample& b, double edgeDb) -> juce::Point<float> {
        const double dg = b.gainDb - a.gainDb;
        const double t = std::abs(dg) > 1e-9 ? (edgeDb - a.gainDb) / dg : 0.0;
        const double edgeNorm = a.norm + t * (b.norm - a.norm);
        return toPixel(edgeNorm, edgeDb);
    };

    struct Segment { std::vector<juce::Point<float>> points; };
    std::vector<Segment> segments;
    Segment current;

    ResponseSample prev {};
    bool prevInside = false;
    bool firstPoint = true;

    for (const ResponseSample& curr : samples)
    {
        const bool currInside = isInside(curr.gainDb);

        if (firstPoint)
        {
            if (currInside)
                current.points.push_back(toPixel(curr.norm, curr.gainDb));
            prev = curr;
            prevInside = currInside;
            firstPoint = false;
            continue;
        }

        if (prevInside && !currInside)
        {
            const double edgeDb = curr.gainDb < -gainRangeDb ? -gainRangeDb : gainRangeDb;
            current.points.push_back(edgePoint(prev, curr, edgeDb));
            if (!current.points.empty())
                segments.push_back(std::move(current));
            current = Segment{};
        }
        else if (!prevInside && currInside)
        {
            const double edgeDb = prev.gainDb < -gainRangeDb ? -gainRangeDb : gainRangeDb;
            current.points.push_back(edgePoint(prev, curr, edgeDb));
            current.points.push_back(toPixel(curr.norm, curr.gainDb));
        }
        else if (currInside)
        {
            current.points.push_back(toPixel(curr.norm, curr.gainDb));
        }

        prev = curr;
        prevInside = currInside;
    }
    if (!current.points.empty())
        segments.push_back(std::move(current));

    for (const auto& seg : segments)
    {
        if (seg.points.size() == 1)
        {
            path.startNewSubPath(seg.points[0]);
            continue;
        }

        const size_t n = seg.points.size();
        std::vector<double> slopes(n, 0.0);
        std::vector<double> secants(n - 1, 0.0);
        for (size_t i = 0; i + 1 < n; ++i)
        {
            const double dx = static_cast<double>(seg.points[i + 1].getX()) - static_cast<double>(seg.points[i].getX());
            if (std::abs(dx) > 0.0001)
                secants[i] = (static_cast<double>(seg.points[i + 1].getY()) - static_cast<double>(seg.points[i].getY())) / dx;
        }

        slopes.front() = secants.front();
        slopes.back() = secants.back();
        for (size_t i = 1; i + 1 < n; ++i)
        {
            const double left = secants[i - 1];
            const double right = secants[i];
            if (left == 0.0 || right == 0.0 || (left < 0.0) != (right < 0.0))
            {
                slopes[i] = 0.0;
                continue;
            }
            const double limit = 3.0 * std::min(std::abs(left), std::abs(right));
            slopes[i] = std::clamp((left + right) * 0.5, -limit, limit);
        }
        for (size_t i = 0; i + 1 < n; ++i)
        {
            const double secant = secants[i];
            if (secant == 0.0)
            {
                slopes[i] = 0.0;
                slopes[i + 1] = 0.0;
                continue;
            }
            if ((slopes[i] < 0.0) != (secant < 0.0))
                slopes[i] = 0.0;
            if ((slopes[i + 1] < 0.0) != (secant < 0.0))
                slopes[i + 1] = 0.0;
            const double alpha = slopes[i] / secant;
            const double beta = slopes[i + 1] / secant;
            const double magnitude = std::sqrt(alpha * alpha + beta * beta);
            if (magnitude > 3.0)
            {
                const double scale = 3.0 / magnitude;
                slopes[i] *= scale;
                slopes[i + 1] *= scale;
            }
        }

        path.startNewSubPath(seg.points[0]);
        for (size_t i = 0; i + 1 < n; ++i)
        {
            const float dx = seg.points[i + 1].getX() - seg.points[i].getX();
            const float s0 = static_cast<float>(slopes[i]);
            const float s1 = static_cast<float>(slopes[i + 1]);
            const juce::Point<float> c1(seg.points[i].getX() + dx / 3.0f,
                                        seg.points[i].getY() + s0 * dx / 3.0f);
            const juce::Point<float> c2(seg.points[i + 1].getX() - dx / 3.0f,
                                        seg.points[i + 1].getY() - s1 * dx / 3.0f);
            path.cubicTo(c1, c2, seg.points[i + 1]);
        }
    }
}

// ============================================================================
// 曲线路径生成
// ============================================================================

juce::Path EqGraphRenderer::buildCombinedPath() const
{
    const auto responseDb = [this](double freq) -> double {
        return combinedResponseDb(freq);
    };
    const auto samples = adaptiveLogResponseSamples(responseDb);

    juce::Path path;
    monotonicCubicPath(path, samples, graphBounds_, gainRangeDb_);
    return path;
}

juce::Path EqGraphRenderer::buildSingleBandPath(int filterIndex) const
{
    const auto responseDb = [this, filterIndex](double freq) -> double {
        return filterResponseDb(filterIndex, freq);
    };
    const auto samples = adaptiveLogResponseSamples(responseDb);

    juce::Path path;
    monotonicCubicPath(path, samples, graphBounds_, gainRangeDb_);
    return path;
}

juce::Path EqGraphRenderer::buildBandInfluencePath(int filterIndex) const
{
    if (filterIndex < 0 || filterIndex >= static_cast<int>(settings_.filters.size()))
        return {};

    const auto& f = settings_.filters[filterIndex];
    const auto responseDb = [this, filterIndex](double freq) -> double {
        return std::clamp(filterResponseDb(filterIndex, freq), -gainRangeDb_, gainRangeDb_);
    };
    const auto samples = adaptiveLogResponseSamples(responseDb);
    if (samples.size() < 2)
        return {};

    juce::Path curve;
    curve.startNewSubPath(
        static_cast<float>(graphBounds_.getX() + graphBounds_.getWidth() * samples.front().norm),
        gainToY(samples.front().gainDb));
    for (size_t i = 1; i < samples.size(); ++i)
        curve.lineTo(
            static_cast<float>(graphBounds_.getX() + graphBounds_.getWidth() * samples[i].norm),
            gainToY(samples[i].gainDb));

    const float neutralY = gainToY(0.0);
    const bool isHighCut = (f.type == EqFilterType::HighCut);
    const bool isLowCut  = (f.type == EqFilterType::LowCut);

    if (isHighCut)
    {
        curve.lineTo(graphBounds_.getX(), curve.getCurrentPosition().y);
        curve.lineTo(graphBounds_.getX(), neutralY);
    }
    else if (isLowCut)
    {
        curve.lineTo(graphBounds_.getRight(), curve.getCurrentPosition().y);
        curve.lineTo(graphBounds_.getRight(), neutralY);
    }
    else
    {
        const auto lastPt = curve.getCurrentPosition();
        curve.lineTo(lastPt.x, neutralY);
        curve.lineTo(static_cast<float>(graphBounds_.getX() + graphBounds_.getWidth() * samples.front().norm), neutralY);
    }
    curve.closeSubPath();
    return curve;
}

// ============================================================================
// 频谱背景动画
// ============================================================================

void EqGraphRenderer::drawSpectrumBackground(juce::Graphics& g,
                                              const std::array<float, 128>& spectrum,
                                              const std::array<float, 128>& peaks) const
{
    const float gx = graphBounds_.getX();
    const float gy = graphBounds_.getY();
    const float gw = graphBounds_.getWidth();
    const float gh = graphBounds_.getHeight();
    const int numBins = static_cast<int>(spectrum.size());

    std::array<juce::Point<float>, 128> linePoints{};
    std::array<juce::Point<float>, 128> peakPoints{};
    for (int i = 0; i < numBins; ++i)
    {
        const float x = gx + (static_cast<float>(i) / static_cast<float>(numBins - 1)) * gw;
        const float h = std::clamp(spectrum[static_cast<size_t>(i)], 0.0f, 1.0f) * gh * 0.7f;
        const float peak = std::clamp(peaks[static_cast<size_t>(i)], 0.0f, 1.0f);
        spectrumPeakTrace_[static_cast<size_t>(i)] = std::max(
            peak, std::max(0.0f, spectrumPeakTrace_[static_cast<size_t>(i)] - 0.012f));
        linePoints[static_cast<size_t>(i)] = { x, gy + gh - h };
        peakPoints[static_cast<size_t>(i)] = {
            x, gy + gh - spectrumPeakTrace_[static_cast<size_t>(i)] * gh * 0.7f };
    }

    const auto makeSmoothPath = [](const auto& points, float baseline) {
        juce::Path path;
        path.startNewSubPath(points.front());
        for (size_t i = 1; i < points.size(); ++i)
        {
            const auto midpoint = juce::Point<float>(
                (points[i - 1].x + points[i].x) * 0.5f,
                (points[i - 1].y + points[i].y) * 0.5f);
            path.quadraticTo(points[i - 1], midpoint);
        }
        path.lineTo(points.back());
        path.lineTo(points.back().x, baseline);
        path.lineTo(points.front().x, baseline);
        path.closeSubPath();
        return path;
    };

    auto fillPath = makeSmoothPath(linePoints, gy + gh);

    {
        juce::Graphics::ScopedSaveState saved(g);
        g.reduceClipRegion(graphBounds_.toNearestInt());
        juce::ColourGradient grad(
            juce::Colour::fromRGBA(59, 213, 255, 18), 0.0f, gy,
            juce::Colour::fromRGBA(59, 213, 255, 0),  0.0f, gy + gh, true);
        grad.addColour(0.4, juce::Colour::fromRGBA(83, 232, 158, 12));
        g.setGradientFill(grad);
        g.fillPath(fillPath);
    }

    const auto makeLinePath = [](const auto& points) {
        juce::Path path;
        path.startNewSubPath(points.front());
        for (size_t i = 1; i < points.size(); ++i)
        {
            const auto midpoint = juce::Point<float>(
                (points[i - 1].x + points[i].x) * 0.5f,
                (points[i - 1].y + points[i].y) * 0.5f);
            path.quadraticTo(points[i - 1], midpoint);
        }
        path.lineTo(points.back());
        return path;
    };

    const auto peakPath = makeLinePath(peakPoints);
    g.setColour(juce::Colour::fromRGBA(130, 245, 222, 0x22));
    g.strokePath(peakPath, juce::PathStrokeType(4.8f,
        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(juce::Colour::fromRGBA(180, 255, 236, 0x58));
    g.strokePath(peakPath, juce::PathStrokeType(1.0f,
        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const auto linePath = makeLinePath(linePoints);
    g.setColour(juce::Colours::white.withAlpha(0.14f));
    g.strokePath(linePath, juce::PathStrokeType(1.25f,
        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// ============================================================================
// 网格 / 轴标签
// ============================================================================

void EqGraphRenderer::drawGrid(juce::Graphics& g) const
{
    g.setColour(gridMajorColor());

    static const double freqs[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    for (double freq : freqs)
    {
        const float x = freqToX(freq);
        if (x >= graphBounds_.getX() && x <= graphBounds_.getRight())
            g.drawVerticalLine(static_cast<int>(x), graphBounds_.getY(), graphBounds_.getBottom());
    }

    const double stepDb = gainRangeDb_ <= 12.0 ? 3.0 : 6.0;
    const int steps = static_cast<int>(std::round(gainRangeDb_ / stepDb));
    for (int i = -steps; i <= steps; ++i)
    {
        const double gainDb = static_cast<double>(i) * stepDb;
        const float y = gainToY(gainDb);
        if (y >= graphBounds_.getY() && y <= graphBounds_.getBottom())
            g.drawHorizontalLine(static_cast<int>(y), graphBounds_.getX(), graphBounds_.getRight());
    }

    g.setColour(axisLabelColor().withAlpha(0.6f));
    const float zeroY = gainToY(0.0);
    g.drawHorizontalLine(static_cast<int>(zeroY), graphBounds_.getX(), graphBounds_.getRight());
}

void EqGraphRenderer::drawAxisLabels(juce::Graphics& g) const
{
    g.setColour(axisLabelColor());
    g.setFont(juce::FontOptions(9.0f));

    const float gx = graphBounds_.getX();
    const float gy = graphBounds_.getY();
    const float gw = graphBounds_.getWidth();
    const float gh = graphBounds_.getHeight();
    const float labelH = 12.0f;
    const float labelW = 24.0f;
    const float padBottom = 2.0f;
    const float padRight = 30.0f;

    static const struct { double freq; const char* label; } freqLabels[] = {
        { 20, "20" }, { 50, "50" }, { 100, "100" }, { 200, "200" },
        { 500, "500" }, { 1000, "1k" }, { 2000, "2k" }, { 5000, "5k" },
        { 10000, "10k" }, { 20000, "20k" }
    };
    for (const auto& fl : freqLabels)
    {
        const float x = freqToX(fl.freq);
        if (x >= gx && x <= gx + gw)
        {
            float labelX = x - labelW * 0.5f;
            labelX = juce::jmax(gx, juce::jmin(labelX, gx + gw - labelW));
            const float labelY = gy + gh - labelH - padBottom;
            g.drawText(fl.label, static_cast<int>(labelX), static_cast<int>(labelY),
                       static_cast<int>(labelW), static_cast<int>(labelH),
                       juce::Justification::centredBottom, false);
        }
    }

    const float gainLabelW = 30.0f;
    const double stepDb = gainRangeDb_ <= 12.0 ? 3.0 : 6.0;
    const int steps = static_cast<int>(std::round(gainRangeDb_ / stepDb));
    for (int i = -steps; i <= steps; ++i)
    {
        const double gainDb = static_cast<double>(i) * stepDb;
        const float y = gainToY(gainDb);
        if (y >= gy && y <= gy + gh)
        {
            const juce::String text = (gainDb > 0 ? "+" : "") + juce::String(gainDb, 0);
            const float labelX = gx + gw - gainLabelW - padRight;
            const float labelY = juce::jlimit(gy, gy + gh - labelH, y - labelH * 0.5f);
            g.drawText(text, static_cast<int>(labelX), static_cast<int>(labelY),
                       static_cast<int>(gainLabelW), static_cast<int>(labelH),
                       juce::Justification::centredRight, false);
        }
    }
}

// ============================================================================
// 锚点绘制 — 使用动态 filter 数据
// ============================================================================

juce::Point<float> EqGraphRenderer::anchorPosition(int filterIndex) const
{
    if (filterIndex < 0 || filterIndex >= static_cast<int>(settings_.filters.size()))
        return {};

    const auto& f = settings_.filters[filterIndex];
    const double freq = static_cast<double>(f.frequencyHz);

    double gain = 0.0;
    if (f.type == EqFilterType::LowCut || f.type == EqFilterType::HighCut)
        gain = filterResponseDb(filterIndex, freq);
    else
        gain = static_cast<double>(f.gainDb);

    return { freqToX(freq), gainToY(gain) };
}

int EqGraphRenderer::hitTestAnchor(juce::Point<float> pos, float threshold) const
{
    int closestBand = -1;
    float closestDist = threshold;
    const int n = static_cast<int>(settings_.filters.size());

    for (int i = 0; i < n; ++i)
    {
        const auto anchorPos = anchorPosition(i);
        const float dist = pos.getDistanceFrom(anchorPos);
        if (dist < closestDist)
        {
            closestDist = dist;
            closestBand = i;
        }
    }
    return closestBand;
}

float EqGraphRenderer::curveYAtX(int filterIndex, float x) const
{
    const double freq = xToFreq(x);
    const double gainDb = filterResponseDb(filterIndex, freq);
    return gainToY(gainDb);
}

int EqGraphRenderer::hitTestCurve(juce::Point<float> pos, float threshold) const
{
    if (!graphBounds_.contains(pos))
        return -1;

    int closestBand = -1;
    float closestDist = threshold;
    const int n = static_cast<int>(settings_.filters.size());

    for (int i = 0; i < n; ++i)
    {
        const float curveY = curveYAtX(i, pos.x);
        const float dist = std::abs(pos.y - curveY);
        if (dist < closestDist)
        {
            closestDist = dist;
            closestBand = i;
        }
    }
    return closestBand;
}

void EqGraphRenderer::drawAnchors(juce::Graphics& g, int hoveredBand, bool showNumbers) const
{
    const int n = static_cast<int>(settings_.filters.size());

    for (int i = 0; i < n; ++i)
    {
        const auto pos = anchorPosition(i);
        const bool isHovered = (i == hoveredBand);
        const auto color = bandColor(i);

        const float radius = isHovered ? kAnchorHoverRadius : kAnchorNormalRadius;
        const float haloRadius = isHovered ? kHaloHoverRadius : kHaloNormalRadius;

        g.setColour(color.withAlpha(isHovered ? 0.30f : 0.15f));
        g.fillEllipse(pos.x - haloRadius, pos.y - haloRadius,
                      haloRadius * 2.0f, haloRadius * 2.0f);

        g.setColour(color);
        g.fillEllipse(pos.x - radius, pos.y - radius,
                      radius * 2.0f, radius * 2.0f);

        g.setColour(juce::Colours::white.withAlpha(isHovered ? 0.85f : 0.60f));
        g.drawEllipse(pos.x - radius, pos.y - radius,
                      radius * 2.0f, radius * 2.0f, 1.25f);

        if (showNumbers)
        {
            const float numOffsetX = radius + 5.0f;
            const float numOffsetY = -radius - 2.0f;

            g.setColour(juce::Colours::black.withAlpha(0.65f));
            g.setFont(juce::FontOptions(8.0f));
            g.drawText(juce::String(i + 1),
                       juce::Rectangle<float>(pos.x + numOffsetX - 0.5f, pos.y + numOffsetY - 0.5f,
                                              10.0f, 10.0f),
                       juce::Justification::centred, false);
            g.drawText(juce::String(i + 1),
                       juce::Rectangle<float>(pos.x + numOffsetX + 0.5f, pos.y + numOffsetY + 0.5f,
                                              10.0f, 10.0f),
                       juce::Justification::centred, false);
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.drawText(juce::String(i + 1),
                       juce::Rectangle<float>(pos.x + numOffsetX, pos.y + numOffsetY,
                                              10.0f, 10.0f),
                       juce::Justification::centred, false);
        }
    }
}

// ============================================================================
// 十字引导线与 HUD
// ============================================================================

void EqGraphRenderer::drawCrosshairAndHud(juce::Graphics& g, juce::Point<float> pos,
                                          int bandIndex, const juce::String& hudText) const
{
    if (!graphBounds_.contains(pos))
        return;

    const auto color = bandColor(bandIndex);

    g.setColour(color.withAlpha(0.25f));
    g.drawHorizontalLine(static_cast<int>(pos.y), graphBounds_.getX(), graphBounds_.getRight());
    g.drawVerticalLine(static_cast<int>(pos.x), graphBounds_.getY(), graphBounds_.getBottom());

    if (!hudText.isEmpty())
    {
        g.setFont(juce::FontOptions(10.0f));
        juce::GlyphArrangement hudGa;
        hudGa.addLineOfText(juce::Font(juce::FontOptions(10.0f)), hudText, 0.0f, 0.0f);
        const auto textWidth = (hudGa.getNumGlyphs() > 0
            ? hudGa.getBoundingBox(0, hudGa.getNumGlyphs(), false).getWidth()
            : 0.0f) + 10.0f;
        const float hudW = textWidth + 8.0f;
        const float hudH = 18.0f;
        float hudX = pos.x + 12.0f;
        float hudY = pos.y - hudH - 4.0f;
        if (hudX + hudW > graphBounds_.getRight())
            hudX = pos.x - hudW - 12.0f;
        if (hudY < graphBounds_.getY())
            hudY = pos.y + 8.0f;

        g.setColour(hudBgColor());
        g.fillRoundedRectangle(hudX, hudY, hudW, hudH, 4.0f);
        g.setColour(hudTextColor());
        g.drawText(hudText, juce::Rectangle<float>(hudX + 4.0f, hudY, hudW - 8.0f, hudH),
                   juce::Justification::centred, false);
    }
}

// ============================================================================
// 图例（动态数量，按 settings_.filters 构建）
// ============================================================================

float EqGraphRenderer::measureLegendItemWidth(const juce::String& label) const
{
    juce::GlyphArrangement ga;
    ga.addLineOfText(juce::Font(juce::FontOptions(9.0f)), label, 0.0f, 0.0f);
    return ga.getNumGlyphs() > 0
        ? ga.getBoundingBox(0, ga.getNumGlyphs(), false).getWidth()
        : 0.0f;
}

std::vector<EqGraphRenderer::LegendItem> EqGraphRenderer::buildLegendItems() const
{
    std::vector<LegendItem> items;
    items.reserve(settings_.filters.size() + 1);

    {
        LegendItem combined;
        combined.isCombined = true;
        combined.filterIndex = -1;
        combined.label = "Combined";
        combined.color = combinedCurveColor();
        combined.enabled = true;
        items.push_back(combined);
    }

    for (int i = 0; i < static_cast<int>(settings_.filters.size()); ++i)
    {
        const auto& f = settings_.filters[i];
        juce::String label;
        switch (f.type)
        {
        case EqFilterType::LowCut:    label = "LowCut";    break;
        case EqFilterType::LowShelf:  label = "LowShelf";  break;
        case EqFilterType::Peak:      label = "Peak";      break;
        case EqFilterType::HighShelf: label = "HighShelf"; break;
        case EqFilterType::HighCut:   label = "HighCut";   break;
        }
        LegendItem item;
        item.isCombined = false;
        item.filterIndex = i;
        item.label = label;
        item.color = bandColor(i);
        item.enabled = isCurveVisible(i);
        items.push_back(item);
    }
    return items;
}

juce::Rectangle<float> EqGraphRenderer::legendBounds(const std::vector<LegendItem>& items) const
{
    float totalWidth = 0.0f;
    const float itemHeight = 12.0f;
    const float gap = 8.0f;
    const float swatchW = 14.0f;

    for (const auto& item : items)
        totalWidth += swatchW + measureLegendItemWidth(item.label) + gap;

    return { graphBounds_.getRight() - totalWidth - 14.0f, graphBounds_.getY() + 12.0f, totalWidth, itemHeight };
}

void EqGraphRenderer::drawLegend(juce::Graphics& g, const std::vector<LegendItem>& items,
                                 int hoveredLegendIndex) const
{
    g.setFont(juce::FontOptions(9.0f));
    const float swatchW = 14.0f;
    const float gap = 8.0f;
    const float itemH = 12.0f;

    float totalWidth = 0.0f;
    for (const auto& item : items)
        totalWidth += swatchW + measureLegendItemWidth(item.label) + gap;
    float x = graphBounds_.getRight() - totalWidth - 14.0f;
    const float y = graphBounds_.getY() + 12.0f;

    const juce::Rectangle<float> box(x - 10.0f, y - 5.0f, totalWidth + 16.0f, itemH + 10.0f);
    g.setColour(juce::Colour::fromRGBA(7, 12, 18, 126));
    g.fillRoundedRectangle(box, 8.0f);
    g.setColour(juce::Colour::fromRGBA(86, 111, 132, 86));
    g.drawRoundedRectangle(box, 8.0f, 1.0f);

    for (size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i];
        const bool hovered = (static_cast<int>(i) == hoveredLegendIndex);
        const float textW = measureLegendItemWidth(item.label);

        auto color = item.color;
        if (!item.enabled)
            color = color.withAlpha(0.25f);
        g.setColour(color);
        g.fillRect(x, y + 2.0f, swatchW, itemH - 4.0f);

        g.setColour(item.enabled ? axisLabelColor() : axisLabelColor().withAlpha(0.35f));
        g.drawText(item.label, juce::Rectangle<float>(x + swatchW + 2.0f, y, textW, itemH),
                   juce::Justification::centredLeft, false);

        if (hovered)
        {
            g.setColour(juce::Colours::white.withAlpha(0.15f));
            g.fillRect(x - 2.0f, y - 1.0f, swatchW + textW + 8.0f, itemH + 2.0f);
        }

        x += swatchW + textW + gap;
    }
}

int EqGraphRenderer::hitTestLegend(juce::Point<float> pos,
                                   const std::vector<LegendItem>& items) const
{
    const float swatchW = 14.0f;
    const float gap = 8.0f;
    const float itemH = 12.0f;

    float totalWidth = 0.0f;
    for (const auto& item : items)
        totalWidth += swatchW + measureLegendItemWidth(item.label) + gap;
    float x = graphBounds_.getRight() - totalWidth - 14.0f;
    const float y = graphBounds_.getY() + 12.0f;

    for (size_t i = 0; i < items.size(); ++i)
    {
        const float textW = measureLegendItemWidth(items[i].label);
        const juce::Rectangle<float> itemRect(x - 2.0f, y - 1.0f, swatchW + textW + 8.0f, itemH + 2.0f);
        if (itemRect.contains(pos))
            return static_cast<int>(i);
        x += swatchW + textW + gap;
    }
    return -1;
}

// ============================================================================
// 视图范围按钮
// ============================================================================

juce::Rectangle<float> EqGraphRenderer::viewRangeButtonRect(int controlIndex) const
{
    const float topInset = 27.0f;
    const float leftInset = 10.0f;
    const float buttonSize = kViewRangeCircleRadius * 2.0f;
    const float centerX = graphBounds_.getX() + leftInset + kViewRangeCircleRadius
                          + static_cast<float>(controlIndex) * (buttonSize + kViewRangeButtonGap);
    const float centerY = graphBounds_.getY() + topInset + kViewRangeCircleRadius;
    return { centerX - kViewRangeCircleRadius, centerY - kViewRangeCircleRadius,
             buttonSize, buttonSize };
}

void EqGraphRenderer::drawViewRangeButtons(juce::Graphics& g,
                                           int hoveredControl, int pressedControl) const
{
    for (int ci = 0; ci < 2; ++ci)
    {
        const auto button = viewRangeButtonRect(ci);
        juce::Rectangle<float> face = button.withSizeKeepingCentre(24.0f, 24.0f);

        juce::Colour fill = juce::Colour::fromRGBA(18, 26, 34, 226);
        juce::Colour stroke = juce::Colour::fromRGBA(91, 113, 132, 168);
        if (ci == pressedControl)
        {
            face = button.reduced(1.0f);
            fill = juce::Colour::fromRGBA(47, 59, 70, 232);
            stroke = juce::Colour::fromRGBA(134, 154, 170, 218);
        }
        else if (ci == hoveredControl)
        {
            face = button.reduced(1.0f);
            fill = juce::Colour::fromRGBA(34, 45, 56, 236);
            stroke = juce::Colour::fromRGBA(119, 144, 164, 218);
        }

        g.setColour(fill);
        g.fillEllipse(face);
        g.setColour(stroke);
        g.drawEllipse(face, 1.0f);

        const float cx = button.getCentreX();
        const float cy = button.getCentreY();
        constexpr float half = 5.2f;
        g.setColour(juce::Colour::fromRGBA(220, 230, 238, 238));
        g.drawLine(cx - half, cy, cx + half, cy, 1.45f);
        if (ci == 0)
            g.drawLine(cx, cy - half, cx, cy + half, 1.45f);
    }
}

EqGraphRenderer::ViewRangeButton EqGraphRenderer::hitTestViewRangeButton(juce::Point<float> pos) const
{
    for (int ci = 0; ci < 2; ++ci)
    {
        if (viewRangeButtonRect(ci).contains(pos))
            return (ci == 0) ? ViewRangeButton::Decrease : ViewRangeButton::Increase;
    }
    return ViewRangeButton::None;
}

// ============================================================================
// 渲染入口 — 动态迭代 filters
// ============================================================================

void EqGraphRenderer::drawPreview(juce::Graphics& g) const
{
    const float bypassAlpha = settings_.active ? 1.0f : 0.28f;
    const int n = static_cast<int>(settings_.filters.size());

    for (int i = 0; i < n; ++i)
    {
        const auto path = buildSingleBandPath(i);
        g.setColour(bandColor(i).withAlpha(0.6f * bypassAlpha));
        g.strokePath(path, juce::PathStrokeType(kSingleCurveWidth,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }

    if (n > 0)
    {
        const auto path = buildCombinedPath();
        g.setColour(combinedCurveColor().withAlpha(bypassAlpha));
        g.strokePath(path, juce::PathStrokeType(kCombinedCurveWidth,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }

    drawAnchors(g, -1, false);

    g.setColour(axisLabelColor().withAlpha(0.35f));
    g.drawRoundedRectangle(graphBounds_.reduced(0.5f), 4.0f, 1.0f);
}

void EqGraphRenderer::drawFull(juce::Graphics& g, int hoveredBand,
                               juce::Point<float> mousePos, bool isDragging,
                               int hoveredViewRangeControl,
                               int pressedViewRangeControl,
                               const std::vector<double>& hoverBandAmounts) const
{
    drawGrid(g);

    const float bypassAlpha = settings_.active ? 1.0f : 0.28f;
    const int n = static_cast<int>(settings_.filters.size());

    for (int i = 0; i < n; ++i)
    {
        const auto path = buildSingleBandPath(i);
        g.setColour(bandColor(i).withAlpha(0.6f * bypassAlpha));
        g.strokePath(path, juce::PathStrokeType(kSingleCurveWidth,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }
    if (n > 0)
    {
        const auto path = buildCombinedPath();
        g.setColour(combinedCurveColor().withAlpha(bypassAlpha));
        g.strokePath(path, juce::PathStrokeType(kCombinedCurveWidth,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }

    drawAxisLabels(g);

    if (settings_.active && isDragging && hoveredBand >= 0 && hoveredBand < n)
    {
        const double freq = xToFreq(mousePos.x);
        const double gain = yToGain(mousePos.y);
        juce::String hudText;
        if (freq >= 1000.0)
            hudText = juce::String(freq / 1000.0, 1) + " kHz";
        else
            hudText = juce::String(static_cast<int>(freq)) + " Hz";
        hudText += juce::String("  ") + (gain >= 0 ? juce::String("+") : juce::String())
                   + juce::String(gain, 1) + juce::String(" dB");
        drawCrosshairAndHud(g, mousePos, hoveredBand, hudText);
    }

    drawAnchors(g, hoveredBand);

    drawViewRangeButtons(g, hoveredViewRangeControl, pressedViewRangeControl);

    // hover influence — 动态数量
    for (int i = 0; i < n; ++i)
    {
        if (i >= static_cast<int>(hoverBandAmounts.size()))
            break;
        const double amount = hoverBandAmounts[static_cast<size_t>(i)];
        if (amount <= 0.01)
            continue;

        const auto color = bandColor(i);
        const auto bright = color.brighter(0.38f);
        const float a = static_cast<float>(amount);

        {
            juce::Graphics::ScopedSaveState saved(g);
            g.reduceClipRegion(graphBounds_.toNearestInt().expanded(8));
            const auto influencePath = buildBandInfluencePath(i);

            juce::ColourGradient grad(
                bright.withAlpha(0.15f * a), 0.0f, graphBounds_.getY(),
                color.withAlpha(0.0f),        0.0f, graphBounds_.getBottom(), true);
            grad.addColour(0.5, color.withAlpha(0.06f * a));
            g.setGradientFill(grad);
            g.fillPath(influencePath);
        }

        const auto curvePath = buildSingleBandPath(i);

        g.setColour(bright.withAlpha(0.15f * a));
        g.strokePath(curvePath, juce::PathStrokeType(2.35f,
            juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour(bright.withAlpha(0.78f * a));
        g.strokePath(curvePath, juce::PathStrokeType(1.12f,
            juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        {
            const auto node = anchorPosition(i);
            const float r = 13.5f;
            juce::ColourGradient glow(
                bright.withAlpha(0.15f * a), node.x, node.y,
                color.withAlpha(0.0f),        node.x, node.y + r, true);
            glow.addColour(0.36, color.withAlpha(0.06f * a));
            g.setGradientFill(glow);
            g.fillEllipse(node.x - r, node.y - r, r * 2.0f, r * 2.0f);
        }
    }
}

} // namespace OpenTune
