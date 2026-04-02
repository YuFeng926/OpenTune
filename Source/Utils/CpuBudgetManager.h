#pragma once

#include <algorithm>
#include <thread>

namespace OpenTune {

class CpuBudgetManager {
public:
    struct BudgetConfig {
        int totalBudget{4};
        int onnxIntra{1};
        int onnxInter{1};
        bool onnxSequential{true};
        bool allowSpinning{false};
    };

    static int computeTotalBudget(unsigned int hardwareThreads);
    // 线程预算在初始化时按 gpuMode 固定，不随 playback 状态动态变化。
    static BudgetConfig buildConfig(bool gpuMode, unsigned int hardwareThreads = std::thread::hardware_concurrency());
};

} // namespace OpenTune
