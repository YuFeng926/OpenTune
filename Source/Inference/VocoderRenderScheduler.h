#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <vector>

namespace OpenTune {

class VocoderInferenceService;

/**
 * VocoderRenderScheduler - Serial task queue for Vocoder rendering
 * 
 * Enforces single-threaded execution of Vocoder synthesis tasks.
 * Vocoder DML sessions require serial Run() calls - this scheduler
 * ensures no concurrent synthesis operations.
 * 
 * Thread-safe: Yes (mutex-protected queue)
 * Concurrency: Strictly serial (one job at a time)
 */
class VocoderRenderScheduler {
public:
    struct Job {
        std::vector<float> f0;
        std::vector<float> energy;
        std::vector<float> mel;
        std::function<void(bool, const juce::String&, const std::vector<float>&)> onComplete;
    };

    VocoderRenderScheduler();
    ~VocoderRenderScheduler();

    /**
     * Initialize scheduler with vocoder service
     * @param service Vocoder inference service (must outlive scheduler)
     * @return true if initialization successful
     */
    bool initialize(VocoderInferenceService* service);

    /**
     * Shutdown and drain queue
     */
    void shutdown();

    /**
     * Submit a rendering job to the queue
     * @param job Job to execute
     */
    void submit(Job job);

    /**
     * Get current queue depth
     */
    int getQueueDepth() const;

    /**
     * Check if scheduler is accepting jobs
     */
    bool isRunning() const;

private:
    void workerThread();

    VocoderInferenceService* service_{nullptr};
    std::queue<Job> jobQueue_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::unique_ptr<std::thread> worker_;
    
    std::atomic<bool> acceptingJobs_{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocoderRenderScheduler)
};

} // namespace OpenTune
