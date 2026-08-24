#pragma once

#include <vector>
#include <cmath>

namespace OpenTune {

/// 50 cents — 超过此阈值走声码器（DSP 移调在 50 音分内质量优于声码器）
constexpr float kPitchShiftThresholdCents = 50.0f;

/// 判断 chunk 是否需要声码器（任一有声帧偏差 >= 阈值）。
/// @param correctedF0    修正后 F0 数组（chunk 局部，100fps）
/// @param numF0Frames    correctedF0 帧数
/// @param originalF0     原始 F0 全局数组
/// @param f0StartFrame   chunk 在全局 F0 中的起始帧索引
/// @param thresholdCents 偏差阈值（默认 50 cents）
/// @return true = 需要声码器；false = 可用 AutoTune cycle resampling
inline bool chunkNeedsVocoder(
    const float* correctedF0,
    int numF0Frames,
    const std::vector<float>& originalF0,
    int f0StartFrame,
    float thresholdCents = kPitchShiftThresholdCents)
{
    for (int i = 0; i < numF0Frames; ++i) {
        const int globalFrame = f0StartFrame + i;
        if (globalFrame < 0 || globalFrame >= static_cast<int>(originalF0.size()))
            continue;

        const float corrected = correctedF0[i];
        const float original = originalF0[static_cast<size_t>(globalFrame)];

        // 跳过无声帧
        if (corrected <= 0.0f || original <= 0.0f) continue;

        const float deviationCents = std::abs(1200.0f * std::log2f(corrected / original));
        if (deviationCents >= thresholdCents)
            return true;
    }
    return false;
}

} // namespace OpenTune