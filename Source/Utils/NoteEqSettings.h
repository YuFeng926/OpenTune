#pragma once

/**
 * Per-note EQ settings — 动态滤波器链数据契约
 *
 * 支持最多 kMaxFilters 个滤波器，每个滤波器含 type/frequency/gain/q/paletteSlot。
 * active 控制全局启用/旁通；filters 向量控制滤波器链。
 *
 * 默认构造生成旧 5 段固定结构（LowCut 80 / LowShelf 500 0dB / Peak 3000 0dB q=2 /
 * HighShelf 8000 0dB / HighCut 12000），保证旧数据语义兼容。
 *
 * Cut 类型（LowCut/HighCut）的 gain 规范为 0（DSP 不使用）。
 * Q 可保存，Peak 默认 2.0，UI 后续只按 src 规则给 Peak 调整。
 */

#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>

namespace OpenTune {

constexpr int kEqMaxFilters = 10;  // 上限，同时作为 paletteSlot 范围的上界

enum class EqFilterType : uint8_t {
    LowCut,
    LowShelf,
    Peak,
    HighShelf,
    HighCut
};

struct EqFilter {
    EqFilterType type = EqFilterType::Peak;
    float frequencyHz = 1000.0f;
    float gainDb = 0.0f;
    float q = 2.0f;
    int paletteSlot = 0;  // 0..9, stable identity within EqSettings
    bool bypassed = false;

    bool operator==(const EqFilter& other) const noexcept
    {
        return type == other.type
            && frequencyHz == other.frequencyHz
            && gainDb == other.gainDb
            && q == other.q
            && paletteSlot == other.paletteSlot
            && bypassed == other.bypassed;
    }

    bool operator!=(const EqFilter& other) const noexcept
    {
        return !(*this == other);
    }

    bool isValid() const noexcept
    {
        const int t = static_cast<int>(type);
        if (t < 0 || t > 4)
            return false;
        if (paletteSlot < 0 || paletteSlot >= kEqMaxFilters)
            return false;
        if (!std::isfinite(frequencyHz) || !std::isfinite(gainDb) || !std::isfinite(q))
            return false;
        if (gainDb < -12.0f || gainDb > 12.0f)
            return false;
        if (q < 0.25f || q > 9.0f)
            return false;
        switch (type) {
        case EqFilterType::LowCut:
        case EqFilterType::HighCut:
            if (frequencyHz < 20.0f || frequencyHz > 20000.0f) return false;
            if (gainDb != 0.0f) return false;
            break;
        case EqFilterType::LowShelf:
        case EqFilterType::HighShelf:
            if (frequencyHz < 20.0f || frequencyHz > 20000.0f) return false;
            break;
        case EqFilterType::Peak:
            if (frequencyHz < 500.0f || frequencyHz > 12000.0f) return false;
            break;
        default:
            return false;
        }
        return true;
    }
};

struct EqSettings {
    static constexpr int kMaxFilters = kEqMaxFilters;

    // 频率范围 Hz
    static constexpr float kMinFrequencyHz = 20.0f;
    static constexpr float kMaxFrequencyHz = 20000.0f;

    // 增益范围 dB
    static constexpr float kMinGainDb = -12.0f;
    static constexpr float kMaxGainDb = 12.0f;

    // Q 范围
    static constexpr float kMinQ = 0.25f;
    static constexpr float kMaxQ = 9.0f;

    // 枚举有效上限：LowCut=0 … HighCut=4
    static constexpr uint8_t kMaxFilterType = static_cast<uint8_t>(EqFilterType::HighCut);

    bool active = true;
    std::vector<EqFilter> filters;

    // 默认构造：3 段（LowShelf / Peak / HighShelf），不预置高切/低切
    EqSettings()
        : filters{
              { EqFilterType::LowShelf, 500.0f,  0.0f, 2.0f,   0 },
              { EqFilterType::Peak,     3000.0f, 0.0f, 2.0f,   1 },
              { EqFilterType::HighShelf,8000.0f, 0.0f, 2.0f,   2 }
          }
    {
    }

    bool operator==(const EqSettings& other) const noexcept
    {
        return active == other.active && filters == other.filters;
    }

    bool operator!=(const EqSettings& other) const noexcept
    {
        return !(*this == other);
    }

    /** Remove filter at index; returns true if filters became empty (caller should remove EQ entirely). */
    bool removeFilter(int index)
    {
        if (index < 0 || index >= static_cast<int>(filters.size()))
            return false;
        filters.erase(filters.begin() + index);
        return filters.empty();
    }

    bool isValid() const noexcept
    {
        if (filters.size() < 1 || filters.size() > static_cast<size_t>(kMaxFilters))
            return false;
        for (const auto& f : filters)
            if (!f.isValid())
                return false;
        // paletteSlot 唯一性校验
        bool used[kEqMaxFilters] = {};
        for (const auto& f : filters) {
            if (used[f.paletteSlot])
                return false;
            used[f.paletteSlot] = true;
        }
        return true;
    }
};

} // namespace OpenTune
