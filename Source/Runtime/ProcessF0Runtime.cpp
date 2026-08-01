#include "ProcessF0Runtime.h"
#include "../Inference/F0InferenceService.h"
#include "../Utils/ModelPathResolver.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"

#include <onnxruntime_cxx_api.h>

namespace OpenTune {

ProcessF0Runtime& ProcessF0Runtime::getInstance()
{
    // 进程寿命 heap 单例：显式分配、永不析构，避免 DLL detach（持 loader lock）
    // 时执行静态析构。资源由最后一次 detach() 在正常上下文释放。
    static auto* instance = new ProcessF0Runtime();
    return *instance;
}

void ProcessF0Runtime::attach()
{
    std::lock_guard<std::mutex> lock(initMutex_);
    ++clientCount_;
}

void ProcessF0Runtime::detach()
{
    // 同一临界区内递减并释放：与 initialize/getF0Service 线性化，无锁外关闭竞态。
    // 先释放 f0Service_（其内部持 env_ 引用）再释放 ortEnv_。
    std::lock_guard<std::mutex> lock(initMutex_);
    --clientCount_;
    if (clientCount_ == 0) {
        f0Service_.reset();
        ortEnv_.reset();
        // 允许下次 initialize() 重新初始化：释放而非永久禁用
        initAttempted_.store(false, std::memory_order_release);
        ready_.store(false, std::memory_order_release);
    }
}

std::shared_ptr<Ort::Env> ProcessF0Runtime::getOrtEnv() const
{
    std::lock_guard<std::mutex> lock(initMutex_);
    return ortEnv_;
}

std::shared_ptr<F0InferenceService> ProcessF0Runtime::getF0Service() const
{
    std::lock_guard<std::mutex> lock(initMutex_);
    return f0Service_;
}

bool ProcessF0Runtime::initialize(const std::string& modelsDir)
{
    if (ready_.load(std::memory_order_acquire))
        return true;

    if (initAttempted_.load(std::memory_order_acquire))
        return ready_.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> lock(initMutex_);

    if (initAttempted_.load(std::memory_order_acquire))
        return ready_.load(std::memory_order_acquire);

    initAttempted_.store(true, std::memory_order_release);

    if (!ModelPathResolver::ensureOnnxRuntimeLoaded())
    {
        AppLogger::log("ProcessF0Runtime: ensureOnnxRuntimeLoaded failed");
        return false;
    }

    AccelerationDetector::getInstance().detect();

    try
    {
        Ort::InitApi();
        ortEnv_ = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OpenTune");
        f0Service_ = std::make_shared<F0InferenceService>(ortEnv_);

        if (!f0Service_->initialize(modelsDir))
        {
            AppLogger::log("ProcessF0Runtime: F0 initialize failed");
            f0Service_.reset();
            ortEnv_.reset();
            return false;
        }

        ready_.store(true, std::memory_order_release);
        AppLogger::log("ProcessF0Runtime: F0 inference service initialized (process-level)");
    }
    catch (const std::exception& e)
    {
        AppLogger::log("ProcessF0Runtime: exception: " + juce::String(e.what()));
        f0Service_.reset();
        ortEnv_.reset();
        return false;
    }

    return true;
}

} // namespace OpenTune
