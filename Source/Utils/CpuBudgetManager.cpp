#include "CpuBudgetManager.h"

#include <cmath>

namespace OpenTune {

int CpuBudgetManager::computeTotalBudget(unsigned int hardwareThreads)
{
    const unsigned int safeThreads = (hardwareThreads == 0) ? 4u : hardwareThreads;
    const int scaled = static_cast<int>(std::floor(static_cast<double>(safeThreads) * 0.60));
    return std::max(4, scaled);
}

CpuBudgetManager::BudgetConfig CpuBudgetManager::buildConfig(bool gpuMode, unsigned int hardwareThreads)
{
    BudgetConfig cfg;
    cfg.totalBudget = computeTotalBudget(hardwareThreads);

    if (gpuMode) {
        cfg.onnxIntra = 2;
    } else {
        cfg.onnxIntra = std::max(1, (cfg.totalBudget * 2) / 3);
    }

    cfg.onnxIntra = std::min(cfg.onnxIntra, std::max(1, cfg.totalBudget - 1));
    cfg.onnxInter = 1;
    cfg.renderWorkers = std::max(1, cfg.totalBudget - cfg.onnxIntra);

    // DML Session::Run() is NOT thread-safe - force single worker for GPU mode
    if (gpuMode) {
        cfg.renderWorkers = 1;
    }

    cfg.onnxSequential = true;
    cfg.allowSpinning = false;

    return cfg;
}

} // namespace OpenTune
