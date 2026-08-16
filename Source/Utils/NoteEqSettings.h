#pragma once

/**
 * Per-note EQ settings — 数据契约（docs/plans/2026-08-13-per-note-eq-tool-plan.md 第 2 节）
 *
 * 5 段固定结构，段数与段类型固定，不可增减、不可改类型；type 与 Q 由 DSP 层常量确定，
 * 不进数据、不进持久化。本结构只保存 9 个用户可调字段：
 *
 * active                // 全局启用；false = 保留设置但全局旁通
 * lowCutFrequencyHz     // 20–20000 Hz，默认 80
 * lowShelfFrequencyHz   // 20–20000 Hz，默认 500
 * lowShelfGainDb        // ±12 dB，默认 0
 * peakFrequencyHz       // 500–12000 Hz，默认 3000
 * peakGainDb            // ±12 dB，默认 0
 * highShelfFrequencyHz  // 20–20000 Hz，默认 8000
 * highShelfGainDb       // ±12 dB，默认 0
 * highCutFrequencyHz    // 20–20000 Hz，默认 12000
 *
 * 禁止保存固定不变的 type 与 Q；禁止 per-band bypass。
 */

namespace OpenTune {

struct EqSettings {
    bool active = true;                    // 全局启用；false = 保留设置但全局旁通

    float lowCutFrequencyHz = 80.0f;       // LowCut 截止频率
    float lowShelfFrequencyHz = 500.0f;    // LowShelf 中心频率
    float lowShelfGainDb = 0.0f;           // LowShelf 增益
    float peakFrequencyHz = 3000.0f;       // Peak 中心频率
    float peakGainDb = 0.0f;               // Peak 增益
    float highShelfFrequencyHz = 8000.0f;  // HighShelf 中心频率
    float highShelfGainDb = 0.0f;          // HighShelf 增益
    float highCutFrequencyHz = 12000.0f;   // HighCut 截止频率

    // 精确比较 9 个用户可调字段，无容差（C++17 optional<EqSettings> 依赖此 ==）。
    bool operator==(const EqSettings& other) const noexcept
    {
        return active == other.active
            && lowCutFrequencyHz == other.lowCutFrequencyHz
            && lowShelfFrequencyHz == other.lowShelfFrequencyHz
            && lowShelfGainDb == other.lowShelfGainDb
            && peakFrequencyHz == other.peakFrequencyHz
            && peakGainDb == other.peakGainDb
            && highShelfFrequencyHz == other.highShelfFrequencyHz
            && highShelfGainDb == other.highShelfGainDb
            && highCutFrequencyHz == other.highCutFrequencyHz;
    }

    bool operator!=(const EqSettings& other) const noexcept
    {
        return !(*this == other);
    }
};

} // namespace OpenTune
