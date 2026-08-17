/**
 * EQ Spectrum Animation — 合成频谱动态背景
 *
 * 生成时间变化的频谱数据，用于 EQ 弹窗背景动画。
 * 视觉风格参考 数字资产/design/screenshots/fft-background.png。
 * 不依赖真实音频数据，用多层正弦波合成。
 */

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "Utils/NoteEqSettings.h"

namespace OpenTune {

class EqSpectrumAnimation {
public:
    static constexpr int kNumBins = 128;

    EqSpectrumAnimation() { spectrum_.fill(0.0f); }

    void update(double timeSeconds, const EqSettings& settings)
    {
        for (int i = 0; i < kNumBins; ++i)
        {
            const double norm = static_cast<double>(i) / static_cast<double>(kNumBins - 1);
            const double freq = 20.0 * std::pow(1000.0, norm);

            double energy = 0.0;

            // 多层正弦波叠加 — 模拟频谱能量分布
            energy += 0.35 * std::sin(timeSeconds * 1.2 + norm * 6.0);
            energy += 0.22 * std::sin(timeSeconds * 0.8 + norm * 3.5 + 1.3);
            energy += 0.15 * std::sin(timeSeconds * 2.1 + norm * 9.2 + 0.7);
            energy += 0.10 * std::sin(timeSeconds * 0.5 + norm * 2.1 + 2.5);
            energy += 0.08 * std::sin(timeSeconds * 3.3 + norm * 12.0 + 4.1);

            // 低频基础能量（模拟音乐低频隆起）
            const double lowBoost = std::exp(-norm * 3.0) * 0.4;
            energy += lowBoost * (0.6 + 0.4 * std::sin(timeSeconds * 0.7 + 0.5));

            // 高频衰减
            const double highAtten = std::exp(-norm * 1.5);
            energy *= highAtten;

            // EQ 曲线调制 — 让频谱响应 EQ 变化
            double eqModulation = 1.0;
            const double lowCutRatio = settings.lowCutFrequencyHz / freq;
            eqModulation *= 1.0 / (1.0 + std::pow(lowCutRatio, 4.0));
            const double highCutRatio = freq / settings.highCutFrequencyHz;
            eqModulation *= 1.0 / (1.0 + std::pow(highCutRatio, 4.0));

            // 归一化到 [0, 1]
            const double raw = (energy + 0.5) * eqModulation;
            spectrum_[static_cast<size_t>(i)] = static_cast<float>(
                std::clamp(raw, 0.0, 1.0));
        }
    }

    float bin(int index) const
    {
        return spectrum_[static_cast<size_t>(std::clamp(index, 0, kNumBins - 1))];
    }

    const std::array<float, kNumBins>& data() const { return spectrum_; }

private:
    std::array<float, kNumBins> spectrum_;
};

} // namespace OpenTune
