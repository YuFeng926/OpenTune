#pragma once

#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>

#include "../Inference/IF0Extractor.h"

namespace Ort { struct Env; }

namespace OpenTune {

class F0InferenceService;
class GameNoteGenerator;
struct NoteGeneratorInput;
struct Note;

/**
 * Process-level inference runtime singleton.
 * Owns the single Ort::Env, the process-level F0InferenceService and
 * the process-level GameNoteGenerator (GAME-small, single lazy instance).
 * All processors and document controllers share this one instance.
 *
 * Process-lifetime heap singleton: getInstance() allocates once and never
 * destroys it, so no static destructor ever runs at DLL detach (loader lock
 * held). In the VST3 build the module itself is pinned for process lifetime
 * (Source/Utils/Vst3ModulePin.cpp), so Env/service state survives instance
 * teardown and is released only at process exit. The F0 service configures
 * on init and creates/destroys ONNX sessions on demand per extraction call.
 * Actual owners (processors, DCs) resolve the service via getF0Service();
 * there is no attach/detach lease — the singleton is process-lifetime.
 */
class ProcessF0Runtime
{
public:
    static ProcessF0Runtime& getInstance();

    bool initialize(const std::string& modelsDir);
    bool initialize(const std::string& modelsDir, F0ModelType initialModel);

    std::shared_ptr<Ort::Env> getOrtEnv() const;
    std::shared_ptr<F0InferenceService> getF0Service() const;
    bool isReady() const { return ready_.load(std::memory_order_acquire); }

    /**
     * Process-level GAME transcription entry point (single interface):
     * lazily creates the single GameNoteGenerator on the shared Ort::Env and
     * runs generate() serialized by the process-level gameMutex_. The shared
     * generator is process-lifetime and never terminated for a single owner
     * (terminateRun 已删除：owner 卸载后活跃 GAME 推理允许在后台完成，调用方
     * 经 completion gate 丢弃结果)；调用方不得持有 per-processor GAME session。
     * Returns {} on failure (Env not initialized / model load failure) —
     * matching GameNoteGenerator::generate's failure signal.
     */
    std::vector<Note> generateNotes(const NoteGeneratorInput& input);

private:
    ProcessF0Runtime() = default;
    ~ProcessF0Runtime() = default;

    // 锁前置条件：调用方已持有 initMutex_。在共享 ortEnv_ 上创建单一 GAME
    // 实例；失败返回 nullptr，下一次显式提交时重试创建（无后台重试循环）。
    std::shared_ptr<GameNoteGenerator> createGameNoteGeneratorLocked();

    std::shared_ptr<Ort::Env> ortEnv_;
    std::shared_ptr<F0InferenceService> f0Service_;
    std::shared_ptr<GameNoteGenerator> gameNoteGenerator_;  // initMutex_ 保护
    std::atomic<bool> ready_{false};
    mutable std::mutex initMutex_;
    mutable std::mutex gameMutex_;  // 进程级 GAME 推理串行互斥

    ProcessF0Runtime(const ProcessF0Runtime&) = delete;
    ProcessF0Runtime& operator=(const ProcessF0Runtime&) = delete;
};

} // namespace OpenTune
