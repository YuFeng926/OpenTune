#include "CpuBudgetManager.h"

#include <algorithm>

namespace OpenTune {

int CpuBudgetManager::computeTotalBudget(unsigned int hardwareThreads)
{
    const unsigned int safeThreads = (hardwareThreads == 0) ? 4u : hardwareThreads;
    // 留 2 线程给 UI 消息线程 + 音频线程，剩余全部给推理
    return std::max(1, static_cast<int>(safeThreads) - 2);
}

CpuBudgetManager::BudgetConfig CpuBudgetManager::buildConfig(bool gpuMode, unsigned int hardwareThreads)
{
    BudgetConfig cfg;
    cfg.totalBudget = computeTotalBudget(hardwareThreads);

    if (gpuMode) {
        cfg.onnxIntra = 2;
    } else {
        cfg.onnxIntra = cfg.totalBudget;
    }

    cfg.onnxInter = 1;
    cfg.onnxSequential = true;
    cfg.allowSpinning = false;

    return cfg;
}

} // namespace OpenTune
