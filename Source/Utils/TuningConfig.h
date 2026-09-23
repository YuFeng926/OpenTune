#pragma once

#include <atomic>

namespace OpenTune::TuningConfig {

constexpr float kDefaultTuningHz = 440.0f;
constexpr float kMinTuningHz = 400.0f;
constexpr float kMaxTuningHz = 500.0f;

/// Runtime tuning frequency (Hz)：唯一原子存储。
/// 写：AppPreferences::setTuning/load → setCurrentTuningHz（消息线程）。
/// 读：currentTuningHz() 返回当下值（任意线程，无数据竞争）。
/// 需要"一次运算使用同一值"的调用方（GAME/Legacy 生成）在入口取一次快照向下传参。
inline std::atomic<float> s_tuningHz{kDefaultTuningHz};

inline float currentTuningHz() {
    return s_tuningHz.load(std::memory_order_relaxed);
}

inline void setCurrentTuningHz(float hz) {
    s_tuningHz.store(hz, std::memory_order_relaxed);
}

struct TuningSettings {
    float tuningHz = kDefaultTuningHz;
    
    static TuningSettings getDefault() {
        return TuningSettings{};
    }
};

} // namespace OpenTune::TuningConfig
