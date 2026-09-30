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
    // 持有的 Env / 服务存活到进程退出；F0 session 由 extractF0 按需创建并返回前析构。
    static auto* instance = new ProcessF0Runtime();
    return *instance;
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

    AppPreferences preferences;
    return initialize(modelsDir, preferences.getF0ModelType());
}

bool ProcessF0Runtime::initialize(const std::string& modelsDir, F0ModelType initialModel)
{
    if (ready_.load(std::memory_order_acquire))
        return true;

    std::lock_guard<std::mutex> lock(initMutex_);

    if (ready_.load(std::memory_order_acquire))
        return true;

    std::string onnxLoadReport;
    if (!ModelPathResolver::ensureOnnxRuntimeLoaded(&onnxLoadReport))
    {
        AppLogger::log("ProcessF0Runtime: ensureOnnxRuntimeLoaded failed: "
            + juce::String(onnxLoadReport));
        return false;
    }
    AppLogger::log("ProcessF0Runtime: onnxRuntime=" + juce::String(onnxLoadReport)
        + " modelsDir=" + juce::String(modelsDir));

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
        AppLogger::log("ProcessF0Runtime: F0 inference service configured (session created on demand)");
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

bool ProcessF0Runtime::submitNotes(NoteGeneratorInput input,
                                   std::function<void(std::vector<Note>)> completion)
{
    std::lock_guard<std::mutex> lifecycleLock(gameLifecycleMutex_);
    std::lock_guard<std::mutex> lock(gameQueueMutex_);
    if (gameStopping_)
        gameStopping_ = false;
    if (gameQueue_.size() >= 100)
        return false;
    bool appended = false;
    try
    {
        gameQueue_.push_back({std::move(input), std::move(completion)});
        appended = true;
        if (!gameWorker_.joinable())
            gameWorker_ = std::thread([this] { gameWorkerLoop(); });
    }
    catch (const std::exception& e)
    {
        if (appended)
            gameQueue_.pop_back();
        AppLogger::error(juce::String("[ProcessF0Runtime] GAME submission failed: ") + e.what());
        return false;
    }
    catch (...)
    {
        if (appended)
            gameQueue_.pop_back();
        AppLogger::error("[ProcessF0Runtime] GAME submission failed: unknown exception");
        return false;
    }
    gameQueueCv_.notify_one();
    return true;
}

void ProcessF0Runtime::gameWorkerLoop()
{
    for (;;) {
        GameJob job;
        {
            std::unique_lock<std::mutex> lock(gameQueueMutex_);
            gameQueueCv_.wait(lock, [this] {
                return gameStopping_ || !gameQueue_.empty();
            });
            if (gameStopping_ && gameQueue_.empty())
                return;
            job = std::move(gameQueue_.front());
            gameQueue_.pop_front();
        }
        std::vector<Note> notes;
        try
        {
            notes = generateNotes(job.input);
        }
        catch (const std::exception& e)
        {
            AppLogger::error(juce::String("[ProcessF0Runtime] GAME worker failed: ") + e.what());
        }
        catch (...)
        {
            AppLogger::error("[ProcessF0Runtime] GAME worker failed: unknown exception");
        }

        try
        {
            if (job.completion)
                job.completion(std::move(notes));
        }
        catch (const std::exception& e)
        {
            AppLogger::error(juce::String("[ProcessF0Runtime] GAME completion failed: ") + e.what());
        }
        catch (...)
        {
            AppLogger::error("[ProcessF0Runtime] GAME completion failed: unknown exception");
        }
    }
}

void ProcessF0Runtime::shutdown() noexcept
{
    std::lock_guard<std::mutex> lifecycleLock(gameLifecycleMutex_);
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(gameQueueMutex_);
        gameStopping_ = true;
        gameQueue_.clear();
        worker = std::move(gameWorker_);
    }
    gameQueueCv_.notify_all();
    if (worker.joinable())
        worker.join();
}

} // namespace OpenTune
