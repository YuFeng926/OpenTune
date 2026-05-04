#pragma once

#include <cmath>
#include <algorithm>

namespace OpenTune {
namespace PitchUtils {

/**
 * 以目标音高为基准位置，叠加原始音高相对于目标的波动形状。
 * retuneSpeed 控制波动的抑制程度：
 *   1.0 = 纯平目标音高（完全修正）
 *   0.0 = 保留原始音高（不修正）
 * @param shiftedF0   原始音高（提供波动形状）
 * @param targetF0    目标音高（绝对基准位置）
 * @param retuneSpeed 修正强度 (0.0 到 1.0)
 * @return 锚定在 targetF0 位置上、带有缩减波动的频率 (Hz)
 */
inline float mixRetune(float shiftedF0, float targetF0, float retuneSpeed) {
    if (shiftedF0 <= 0.0f) return targetF0;
    if (targetF0 <= 0.0f) return shiftedF0;
    float logDeviation = std::log2(shiftedF0) - std::log2(targetF0);
    float scaledDeviation = logDeviation * (1.0f - std::max(0.0f, std::min(1.0f, retuneSpeed)));
    return std::pow(2.0f, std::log2(targetF0) + scaledDeviation);
}

/**
 * 将频率 (Hz) 转换为 MIDI 编号 (float)。
 */
inline float freqToMidi(float freq) {
    if (freq <= 0.0f) return 0.0f;
    return 69.0f + 12.0f * std::log2(freq / 440.0f);
}

/**
 * 将 MIDI 编号转换为频率 (Hz)。
 */
inline float midiToFreq(float midi) {
    if (midi <= 0.0f) return 0.0f;
    return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

} // namespace PitchUtils
} // namespace OpenTune
