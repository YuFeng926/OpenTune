#pragma once

/**
 * 频谱显示常量 — DSP 与 UI 共享。
 *
 * 684 个对数采样频率点，覆盖 20 Hz – 20 kHz。
 * 用于 OutputSpectrumAnalyzer 输出和 EqGraphRenderer 绘制。
 */

#include <array>

namespace OpenTune {

static constexpr int kSpectrumDisplayPoints = 684;
using SpectrumArray = std::array<float, kSpectrumDisplayPoints>;

} // namespace OpenTune
