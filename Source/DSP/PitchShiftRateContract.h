#pragma once

#include <cmath>

namespace OpenTune {

/// 轻量 AutoTune 周期重采样与声码器路径的唯一分界：75 音分。
constexpr float kPitchShiftThresholdCents = 75.0f;

/// 重采样倍率是否处于给定音分范围内。阈值边界本身属于合同内。
inline bool isPitchShiftRateWithinCents(double resampleRate, float thresholdCents)
{
    const double cents = static_cast<double>(thresholdCents);
    return resampleRate >= std::exp2(-cents / 1200.0)
        && resampleRate <= std::exp2(cents / 1200.0);
}

/// 实际请求的重采样倍率是否处于轻量 AutoTune 的 ±75 音分合同内。
inline bool isPitchShiftRateWithinLightContract(double resampleRate)
{
    return isPitchShiftRateWithinCents(resampleRate, kPitchShiftThresholdCents);
}

} // namespace OpenTune
