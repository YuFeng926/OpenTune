#include "ProcessF0Runtime.h"
#include "../Inference/F0InferenceService.h"
#include "../Inference/GameNoteGenerator.h"
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
    // 持有的 Env / 服务存活到进程退出，实例卸载不释放任何资源。
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
    // 只递减客户端租约计数：服务是进程寿命的（VST3 模块 pin / Standalone 进程
    // 寿命），不在此释放 f0Service_/ortEnv_/gameNoteGenerator_。后续实例复用
    // 同一 Env 与 Session，避免重建 ~350MB rmvpe 与重复模型加载。显式释放
    // 不存在：资源随进程退出由系统回收。
    std::lock_guard<std::mutex> lock(initMutex_);
    --clientCount_;
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
    if (ready_.load(std::memory_order_acquire)
        || initAttempted_.load(std::memory_order_acquire))
        return ready_.load(std::memory_order_acquire);

    AppPreferences preferences;
    return initialize(modelsDir, preferences.getF0ModelType());
}

bool ProcessF0Runtime::initialize(const std::string& modelsDir, F0ModelType initialModel)
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

        if (!f0Service_->initialize(modelsDir, initialModel))
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

std::shared_ptr<GameNoteGenerator> ProcessF0Runtime::createGameNoteGeneratorLocked()
{
    if (!ortEnv_)
        return nullptr;

    const auto modelsDir = ModelPathResolver::getModelsDirectory();
    const auto gameDir = juce::File(juce::String(modelsDir))
                             .getChildFile("GAME").getFullPathName().toStdString();

    try
    {
        return std::make_shared<GameNoteGenerator>(gameDir, *ortEnv_);
    }
    catch (const std::exception& e)
    {
        AppLogger::error(juce::String("[ProcessF0Runtime] GAME init failed: ") + e.what());
        return nullptr;
    }
}

std::vector<Note> ProcessF0Runtime::generateNotes(const NoteGeneratorInput& input)
{
    if (!isReady() && !initialize(ModelPathResolver::getModelsDirectory()))
        return {};

    // 进程级互斥串行全部 GAME 推理；同一临界区内惰性创建单一实例。
    // 锁序 gameMutex_ → initMutex_：其他成员（attach/detach/initialize/
    // getOrtEnv/getF0Service）只取 initMutex_，无反向嵌套，无环。
    std::lock_guard<std::mutex> gameLock(gameMutex_);
    std::shared_ptr<GameNoteGenerator> generator;
    {
        std::lock_guard<std::mutex> initLock(initMutex_);
        if (!gameNoteGenerator_)
            gameNoteGenerator_ = createGameNoteGeneratorLocked();
        generator = gameNoteGenerator_;
    }
    if (!generator)
        return {};

    return generator->generate(input);
}

} // namespace OpenTune
