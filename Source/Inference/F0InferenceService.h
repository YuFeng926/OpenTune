#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include "IF0Extractor.h"
#include "../Utils/Error.h"

namespace Ort { struct Env; }

namespace OpenTune {

/**
 * F0 run owner identity for termination. One F0ExtractionService owns exactly one
 * state for its whole lifetime; shared_ptr ownership removes raw-pointer identity
 * (address reuse can never alias a live owner). closed is set under the service's
 * runMutex_ by terminateActiveRun and checked at lease admission under the same
 * lock, so a closed owner can never start a new Run.
 */
struct F0RunOwnerState {
    std::atomic<bool> closed{false};
};

/**
 * F0InferenceService - F0 extraction service (process-level shared ONNX session)
 *
 * Responsibilities:
 * - Manage F0 extractor lifecycle (shared session across all callers)
 * - Serialize inference: DML contract allows only one Run() on a session at a time
 * - Run lease admission gate: waiters queue on runCv_; cancellation is per-owner
 *   state (pending leases are flagged cancelled and exit without taking the gate,
 *   the active lease is terminated via SetTerminate)
 * - Owner closure is persistent: terminateActiveRun sets state->closed; late-arriving
 *   leases from a closed owner are rejected at admission
 * - Single state lock (runMutex_) linearizes lease admission, termination and
 *   RunOptions mutation, eliminating TOCTOU between owner checks and SetTerminate
 * - Handle model switching and configuration
 *
 * Thread-safe: Yes (runMutex_ serializes all Runs; extractorMutex_ guards model access)
 * Lifecycle: Model loaded on demand, released explicitly by caller after use
 */
class F0InferenceService {
public:
    F0InferenceService(std::shared_ptr<Ort::Env> env);
    ~F0InferenceService();

    /**
     * Initialize F0 service with model directory
     * @param modelDir Path to model directory
     * @return true if initialization successful
     */
    bool initialize(const std::string& modelDir);

    /**
     * Extract F0 from audio
     * @param audio Audio samples
     * @param length Number of samples
     * @param sampleRate Sample rate
     * @param ownerState Owner identity for run termination
     * @param progressCallback Optional progress callback (0.0 to 1.0)
     * @param partialCallback Optional partial result callback
     * @return Result containing F0 vector
     */
    Result<std::vector<float>> extractF0(
        const float* audio,
        size_t length,
        int sampleRate,
        std::shared_ptr<F0RunOwnerState> ownerState,
        std::function<void(float)> progressCallback = nullptr,
        std::function<void(const std::vector<float>&, int)> partialCallback = nullptr);

    /**
     * Permanently close the given owner: set state->closed, flag all pending
     * (waiting) leases sharing that state as cancelled, and terminate the
     * active Run if it shares that state. A closed owner never re-enters, so
     * late-arriving leases (e.g. a worker that dequeued before closure but has
     * not yet reached admission) are rejected at admission. All under the same
     * state lock, so a waiter can never be terminated after it has already
     * taken the gate.
     * Thread-safe: may be called from another thread while a Run is executing.
     * @param ownerState Owner state to close permanently
     */
    void terminateActiveRun(const std::shared_ptr<F0RunOwnerState>& ownerState);

    /**
     * Set F0 model type
     * @param type F0 model type (e.g., RMVPE)
     * @return true if model switched successfully
     */
    bool setF0Model(F0ModelType type);

    /**
     * Get current F0 model type
     */
    F0ModelType getCurrentF0Model() const;

    /**
     * Get available F0 models in model directory
     */
    std::vector<F0ModelInfo> getAvailableF0Models() const;

    /**
     * Configure F0 extraction parameters
     */
    void setConfidenceThreshold(float threshold);
    void setF0Min(float minFreq);
    void setF0Max(float maxFreq);

    float getConfidenceThreshold() const;
    float getF0Min() const;
    float getF0Max() const;

    /**
     * Get F0 extraction parameters
     */
    int getF0HopSize() const;
    int getF0SampleRate() const;

    bool isInitialized() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(F0InferenceService)
};

} // namespace OpenTune
