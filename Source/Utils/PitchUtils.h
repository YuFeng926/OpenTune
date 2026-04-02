#pragma once

#include <cmath>
#include <algorithm>

namespace OpenTune {
namespace PitchUtils {

/**
 * 在平移后的原始音高（保留细节）和目标音高（完全平直）之间进行混合。
 * @param shiftedF0 0% Retune Speed 时的音高（已平移到目标音域）
 * @param targetF0 100% Retune Speed 时的音高（目标音符的中心频率）
 * @param retuneSpeed 混合系数 (0.0 到 1.0)
 * @return 混合后的频率 (Hz)
 */
inline float mixRetune(float shiftedF0, float targetF0, float retuneSpeed) {
    if (shiftedF0 <= 0.0f) return targetF0;
    if (targetF0 <= 0.0f) return shiftedF0;
    
    // 使用对数空间进行音高混合，以保持音程比例
    float logShifted = std::log2(shiftedF0);
    float logTarget = std::log2(targetF0);
    float logResult = logShifted + (logTarget - logShifted) * std::max(0.0f, std::min(1.0f, retuneSpeed));
    
    return std::pow(2.0f, logResult);
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
