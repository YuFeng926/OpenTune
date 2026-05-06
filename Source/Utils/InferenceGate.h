#pragma once

#include <mutex>
#include <atomic>

namespace OpenTune {

/**
 * InferenceGate - Serializes large model inference operations.
 *
 * Ensures RMVPE and GAME never run concurrently (they share GPU/memory budget).
 * Vocoder does NOT use this gate (it's streaming, not batch).
 *
 * Thread-safe: Yes (mutex-based serialization)
 */
class InferenceGate {
public:
    /**
     * Acquire exclusive access for a large inference operation.
     * Blocks until the gate is available. Returns RAII lock.
     */
    [[nodiscard]] std::unique_lock<std::mutex> acquire() {
        return std::unique_lock<std::mutex>(mutex_);
    }

    /**
     * Request cancellation of the current inference operation.
     * The running operation must poll isCancelled() to honor this.
     */
    void requestCancel() { cancelled_.store(true, std::memory_order_release); }

    /**
     * Check if cancellation has been requested.
     */
    bool isCancelled() const { return cancelled_.load(std::memory_order_acquire); }

    /**
     * Clear the cancellation flag (call before starting a new operation).
     */
    void clearCancel() { cancelled_.store(false, std::memory_order_release); }

private:
    std::mutex mutex_;
    std::atomic<bool> cancelled_{false};
};

} // namespace OpenTune
