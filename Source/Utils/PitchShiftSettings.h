#pragma once

#include <cmath>
#include <cstdint>

namespace OpenTune {

/**
 * Pitch Shift 设置（clip 级整体移调渲染修饰器）
 *
 * 轻量值类型，存储在 ContentEntry 中，与 notes / correctionSegments / TimeGrid 同级。
 * 不改写 OriginalF0 真值，仅作为渲染链路的全局偏移修饰器。
 */
struct PitchShiftSettings {
    int semitone{0};    // [-24, +24] 半音
    int cents{0};       // [-99, +99] 音分

    /** 是否为恒等（无偏移） */
    bool isIdentity() const noexcept
    {
        return semitone == 0 && cents == 0;
    }

    /** 计算音高比率：2^((semitone + cents/100) / 12) */
    double getPitchRatio() const noexcept
    {
        if (isIdentity())
            return 1.0;
        const double totalSemitones = static_cast<double>(semitone) + static_cast<double>(cents) / 100.0;
        return std::pow(2.0, totalSemitones / 12.0);
    }

    /** 获取总偏移量（以音分为单位） */
    double getTotalCents() const noexcept
    {
        return static_cast<double>(semitone) * 100.0 + static_cast<double>(cents);
    }

    /** 恒等设置工厂 */
    static PitchShiftSettings identity() noexcept
    {
        return PitchShiftSettings{0, 0};
    }

    bool operator==(const PitchShiftSettings& other) const noexcept
    {
        return semitone == other.semitone && cents == other.cents;
    }

    bool operator!=(const PitchShiftSettings& other) const noexcept
    {
        return !(*this == other);
    }
};

} // namespace OpenTune
