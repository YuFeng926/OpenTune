/**
 * EQ Graph Renderer — 严格复刻 SRC 视觉数学的曲线渲染引擎
 *
 * 视觉数学从 SRC (EQGraphWidget.h/.cpp) 迁移到 UI 层，
 * 严禁调用 NoteEqProcessor/DSP magnitude。
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
    // 数据增益范围：±12 dB（数据裁剪边界，不等于视图范围）
    static constexpr double kMinGainDb = -12.0;
    static constexpr double kMaxGainDb = 12.0;

    // ── 视觉数学常量（源自 SRC EQGraphWidget.cpp） ──
    static constexpr int kBaseSegments = 240;
    static constexpr double kCurvatureDb = 0.22;
    static constexpr int kMaxSubdivisionDepth = 3;
    static constexpr double kFixedShelfCutRatio = 2.0;
    static constexpr double kPeakQ = 2.0;

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

    // ── ViewRange 双圆形按钮（SRC: Decrease=+，Increase=-） ──
    static constexpr float kViewRangeCircleRadius = 16.0f;  // 直径 32px
    static constexpr float kViewRangeButtonGap = 8.0f;

    // ── 图例曲线标识 ──
    enum class CurveId { Combined, LowCut, LowShelf, Peak, HighShelf, HighCut, Count };

    // ── 颜色（静态函数避免 constexpr 初始化问题） ──
    static juce::Colour backgroundColor();
    static juce::Colour gridMajorColor();
    static juce::Colour axisLabelColor();
    static juce::Colour combinedCurveColor();
    static juce::Colour hudBgColor();
    static juce::Colour hudTextColor();
    static juce::Colour bandColor(int bandIndex);

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

    // ── 视觉响应计算（严格复刻 SRC filterResponseDb） ──
    double filterResponseDb(int bandIndex, double frequencyHz) const;
    double combinedResponseDb(double frequencyHz) const;

    // ── 曲线路径 ──
    juce::Path buildCombinedPath() const;
    juce::Path buildSingleBandPath(int bandIndex) const;
    juce::Path buildBandInfluencePath(int bandIndex) const;

    // ── 渲染入口 ──
    void drawPreview(juce::Graphics& g) const;
    void drawFull(juce::Graphics& g, int hoveredBand = -1,
                  juce::Point<float> mousePos = {}, bool isDragging = false,
                  int hoveredViewRangeControl = -1,
                  int pressedViewRangeControl = -1) const;

    // ── 图例 ──
    struct LegendItem {
        CurveId id;
        juce::String label;
        juce::Colour color;
        bool enabled = true;
    };
    std::array<LegendItem, 6> buildLegendItems() const;
    void drawLegend(juce::Graphics& g, const std::array<LegendItem, 6>& items,
                    int hoveredLegendIndex = -1) const;
    int hitTestLegend(juce::Point<float> pos, const std::array<LegendItem, 6>& items) const;
    juce::Rectangle<float> legendBounds(const std::array<LegendItem, 6>& items) const;

    // ── 视图范围按钮（SRC 双圆形：Decrease(+，30→12→6) / Increase(-，6→12→30)） ──
    enum class ViewRangeButton { None, Decrease, Increase };
    void drawViewRangeButtons(juce::Graphics& g,
                              int hoveredControl = -1, int pressedControl = -1) const;
    ViewRangeButton hitTestViewRangeButton(juce::Point<float> pos) const;
    juce::Rectangle<float> viewRangeButtonRect(int controlIndex) const;

    // ── 锚点（SRC 视觉：halo + lighter 描边 + 序号双层描边） ──
    void drawAnchors(juce::Graphics& g, int hoveredBand = -1, bool showNumbers = true) const;
    juce::Point<float> anchorPosition(int bandIndex) const;
    int hitTestAnchor(juce::Point<float> pos, float threshold = 10.0f) const;

    // ── 十字引导线与 HUD（SRC 虚线节奏） ──
    void drawCrosshairAndHud(juce::Graphics& g, juce::Point<float> pos,
                             int bandIndex, const juce::String& hudText) const;

    // ── 坐标反馈（SRC 轴边缘动态频率/增益标签与十字线，无浮动 HUD 盒） ──
    void drawCoordReadout(juce::Graphics& g, juce::Point<float> pos,
                          bool showGuides, float opacity = 1.0f) const;

    // ── 曲线可见性（图例交互） ──
    void setCurveVisible(CurveId id, bool visible) { curveVisible_[static_cast<int>(id)] = visible; }
    bool isCurveVisible(CurveId id) const { return curveVisible_[static_cast<int>(id)]; }

private:
    juce::Rectangle<float> graphBounds_;
    double gainRangeDb_ = 12.0;
    double previewMinFreq_ = kMinFrequencyHz;
    double previewMaxFreq_ = kMaxFrequencyHz;
    EqSettings settings_;
    std::array<bool, static_cast<size_t>(CurveId::Count)> curveVisible_ = { true, true, true, true, true, true };

    static double logGaussian(double frequencyHz, double centerHz, double widthOctaves);
    static double normToFrequency(double norm);

    struct ResponseSample { double norm; double gainDb; };
    std::vector<ResponseSample> adaptiveLogResponseSamples(
        const std::function<double(double)>& responseDb) const;
    // monotonicCubicPath 使用传入的 gainRangeDb 做 Y 映射
    static void monotonicCubicPath(juce::Path& path, const std::vector<ResponseSample>& samples,
                                   juce::Rectangle<float> graphBounds, double gainRangeDb);
    void drawGrid(juce::Graphics& g) const;
    void drawAxisLabels(juce::Graphics& g) const;
    void drawBackground(juce::Graphics& g) const;

    // 图例布局辅助（使用 juce::Font 直接测量，不用临时 Graphics）
    float measureLegendItemWidth(const juce::String& label) const;
};

} // namespace OpenTune
