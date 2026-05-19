/**
 * VadExtractionService — async VAD extraction task manager.
 *
 * Mirrors F0ExtractionService's submit/cancel pattern (request key dedupe +
 * token-based cancellation + worker pool) so call sites that already manage
 * F0 extraction lifecycle can be wired symmetrically.
 *
 * Spec: openspec/changes/vocal-time-stretch/specs/silero-vad-extractor/spec.md
 *
 * Phase H scaffolding (vocal-time-stretch §4.5):
 *   This is the wiring; the actual VAD inference body lives in
 *   `VadInferenceService::extractVad` (which falls back to empty when the
 *   Silero model is missing).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../Utils/LockFreeQueue.h"

namespace OpenTune {

class VadExtractionService {
public:
    struct Result {
        bool        success{false};
        uint64_t    materializationId{0};
        uint64_t    requestKey{0};
        uint64_t    requestToken{0};
        std::vector<float> vadProb;        // VAD probability per frame (native rate)
        int         hopSize{0};
        int         vadSampleRate{0};
        std::string errorMessage;
    };

    using ExecuteFn = std::function<Result()>;
    using CommitFn  = std::function<void(Result&&)>;

    enum class SubmitResult : uint8_t {
        Accepted,
        AlreadyInProgress,
        QueueFull,
        InvalidTask
    };

    explicit VadExtractionService(int workerCount = 1, size_t maxQueueSize = 64);
    ~VadExtractionService();

    static uint64_t makeRequestKey(uint64_t materializationId, int trackId, int placementIndex);

    SubmitResult submit(uint64_t requestKey, ExecuteFn execute, CommitFn commit);

    bool isActive(uint64_t requestKey) const;
    void cancel(uint64_t requestKey);

private:
    struct Task {
        uint64_t requestKey{0};
        uint64_t token{0};
        ExecuteFn execute;
        CommitFn  commit;
    };
    struct ActiveEntry {
        std::atomic<uint64_t> token{0};
    };

    void workerLoop();

    LockFreeQueue<Task> queue_;
    std::unordered_map<uint64_t, std::unique_ptr<ActiveEntry>> activeEntries_;
    mutable std::mutex entriesMutex_;
    std::vector<std::thread> workers_;
    size_t maxQueueSize_{64};
    std::atomic<uint64_t> tokenCounter_{1};
    std::atomic<bool> running_{true};
};

} // namespace OpenTune
