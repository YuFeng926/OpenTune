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
 *
 * Process-lifetime heap singleton: getInstance() allocates once and never
 * destroys it, so no static destructor ever runs at DLL detach (loader lock
 * held). Resources are released explicitly on the last detach() in the normal
 * destruction context. Client lease: attach() at construction, detach() at
 * destruction; when the count reaches zero, detach() resets f0Service_ then
 * ortEnv_ inside the same initMutex_ critical section, eliminating the
 * lock-free shutdown race.
 */
class ProcessF0Runtime
{
public:
    static ProcessF0Runtime& getInstance();

    /// 客户端注册（构造时调用；正常上下文）
    void attach();
    /// 客户端注销（析构时调用）；计数归零时在 initMutex_ 临界区内按
    /// f0Service_ → ortEnv_ 顺序释放并复位初始化标志
    void detach();

    bool initialize(const std::string& modelsDir);

    std::shared_ptr<Ort::Env> getOrtEnv() const;
    std::shared_ptr<F0InferenceService> getF0Service() const;
    bool isReady() const { return ready_.load(std::memory_order_acquire); }

private:
    ProcessF0Runtime() = default;
    ~ProcessF0Runtime() = default;

    std::shared_ptr<Ort::Env> ortEnv_;
    std::shared_ptr<F0InferenceService> f0Service_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> initAttempted_{false};
    mutable std::mutex initMutex_;
    int clientCount_{0};  // initMutex_ 保护

    ProcessF0Runtime(const ProcessF0Runtime&) = delete;
    ProcessF0Runtime& operator=(const ProcessF0Runtime&) = delete;
};

} // namespace OpenTune
