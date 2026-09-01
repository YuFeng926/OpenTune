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
 * F0InferenceService - F0 extraction service (process-level shared service)
 *
 * Responsibilities:
 * - Configure F0 extraction: store model directory, selected model type, and
 *   extraction parameters (confidence, f0Min, f0Max) without loading an ONNX
 *   session. isInitialized() reflects "service configured", not "session loaded".
 * - On-demand session lifecycle: each extractF0() call creates a local
 *   Ort::Session via ModelFactory, runs inference, and destroys the session
 *   before returning. No session persists across calls.
 * - Serialize inference: DML contract allows only one Run() at a time;
 *   runMutex_ admission gate ensures serial execution.
 * - Run lease admission gate: waiters queue on runCv_; cancellation is per-owner
 *   state (pending leases are flagged cancelled and exit without taking the gate,
 *   the active lease is terminated via SetTerminate)
 * - Owner closure is persistent: terminateActiveRun sets state->closed; late-arriving
 *   leases from a closed owner are rejected at admission
 * - Handle model switching: setF0Model() updates the selected model without
 *   creating a session; the next admitted extractF0() uses the new model.
 *   An admitted call keeps its configuration snapshot for the whole extraction.
 *
 * Thread-safe: Yes (runMutex_ serializes all Runs; extractorMutex_ guards config)
 * Lifecycle: Ort::Env/service are process-level; sessions are created on demand
 *   per extractF0() call and destroyed before the call returns.
 */
class F0InferenceService {
public:
    F0InferenceService(std::shared_ptr<Ort::Env> env);
    ~F0InferenceService();

    /**
     * Configure F0 service: save model directory and selected model type.
     * Does NOT load an ONNX session; sessions are created on demand in extractF0().
     * @param modelDir Path to model directory
     * @return true if model file exists and service is configured
     */
    bool initialize(const std::string& modelDir,
                    F0ModelType initialModel = F0ModelType::FCPE);

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
     * Set F0 model type (does not create a session; next extractF0 uses new model)
     * @param type F0 model type (e.g., FCPE)
     * @return true if model file exists and selection updated
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
