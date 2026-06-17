#pragma once

#include <memory>
#include <string>
#include <atomic>
#include <mutex>

namespace Ort { struct Env; }

namespace OpenTune {

class F0InferenceService;

/**
 * Process-level F0 inference runtime singleton.
 * Owns the single Ort::Env and F0InferenceService (rmvpe.onnx ~350MB).
 * All processors and document controllers share this one instance.
 */
class ProcessF0Runtime
{
public:
    static ProcessF0Runtime& getInstance();

    bool initialize(const std::string& modelsDir);

    std::shared_ptr<Ort::Env> getOrtEnv() const { return ortEnv_; }
    std::shared_ptr<F0InferenceService> getF0Service() const { return f0Service_; }
    bool isReady() const { return ready_.load(std::memory_order_acquire); }

private:
    ProcessF0Runtime() = default;
    ~ProcessF0Runtime() = default;

    std::shared_ptr<Ort::Env> ortEnv_;
    std::shared_ptr<F0InferenceService> f0Service_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> initAttempted_{false};
    std::mutex initMutex_;

    ProcessF0Runtime(const ProcessF0Runtime&) = delete;
    ProcessF0Runtime& operator=(const ProcessF0Runtime&) = delete;
};

} // namespace OpenTune
