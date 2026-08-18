/**
 * EQ Graph Renderer — 动态滤波器列表曲线渲染引擎
 *
 * 视觉数学从 SRC (EQGraphWidget.h/.cpp) 迁移到 UI 层，
 * 严禁调用 NoteEqProcessor/DSP magnitude。
 * 动态迭代 settings_.filters，不假设固定数量。
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>
#include <vector>
#include <array>
#include <functional>

#include "Utils/NoteEqSettings.h"

namespace OpenTune {

class EqGraphRenderer {
public:
    // ── 范围常量（源自 SRC） ──
    static constexpr double kMinFrequencyHz = 20.0;
    static constexpr double kMaxFrequencyHz = 20000.0;

    // ── 视觉数学常量（源自 SRC EQGraphWidget.cpp） ──
    static constexpr int kBaseSegments = 240;
    static constexpr double kCurvatureDb = 0.22;
    static constexpr int kMaxSubdivisionDepth = 3;
    static constexpr double kFixedShelfCutRatio = 2.0;

    // ── 视图范围 ──
    static constexpr double kViewRange6 = 6.0;
    static constexpr double kViewRange12 = 12.0;
    static constexpr double kViewRange30 = 30.0;

    // ── 曲线常量 ──
    static constexpr float kCombinedCurveWidth = 2.25f;
    static constexpr float kSingleCurveWidth = 1.25f;

    // ── SRC 锚点半径 ──
    static constexpr float kAnchorNormalRadius  = 4.75f;
    static constexpr float kAnchorHoverRadius   = 4.9f;
    static constexpr float kAnchorActiveRadius  = 5.2f;
    static constexpr float kHaloNormalRadius    = 5.6f;
    static constexpr float kHaloHoverRadius     = 7.1f;

    // ── ViewRange 双圆形按钮 ──
    static constexpr float kViewRangeCircleRadius = 16.0f;
    static constexpr float kViewRangeButtonGap = 8.0f;

    // ── 10 色调色板（SRC 滤波器 + 频谱颜色循环共享） ──
    static constexpr int kPaletteSize = 10;
    static const std::array<juce::Colour, kPaletteSize> kPalette;

    // ── 颜色 ──
    static juce::Colour backgroundColor();
    static juce::Colour gridMajorColor();
    static juce::Colour axisLabelColor();
    static juce::Colour combinedCurveColor();
    static juce::Colour hudBgColor();
    static juce::Colour hudTextColor();
    // 按 paletteSlot 查 10 色（无效 slot → fallback 按 type）
    juce::Colour bandColor(int filterIndex) const;

    EqGraphRenderer() = default;

    // ── 坐标映射 ──
    void setGraphBounds(juce::Rectangle<float> bounds) { graphBounds_ = bounds; }
    void setViewGainRangeDb(double rangeDb) { gainRangeDb_ = std::clamp(rangeDb, 6.0, 30.0); }
    void setSettings(const EqSettings& s) { settings_ = s; }
    void setPreviewFreqRange(double minHz, double maxHz) { previewMinFreq_ = minHz; previewMaxFreq_ = maxHz; }
    void clearPreviewFreqRange() { previewMinFreq_ = kMinFrequencyHz; previewMaxFreq_ = kMaxFrequencyHz; }
    double viewGainRangeDb() const { return gainRangeDb_; }
    juce::Rectangle<float> graphBounds() const { return graphBounds_; }

    float freqToX(double frequencyHz) const;
    double xToFreq(float x) const;
    float gainToY(double gainDb) const;
    double yToGain(float y) const;

    // ── 视觉响应计算（严格复刻 SRC filterResponseDb，使用动态 filter 类型/Q） ──
    double filterResponseDb(int filterIndex, double frequencyHz) const;
    double combinedResponseDb(double frequencyHz) const;

    // ── 曲线路径 ──
    juce::Path buildCombinedPath() const;
    juce::Path buildSingleBandPath(int filterIndex) const;
    juce::Path buildBandInfluencePath(int filterIndex) const;

    // ── 渲染入口 ──
    void drawPreview(juce::Graphics& g) const;
    void drawFull(juce::Graphics& g, int hoveredBand = -1,
                  juce::Point<float> mousePos = {}, bool isDragging = false,
                  int hoveredViewRangeControl = -1,
                  int pressedViewRangeControl = -1,
                  const std::vector<double>& hoverBandAmounts = {},
                  int activeCardBand = -1) const;

    // ── 频谱背景动画 ──
    void drawSpectrumBackground(juce::Graphics& g,
                                const std::array<float, 128>& spectrum,
                                const std::array<float, 128>& peaks) const;
    /// 每帧由 timer 调用一次，推进频谱颜色循环插值
    void advanceSpectrumColorCycle(double dt);

    // ── 辉光质量（1=低, 2=中, 3=高） ──
    void setGlowQuality(int quality) { glowQuality_ = std::clamp(quality, 1, 3); }
    int glowQuality() const { return glowQuality_; }

    // ── 图例（动态数量） ──
    struct LegendItem {
        bool isCombined = false;
        int filterIndex = -1;  // -1 = combined, 0..N-1 = filter index
        juce::String label;
        juce::Colour color;
        bool enabled = true;
    };
    std::vector<LegendItem> buildLegendItems() const;
    void drawLegend(juce::Graphics& g, const std::vector<LegendItem>& items,
                    int hoveredLegendIndex = -1) const;
    int hitTestLegend(juce::Point<float> pos, const std::vector<LegendItem>& items) const;
    juce::Rectangle<float> legendBounds(const std::vector<LegendItem>& items) const;

    // ── 视图范围按钮 ──
    enum class ViewRangeButton { None, Decrease, Increase };
    void drawViewRangeButtons(juce::Graphics& g,
                              int hoveredControl = -1, int pressedControl = -1) const;
    ViewRangeButton hitTestViewRangeButton(juce::Point<float> pos) const;
    juce::Rectangle<float> viewRangeButtonRect(int controlIndex) const;

    // ── 锚点 ──
    void drawAnchors(juce::Graphics& g, int hoveredBand = -1, bool showNumbers = true) const;
    juce::Point<float> anchorPosition(int filterIndex) const;
    int hitTestAnchor(juce::Point<float> pos, float threshold = 10.0f) const;

    // ── 曲线级 hover 检测 ──
    float curveYAtX(int filterIndex, float x) const;
    int hitTestCurve(juce::Point<float> pos, float threshold = 12.0f) const;

    // ── 十字引导线与 HUD ──
    void drawCrosshairAndHud(juce::Graphics& g, juce::Point<float> pos,
                             int bandIndex, const juce::String& hudText) const;

    // ── 曲线可见性（图例交互） ──
    void setCurveVisible(int filterIndex, bool visible)
    {
        if (filterIndex >= 0 && filterIndex < EqSettings::kMaxFilters)
            curveVisible_[static_cast<size_t>(filterIndex)] = visible;
    }
    bool isCurveVisible(int filterIndex) const
    {
        if (filterIndex >= 0 && filterIndex < EqSettings::kMaxFilters)
            return curveVisible_[static_cast<size_t>(filterIndex)];
        return true;
    }

    // ── paletteSlot 辅助 ──
    static void assignPaletteSlots(EqSettings& settings);
    static int getPaletteSlot(const EqFilter& f);

private:
    juce::Rectangle<float> graphBounds_;
    double gainRangeDb_ = 12.0;
    double previewMinFreq_ = kMinFrequencyHz;
    double previewMaxFreq_ = kMaxFrequencyHz;
    EqSettings settings_;
    std::array<bool, static_cast<size_t>(EqSettings::kMaxFilters)> curveVisible_ = [] {
        std::array<bool, static_cast<size_t>(EqSettings::kMaxFilters)> visible{};
        visible.fill(true);
        return visible;
    }();
    mutable std::array<float, 128> spectrumPeakTrace_{};
    int glowQuality_ = 2;

    // ── 频谱颜色循环状态 ──
    int spectrumColorIndex_ = 0;
    int spectrumTargetIndex_ = 1;
    float spectrumColorProgress_ = 0.0f;
    static constexpr float kSpectrumColorCycleInterval = 3.0f;
    float spectrumColorTimer_ = 0.0f;

    static double logGaussian(double frequencyHz, double centerHz, double widthOctaves);
    double normToFrequency(double norm) const;

    struct ResponseSample { double norm; double gainDb; };
    std::vector<ResponseSample> adaptiveLogResponseSamples(
        const std::function<double(double)>& responseDb) const;
    static void monotonicCubicPath(juce::Path& path, const std::vector<ResponseSample>& samples,
                                   juce::Rectangle<float> graphBounds, double gainRangeDb);
    void drawGrid(juce::Graphics& g) const;
    void drawAxisLabels(juce::Graphics& g) const;

    float measureLegendItemWidth(const juce::String& label) const;

    /// 旧 type-based fallback（paletteSlot 无效时使用）
    static juce::Colour typeFallbackColor(EqFilterType type);

    // ── 频谱热力图颜色（4色水平梯度，按频率位置着色） ──
    static juce::Colour spectrumHeatColor(float norm);
    // ── Catmull-Rom 样条路径构建 ──
    static juce::Path catmullRomLinePath(const std::array<juce::Point<float>, 128>& points, int count);
    static juce::Path catmullRomFillPath(const std::array<juce::Point<float>, 128>& points, int count, float baseline);
};

} // namespace OpenTune
