#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "../Inference/IF0Extractor.h"

namespace Ort { struct Env; }

namespace OpenTune {

class F0ExtractionService;
class F0InferenceService;

/**
 * Process-level inference runtime singleton.
 * Owns the single Ort::Env and the process-level F0InferenceService.
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

    bool initialize(const std::string& modelsDir, std::string* errorMessage = nullptr);
    bool initialize(const std::string& modelsDir, F0ModelType initialModel,
                    std::string* errorMessage = nullptr);

    std::shared_ptr<Ort::Env> getOrtEnv() const;
    std::shared_ptr<F0InferenceService> getF0Service() const;
    bool isReady() const { return ready_.load(std::memory_order_acquire); }

    F0ExtractionService* araAnalysisQueue();
    void shutdownAraAnalysisAndJoin();

private:
    ProcessF0Runtime() = default;
    ~ProcessF0Runtime() = default;

    std::shared_ptr<Ort::Env> ortEnv_;
    std::shared_ptr<F0InferenceService> f0Service_;
    std::atomic<bool> ready_{false};
    mutable std::mutex initMutex_;

    std::mutex araQueueMutex_;
    std::unique_ptr<F0ExtractionService> araQueue_;
    bool araQueueClosing_{false};

    ProcessF0Runtime(const ProcessF0Runtime&) = delete;
    ProcessF0Runtime& operator=(const ProcessF0Runtime&) = delete;
};

} // namespace OpenTune
