/**
 * EQ Graph Renderer — 严格复刻 SRC 视觉数学的曲线渲染引擎（实现）
 *
 * 视觉数学全部从 SRC (EQGraphWidget.cpp) 迁移，严禁调用 NoteEqProcessor/DSP。
 * 数学公式参考：
 *   - filterResponseDb: SRC line 6591-6615
 *   - logGaussian: SRC line 364-369
 *   - adaptiveLogResponseSamples: SRC line 6140-6176
 *   - forEachMonotoneCubicSegment (Fritsch-Carlson): SRC line 799-874
 *   - bandColor: SRC line 6199-6213
 *   - Theme constants: SRC line 393-403
 *   - Anchor visuals: SRC radius/halo/stroke
 */

#include "EqGraphRenderer.h"
#include "../AuroraTheme.h"

#include <cmath>
#include <algorithm>
#include <numeric>

namespace OpenTune {

// ============================================================================
// 颜色常量（源自 Aurora 主题）
// ============================================================================

juce::Colour EqGraphRenderer::backgroundColor() { return juce::Colour(Aurora::Colors::PianoRollBg); }
juce::Colour EqGraphRenderer::gridMajorColor() { return juce::Colour(0xFF6D7681u); }
juce::Colour EqGraphRenderer::axisLabelColor() { return juce::Colour(0xFFAAB4C0u); }
juce::Colour EqGraphRenderer::combinedCurveColor() { return juce::Colour::fromRGB(255, 200, 72); }
juce::Colour EqGraphRenderer::hudBgColor() { return juce::Colour::fromRGBA(12, 17, 24, 224); }
juce::Colour EqGraphRenderer::hudTextColor() { return juce::Colour::fromRGB(230, 236, 243); }

juce::Colour EqGraphRenderer::bandColor(int bandIndex)
{
    static const std::array<juce::Colour, 5> palette = {
        juce::Colour::fromRGB(255, 190, 66),   // 0 LowCut  - 金橙
        juce::Colour::fromRGB(59, 213, 255),   // 1 LowShelf - 青
        juce::Colour::fromRGB(157, 116, 255),  // 2 Peak    - 紫
        juce::Colour::fromRGB(255, 111, 184),  // 3 HighShelf - 粉
        juce::Colour::fromRGB(83, 232, 158),   // 4 HighCut - 绿
    };
    return palette[static_cast<size_t>(bandIndex)];
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
// 视觉响应计算 — 严格复刻 SRC filterResponseDb (EQGraphWidget.cpp:6591-6615)
// ============================================================================

double EqGraphRenderer::logGaussian(double frequencyHz, double centerHz, double widthOctaves)
{
    const double octaves = std::log2(frequencyHz / centerHz);
    const double width = std::max(0.05, widthOctaves);
    return std::exp(-(octaves * octaves) / (2.0 * width * width));
}

double EqGraphRenderer::normToFrequency(double norm)
{
    return kMinFrequencyHz * std::pow(kMaxFrequencyHz / kMinFrequencyHz, norm);
}

double EqGraphRenderer::filterResponseDb(int bandIndex, double frequencyHz) const
{
    switch (bandIndex)
    {
    case 0: { // LowCut — SRC line 6609-6612
        const double ratio = settings_.lowCutFrequencyHz / frequencyHz;
        return -10.0 * std::log10(1.0 + std::pow(ratio, 2.0 * kFixedShelfCutRatio));
    }
    case 1: { // LowShelf — SRC line 6597-6599
        const double t = 1.0 / (1.0 + std::pow(frequencyHz / settings_.lowShelfFrequencyHz, kFixedShelfCutRatio));
        return settings_.lowShelfGainDb * t;
    }
    case 2: { // Peak — SRC line 6595-6596
        const double peakWidthOctaves = 0.42 / std::sqrt(kPeakQ);
        return settings_.peakGainDb * logGaussian(frequencyHz, settings_.peakFrequencyHz, peakWidthOctaves);
    }
    case 3: { // HighShelf — SRC line 6601-6603
        const double t = 1.0 / (1.0 + std::pow(settings_.highShelfFrequencyHz / frequencyHz, kFixedShelfCutRatio));
        return settings_.highShelfGainDb * t;
    }
    case 4: { // HighCut — SRC line 6605-6608
        const double ratio = frequencyHz / settings_.highCutFrequencyHz;
        return -10.0 * std::log10(1.0 + std::pow(ratio, 2.0 * kFixedShelfCutRatio));
    }
    }
    return 0.0;
}

double EqGraphRenderer::combinedResponseDb(double frequencyHz) const
{
    double total = 0.0;
    for (int i = 0; i < 5; ++i)
        total += filterResponseDb(i, frequencyHz);
    // 返回真实求和，不固定 clamp ±12；路径生成按 gainRangeDb_ 裁剪
    return total;
}

// ============================================================================
// 自适应对数响应采样 — 严格复刻 SRC adaptiveLogResponseSamples (line 6140-6176)
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
// Fritsch-Carlson 单调三次插值 — 严格复刻 SRC forEachMonotoneCubicSegment (line 799-874)
// gainRangeDb 从外部传入，用于 Y 映射和路径裁剪
// ============================================================================

void EqGraphRenderer::monotonicCubicPath(juce::Path& path,
                                         const std::vector<ResponseSample>& samples,
                                         juce::Rectangle<float> graphBounds,
                                         double gainRangeDb)
{
    if (samples.size() < 2)
        return;

    // 转换为像素点（使用传入的 gainRangeDb 映射 Y）
    std::vector<juce::Point<float>> points;
    points.reserve(samples.size());
    const double twoRange = 2.0 * gainRangeDb;
    for (const auto& s : samples)
    {
        const float x = static_cast<float>(graphBounds.getX() + s.norm * graphBounds.getWidth());
        const double gainNorm = (s.gainDb + gainRangeDb) / twoRange;
        const float y = static_cast<float>(graphBounds.getBottom() - gainNorm * graphBounds.getHeight());
        points.emplace_back(x, y);
    }

    // Fritsch-Carlson 斜率计算（源自 SRC）
    const size_t n = points.size();
    std::vector<double> slopes(n, 0.0);
    std::vector<double> secants(n - 1, 0.0);
    for (size_t i = 0; i + 1 < n; ++i)
    {
        const double dx = static_cast<double>(points[i + 1].getX()) - static_cast<double>(points[i].getX());
        if (std::abs(dx) > 0.0001)
            secants[i] = (static_cast<double>(points[i + 1].getY()) - static_cast<double>(points[i].getY())) / dx;
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

    // 生成贝塞尔路径（slopes 在视觉像素边界处显式窄化到 float）
    path.startNewSubPath(points[0]);
    for (size_t i = 0; i + 1 < n; ++i)
    {
        const float dx = points[i + 1].getX() - points[i].getX();
        const float s0 = static_cast<float>(slopes[i]);
        const float s1 = static_cast<float>(slopes[i + 1]);
        const juce::Point<float> c1(points[i].getX() + dx / 3.0f,
                                    points[i].getY() + s0 * dx / 3.0f);
        const juce::Point<float> c2(points[i + 1].getX() - dx / 3.0f,
                                    points[i + 1].getY() - s1 * dx / 3.0f);
        path.cubicTo(c1, c2, points[i + 1]);
    }
}

// ============================================================================
// 曲线路径生成（传递 gainRangeDb 给 monotonicCubicPath，路径裁剪到 [-range,+range]）
// ============================================================================

juce::Path EqGraphRenderer::buildCombinedPath() const
{
    const auto responseDb = [this](double freq) -> double {
        // 按当前视图增益范围裁剪到[-range,+range]用于显示
        return std::clamp(combinedResponseDb(freq), -gainRangeDb_, gainRangeDb_);
    };
    const auto samples = adaptiveLogResponseSamples(responseDb);

    juce::Path path;
    monotonicCubicPath(path, samples, graphBounds_, gainRangeDb_);
    return path;
}

juce::Path EqGraphRenderer::buildSingleBandPath(int bandIndex) const
{
    const auto responseDb = [this, bandIndex](double freq) -> double {
        // 单band路径也按视图增益范围裁剪
        return std::clamp(filterResponseDb(bandIndex, freq), -gainRangeDb_, gainRangeDb_);
    };
    const auto samples = adaptiveLogResponseSamples(responseDb);

    juce::Path path;
    monotonicCubicPath(path, samples, graphBounds_, gainRangeDb_);
    return path;
}

// ============================================================================
// 背景/网格/轴标签
// ============================================================================

void EqGraphRenderer::drawBackground(juce::Graphics& g) const
{
    g.setColour(backgroundColor());
    g.fillRect(graphBounds_);
}

void EqGraphRenderer::drawGrid(juce::Graphics& g) const
{
    g.setColour(gridMajorColor());

    // 垂直网格线（频率）
    static const double freqs[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    for (double freq : freqs)
    {
        const float x = freqToX(freq);
        if (x >= graphBounds_.getX() && x <= graphBounds_.getRight())
            g.drawVerticalLine(static_cast<int>(x), graphBounds_.getY(), graphBounds_.getBottom());
    }

    // 水平网格线（增益）— 按 gainRangeDb_ 生成
    const double stepDb = gainRangeDb_ <= 12.0 ? 3.0 : 6.0;
    const int steps = static_cast<int>(std::round(gainRangeDb_ / stepDb));
    for (int i = -steps; i <= steps; ++i)
    {
        const double gainDb = static_cast<double>(i) * stepDb;
        const float y = gainToY(gainDb);
        if (y >= graphBounds_.getY() && y <= graphBounds_.getBottom())
            g.drawHorizontalLine(static_cast<int>(y), graphBounds_.getX(), graphBounds_.getRight());
    }

    // 0 dB 基准线（稍亮）
    g.setColour(axisLabelColor().withAlpha(0.6f));
    const float zeroY = gainToY(0.0);
    g.drawHorizontalLine(static_cast<int>(zeroY), graphBounds_.getX(), graphBounds_.getRight());
}

void EqGraphRenderer::drawAxisLabels(juce::Graphics& g) const
{
    g.setColour(axisLabelColor());
    g.setFont(juce::FontOptions(9.0f));

    // 底部频率标签
    static const struct { double freq; const char* label; } freqLabels[] = {
        { 20, "20" }, { 50, "50" }, { 100, "100" }, { 200, "200" },
        { 500, "500" }, { 1000, "1k" }, { 2000, "2k" }, { 5000, "5k" },
        { 10000, "10k" }, { 20000, "20k" }
    };
    for (const auto& fl : freqLabels)
    {
        const float x = freqToX(fl.freq);
        if (x >= graphBounds_.getX() && x <= graphBounds_.getRight())
            g.drawText(fl.label, static_cast<int>(x) - 10, static_cast<int>(graphBounds_.getBottom()) + 2,
                       20, 12, juce::Justification::centredTop, false);
    }

    // 右侧增益标签 — 按 gainRangeDb_ 生成
    const double stepDb = gainRangeDb_ <= 12.0 ? 3.0 : 6.0;
    const int steps = static_cast<int>(std::round(gainRangeDb_ / stepDb));
    for (int i = -steps; i <= steps; ++i)
    {
        const double gainDb = static_cast<double>(i) * stepDb;
        const float y = gainToY(gainDb);
        if (y >= graphBounds_.getY() && y <= graphBounds_.getBottom())
        {
            const juce::String text = (gainDb > 0 ? "+" : "") + juce::String(gainDb, 0);
            g.drawText(text, static_cast<int>(graphBounds_.getRight()) + 2, static_cast<int>(y) - 6,
                       30, 12, juce::Justification::centredLeft, false);
        }
    }
}

// ============================================================================
// 锚点绘制 — SRC 视觉：radius 4.75/hover 4.9/active 5.2, halo 5.6/7.1
// lighter 描边, 序号偏移双层描边
// ============================================================================

juce::Point<float> EqGraphRenderer::anchorPosition(int bandIndex) const
{
    double freq = 0.0;
    switch (bandIndex)
    {
    case 0: freq = settings_.lowCutFrequencyHz; break;
    case 1: freq = settings_.lowShelfFrequencyHz; break;
    case 2: freq = settings_.peakFrequencyHz; break;
    case 3: freq = settings_.highShelfFrequencyHz; break;
    case 4: freq = settings_.highCutFrequencyHz; break;
    }
    // SRC nodeGainDb: Cut uses filterResponseDb at cutoff, Shelf/Peak uses gainDb directly
    double gain = 0.0;
    if (bandIndex == 0 || bandIndex == 4)  // LowCut / HighCut
        gain = filterResponseDb(bandIndex, freq);
    else  // LowShelf / Peak / HighShelf
    {
        switch (bandIndex)
        {
        case 1: gain = settings_.lowShelfGainDb; break;
        case 2: gain = settings_.peakGainDb; break;
        case 3: gain = settings_.highShelfGainDb; break;
        }
    }
    return { freqToX(freq), gainToY(gain) };
}

int EqGraphRenderer::hitTestAnchor(juce::Point<float> pos, float threshold) const
{
    int closestBand = -1;
    float closestDist = threshold;

    for (int i = 0; i < 5; ++i)
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

void EqGraphRenderer::drawAnchors(juce::Graphics& g, int hoveredBand, bool showNumbers) const
{
    for (int i = 0; i < 5; ++i)
    {
        const auto pos = anchorPosition(i);
        // 锚点始终可见/可命中，不因 ±6 视图消失 — 不裁剪到 graphBounds_

        const bool isHovered = (i == hoveredBand);
        const auto color = bandColor(i);

        // SRC 锚点半径：normal 4.75, hover 4.9
        const float radius = isHovered ? kAnchorHoverRadius : kAnchorNormalRadius;
        // SRC 光晕：normal 5.6, hover 7.1
        const float haloRadius = isHovered ? kHaloHoverRadius : kHaloNormalRadius;

        // 径向光晕（halo）
        g.setColour(color.withAlpha(isHovered ? 0.30f : 0.15f));
        g.fillEllipse(pos.x - haloRadius, pos.y - haloRadius,
                      haloRadius * 2.0f, haloRadius * 2.0f);

        // 锚点填充圆
        g.setColour(color);
        g.fillEllipse(pos.x - radius, pos.y - radius,
                      radius * 2.0f, radius * 2.0f);

        // lighter 描边（SRC 风格，白色高光）
        g.setColour(juce::Colours::white.withAlpha(isHovered ? 0.85f : 0.60f));
        g.drawEllipse(pos.x - radius, pos.y - radius,
                      radius * 2.0f, radius * 2.0f, 1.25f);

        // 序号双层描边（黑色外层 + 白色内层，偏移避免遮挡锚点中心）
        if (showNumbers)
        {
            const float numOffsetX = radius + 5.0f;
            const float numOffsetY = -radius - 2.0f;

            // 黑色外层
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
            // 白色内层
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.drawText(juce::String(i + 1),
                       juce::Rectangle<float>(pos.x + numOffsetX, pos.y + numOffsetY,
                                              10.0f, 10.0f),
                       juce::Justification::centred, false);
        }
    }
}

// ============================================================================
// 十字引导线与 HUD（SRC 虚线节奏风格）
// ============================================================================

void EqGraphRenderer::drawCrosshairAndHud(juce::Graphics& g, juce::Point<float> pos,
                                          int bandIndex, const juce::String& hudText) const
{
    if (!graphBounds_.contains(pos))
        return;

    const auto color = bandColor(bandIndex);

    // 水平引导线（SRC 虚线节奏：alpha 0.25，使用点画线）
    g.setColour(color.withAlpha(0.25f));
    g.drawHorizontalLine(static_cast<int>(pos.y), graphBounds_.getX(), graphBounds_.getRight());

    // 垂直引导线
    g.drawVerticalLine(static_cast<int>(pos.x), graphBounds_.getY(), graphBounds_.getBottom());

    // HUD 文本框（圆角 4.0f，SRC 风格）
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
// 坐标反馈（SRC 轴边缘动态频率/增益标签与十字线，无浮动 HUD 盒）
// ============================================================================

void EqGraphRenderer::drawCoordReadout(juce::Graphics& g, juce::Point<float> pos,
                                       bool showGuides, float opacity) const
{
    if (!graphBounds_.contains(pos) || opacity <= 0.01f)
        return;

    const double freq = xToFreq(pos.x);
    const double gain = yToGain(pos.y);

    juce::String freqText;
    if (freq >= 1000.0)
        freqText = juce::String(freq / 1000.0, 2) + " kHz";
    else
        freqText = juce::String(freq, 1) + " Hz";

    juce::String gainText = juce::String(gain, 1) + " dB";

    g.setFont(juce::FontOptions(9.0f));
    const juce::Font readoutFont(juce::FontOptions(9.0f));

    // 用 GlyphArrangement 测量文本宽度
    auto measureWidth = [&readoutFont](const juce::String& text) -> float {
        juce::GlyphArrangement ga;
        ga.addLineOfText(readoutFont, text, 0.0f, 0.0f);
        return ga.getNumGlyphs() > 0
            ? ga.getBoundingBox(0, ga.getNumGlyphs(), false).getWidth()
            : 0.0f;
    };

    const float freqWidth = measureWidth(freqText) + 10.0f;
    const float gainWidth = measureWidth(gainText) + 10.0f;
    const float labelHeight = 18.0f;

    // 底部轴动态频率标签位置（SRC bottomAxisDynamicLabelRect 逻辑）
    juce::Rectangle<float> freqRect(pos.x - freqWidth * 0.5f,
                                    graphBounds_.getBottom() + 2.0f,
                                    freqWidth, labelHeight);
    // 夹紧到 graphBounds 下方
    freqRect.setX(juce::jmax(graphBounds_.getX() - 4.0f, freqRect.getX()));
    if (freqRect.getRight() > graphBounds_.getRight() + 30.0f)
        freqRect.setX(graphBounds_.getRight() + 30.0f - freqWidth);

    // 右侧轴动态增益标签位置（SRC rightAxisDynamicLabelRect 逻辑）
    juce::Rectangle<float> gainRect(graphBounds_.getRight() + 2.0f,
                                    pos.y - labelHeight * 0.5f,
                                    gainWidth, labelHeight);

    // 十字线 — SRC cross ticks
    if (showGuides)
    {
        const juce::Colour crossColor = juce::Colour::fromRGBA(245, 249, 255,
            static_cast<juce::uint8>(184.0f * opacity));
        g.setColour(crossColor);
        // 短十字刻度
        g.drawLine(pos.x - 7.0f, pos.y, pos.x - 2.0f, pos.y, 1.0f);
        g.drawLine(pos.x + 2.0f, pos.y, pos.x + 7.0f, pos.y, 1.0f);
        g.drawLine(pos.x, pos.y - 7.0f, pos.x, pos.y - 2.0f, 1.0f);
        g.drawLine(pos.x, pos.y + 2.0f, pos.x, pos.y + 7.0f, 1.0f);

        // 延伸引导线（到轴边缘）
        const juce::Colour guideColor = juce::Colour::fromRGBA(255, 255, 255,
            static_cast<juce::uint8>(42.0f * opacity));
        g.setColour(guideColor);
        // 垂直引导线：从 pos 到底部轴
        if (pos.y < graphBounds_.getBottom() - 2.0f)
            g.drawLine(pos.x, pos.y + 8.0f, pos.x, graphBounds_.getBottom() - 1.0f, 1.0f);
        // 水平引导线：从 pos 到右侧轴
        if (pos.x < graphBounds_.getRight() - 2.0f)
            g.drawLine(pos.x + 8.0f, pos.y, graphBounds_.getRight() - 1.0f, pos.y, 1.0f);
    }

    // 底部轴频率标签（SRC 双层描边：glow + 实色）
    {
        const juce::Colour glow = juce::Colour::fromRGBA(214, 230, 242,
            static_cast<juce::uint8>(42.0f * opacity));
        g.setColour(glow);
        g.drawText(freqText, freqRect.translated(0.0f, 0.45f),
                   juce::Justification::centred, false);
        const juce::Colour textCol = juce::Colour::fromRGBA(244, 248, 252,
            static_cast<juce::uint8>(244.0f * opacity));
        g.setColour(textCol);
        g.drawText(freqText, freqRect, juce::Justification::centred, false);
    }

    // 右侧轴增益标签
    {
        const juce::Colour glow = juce::Colour::fromRGBA(168, 216, 190,
            static_cast<juce::uint8>(40.0f * opacity));
        g.setColour(glow);
        g.drawText(gainText, gainRect.translated(0.0f, 0.45f),
                   juce::Justification::centred, false);
        const juce::Colour textCol = juce::Colour::fromRGBA(208, 234, 218,
            static_cast<juce::uint8>(238.0f * opacity));
        g.setColour(textCol);
        g.drawText(gainText, gainRect, juce::Justification::centred, false);
    }
}

// ============================================================================
// 图例（使用 juce::Font 直接测量，不用临时 Graphics）
// ============================================================================

float EqGraphRenderer::measureLegendItemWidth(const juce::String& label) const
{
    juce::GlyphArrangement ga;
    ga.addLineOfText(juce::Font(juce::FontOptions(9.0f)), label, 0.0f, 0.0f);
    return ga.getNumGlyphs() > 0
        ? ga.getBoundingBox(0, ga.getNumGlyphs(), false).getWidth()
        : 0.0f;
}

std::array<EqGraphRenderer::LegendItem, 6> EqGraphRenderer::buildLegendItems() const
{
    return {{
        { CurveId::Combined, "Combined", combinedCurveColor(), curveVisible_[0] },
        { CurveId::LowCut, "LowCut", bandColor(0), curveVisible_[1] },
        { CurveId::LowShelf, "LowShelf", bandColor(1), curveVisible_[2] },
        { CurveId::Peak, "Peak", bandColor(2), curveVisible_[3] },
        { CurveId::HighShelf, "HighShelf", bandColor(3), curveVisible_[4] },
        { CurveId::HighCut, "HighCut", bandColor(4), curveVisible_[5] },
    }};
}

juce::Rectangle<float> EqGraphRenderer::legendBounds(const std::array<LegendItem, 6>& items) const
{
    float totalWidth = 0.0f;
    const float itemHeight = 12.0f;
    const float gap = 8.0f;
    const float swatchW = 14.0f;

    for (const auto& item : items)
        totalWidth += swatchW + measureLegendItemWidth(item.label) + gap;

    // SRC: top-right of graph area, inset 14px from right edge, 12px from top
    return { graphBounds_.getRight() - totalWidth - 14.0f, graphBounds_.getY() + 12.0f, totalWidth, itemHeight };
}

void EqGraphRenderer::drawLegend(juce::Graphics& g, const std::array<LegendItem, 6>& items,
                                 int hoveredLegendIndex) const
{
    g.setFont(juce::FontOptions(9.0f));
    const float swatchW = 14.0f;
    const float gap = 8.0f;
    const float itemH = 12.0f;

    // SRC: top-right — compute total width to position from right edge
    float totalWidth = 0.0f;
    for (const auto& item : items)
        totalWidth += swatchW + measureLegendItemWidth(item.label) + gap;
    float x = graphBounds_.getRight() - totalWidth - 14.0f;
    const float y = graphBounds_.getY() + 12.0f;

    // SRC: semi-transparent rounded background with border
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

        // 色条
        auto color = item.color;
        if (!item.enabled)
            color = color.withAlpha(0.25f);
        g.setColour(color);
        g.fillRect(x, y + 2.0f, swatchW, itemH - 4.0f);

        // 文字
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
                                   const std::array<LegendItem, 6>& items) const
{
    const float swatchW = 14.0f;
    const float gap = 8.0f;
    const float itemH = 12.0f;

    // SRC: top-right — same layout as drawLegend/legendBounds
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
// 视图范围按钮 — SRC 双圆形 Decrease(+，30→12→6) / Increase(-，6→12→30)
// ============================================================================

juce::Rectangle<float> EqGraphRenderer::viewRangeButtonRect(int controlIndex) const
{
    // SRC viewRangeButtonRect: inset=10, buttonSize=32, buttonGap=8
    const float inset = 10.0f;
    const float buttonSize = kViewRangeCircleRadius * 2.0f;
    const float centerX = graphBounds_.getX() + inset + kViewRangeCircleRadius
                          + static_cast<float>(controlIndex) * (buttonSize + kViewRangeButtonGap);
    const float centerY = graphBounds_.getY() + inset + kViewRangeCircleRadius;
    return { centerX - kViewRangeCircleRadius, centerY - kViewRangeCircleRadius,
             buttonSize, buttonSize };
}

void EqGraphRenderer::drawViewRangeButtons(juce::Graphics& g,
                                           int hoveredControl, int pressedControl) const
{
    for (int ci = 0; ci < 2; ++ci)
    {
        const auto button = viewRangeButtonRect(ci);
        // SRC: face 24x24 centered in 32px button, pressed/hover shrink 1px
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

        // SRC: 符号 — Decrease(+), Increase(-)
        const float cx = button.getCentreX();
        const float cy = button.getCentreY();
        constexpr float half = 5.2f;
        g.setColour(juce::Colour::fromRGBA(220, 230, 238, 238));
        g.drawLine(cx - half, cy, cx + half, cy, 1.45f);
        if (ci == 0)  // Decrease = '+'
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
// 渲染入口
// ============================================================================

void EqGraphRenderer::drawPreview(juce::Graphics& g) const
{
    drawBackground(g);

    // bypass 时仍绘制全部曲线，以降低透明度表达 bypass 状态
    const float bypassAlpha = settings_.active ? 1.0f : 0.28f;

    // 单滤波器曲线（5 条，线宽 1.25）
    for (int i = 0; i < 5; ++i)
    {
        if (!curveVisible_[static_cast<size_t>(i + 1)])
            continue;
        const auto path = buildSingleBandPath(i);
        g.setColour(bandColor(i).withAlpha(0.6f * bypassAlpha));
        g.strokePath(path, juce::PathStrokeType(kSingleCurveWidth,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }

    // Combined 主曲线（线宽 2.25）
    if (curveVisible_[0])
    {
        const auto path = buildCombinedPath();
        g.setColour(combinedCurveColor().withAlpha(bypassAlpha));
        g.strokePath(path, juce::PathStrokeType(kCombinedCurveWidth,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }

    // 锚点（始终显示，bypass 时降低透明度，预览模式不显示序号）
    drawAnchors(g, -1, false);
}

void EqGraphRenderer::drawFull(juce::Graphics& g, int hoveredBand,
                               juce::Point<float> mousePos, bool isDragging,
                               int hoveredLegend, int hoveredViewRangeControl,
                               int pressedViewRangeControl) const
{
    drawBackground(g);
    drawGrid(g);
    drawAxisLabels(g);

    // bypass 时仍绘制全部曲线 + anchors + 图例 + 视图按钮，以降低透明度表达 bypass 状态
    const float bypassAlpha = settings_.active ? 1.0f : 0.28f;

    // 单滤波器曲线（5 条，线宽 1.25）
    for (int i = 0; i < 5; ++i)
    {
        if (!curveVisible_[static_cast<size_t>(i + 1)])
            continue;
        const auto path = buildSingleBandPath(i);
        g.setColour(bandColor(i).withAlpha(0.6f * bypassAlpha));
        g.strokePath(path, juce::PathStrokeType(kSingleCurveWidth,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }

    // Combined 主曲线（线宽 2.25）
    if (curveVisible_[0])
    {
        const auto path = buildCombinedPath();
        g.setColour(combinedCurveColor().withAlpha(bypassAlpha));
        g.strokePath(path, juce::PathStrokeType(kCombinedCurveWidth,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }

    // 十字引导线与 HUD（拖拽期间，仅 active 时显示）
    if (settings_.active && isDragging && hoveredBand >= 0 && hoveredBand < 5)
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

    // 坐标反馈 — 不在此处直接绘制，由 EqPopupComponent 的动画路径驱动 drawCoordReadout

    // 锚点（bypass 时仍显示，降低透明度）
    drawAnchors(g, hoveredBand);

    // 图例（bypass 时仍显示）
    const auto legendItems = buildLegendItems();
    drawLegend(g, legendItems, hoveredLegend);

    // 视图范围按钮（SRC 双圆形，bypass 时仍显示）
    drawViewRangeButtons(g, hoveredViewRangeControl, pressedViewRangeControl);
}

} // namespace OpenTune
