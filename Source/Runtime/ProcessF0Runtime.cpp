#include "ProcessF0Runtime.h"
#include "../Inference/F0InferenceService.h"
#include "../Utils/ModelPathResolver.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"

#include <onnxruntime_cxx_api.h>

namespace OpenTune {

ProcessF0Runtime& ProcessF0Runtime::getInstance()
{
    static ProcessF0Runtime instance;
    return instance;
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
            ortEnv_.reset();
            f0Service_.reset();
            return false;
        }

        ready_.store(true, std::memory_order_release);
        AppLogger::log("ProcessF0Runtime: F0 inference service initialized (process-level)");
    }
    catch (const std::exception& e)
    {
        AppLogger::log("ProcessF0Runtime: exception: " + juce::String(e.what()));
        ortEnv_.reset();
        f0Service_.reset();
        return false;
    }

    return true;
}

} // namespace OpenTune
