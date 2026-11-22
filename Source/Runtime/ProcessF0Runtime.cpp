#include "ProcessF0Runtime.h"
#include "../Inference/F0InferenceService.h"
#include "../Services/F0ExtractionService.h"
#include "../Utils/ModelPathResolver.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppPreferences.h"
#include "../Utils/AppLogger.h"

#include <onnxruntime_cxx_api.h>
#include <exception>

namespace OpenTune {

ProcessF0Runtime& ProcessF0Runtime::getInstance()
{
    // 进程寿命 heap 单例：显式分配、永不析构，避免 DLL detach（持 loader lock）
    // 时执行静态析构。VST3 构建中模块已被 pin（Vst3ModulePin.cpp），单例与其
    // 持有的 Env / 服务存活到进程退出；F0 session 由 extractF0 按需创建并返回前析构。
    static auto* instance = new ProcessF0Runtime();
    return *instance;
}

F0ExtractionService* ProcessF0Runtime::araAnalysisQueue()
{
    std::lock_guard<std::mutex> lock(araQueueMutex_);
    if (!araQueue_ && !araQueueClosing_)
        araQueue_.reset(new F0ExtractionService(100));
    return araQueue_.get();
}

void ProcessF0Runtime::shutdownAraAnalysisAndJoin()
{
    F0ExtractionService* queue;
    {
        std::lock_guard<std::mutex> lock(araQueueMutex_);
        araQueueClosing_ = true;
        queue = araQueue_.get();
    }
    if (queue)
        queue->shutdown();
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

bool ProcessF0Runtime::initialize(const std::string& modelsDir, std::string* errorMessage)
{
    if (ready_.load(std::memory_order_acquire))
        return true;

    AppPreferences preferences;
    return initialize(modelsDir, preferences.getF0ModelType(), errorMessage);
}

bool ProcessF0Runtime::initialize(const std::string& modelsDir,
                                 F0ModelType initialModel,
                                 std::string* errorMessage)
{
    if (ready_.load(std::memory_order_acquire))
        return true;

    std::lock_guard<std::mutex> lock(initMutex_);

    if (ready_.load(std::memory_order_acquire))
        return true;

    std::string onnxLoadReport;
    if (!ModelPathResolver::ensureOnnxRuntimeLoaded(&onnxLoadReport))
    {
        if (errorMessage != nullptr)
            *errorMessage = "ONNX Runtime loading failed: " + onnxLoadReport;
        AppLogger::log("ProcessF0Runtime: ensureOnnxRuntimeLoaded failed: "
            + juce::String(onnxLoadReport));
        return false;
    }
    AppLogger::log("ProcessF0Runtime: onnxRuntime=" + juce::String(onnxLoadReport)
        + " modelsDir=" + juce::String(modelsDir));

    try
    {
        AccelerationDetector::getInstance().detect();
        Ort::InitApi();
        ortEnv_ = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OpenTune");
        f0Service_ = std::make_shared<F0InferenceService>(ortEnv_);

        if (!f0Service_->initialize(modelsDir, initialModel, errorMessage))
        {
            AppLogger::log("ProcessF0Runtime: F0 initialize failed");
            f0Service_.reset();
            ortEnv_.reset();
            return false;
        }

        ready_.store(true, std::memory_order_release);
        AppLogger::log("ProcessF0Runtime: F0 inference service configured (session created on demand)");
    }
    catch (const std::exception& e)
    {
        if (errorMessage != nullptr)
            *errorMessage = "F0 runtime initialization threw std::exception: " + std::string(e.what());
        AppLogger::log("ProcessF0Runtime: exception: " + juce::String(e.what()));
        f0Service_.reset();
        ortEnv_.reset();
        return false;
    }
    catch (...)
    {
        if (errorMessage != nullptr)
            *errorMessage = "F0 runtime initialization threw an unknown exception";
        f0Service_.reset();
        ortEnv_.reset();
        return false;
    }

    return true;
}

} // namespace OpenTune
