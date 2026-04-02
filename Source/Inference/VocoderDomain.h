#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <vector>
#include <functional>
#include "../Utils/Error.h"

namespace OpenTune {

/**
 * VocoderDomain - Encapsulates Vocoder inference and scheduling as a single unit
 * 
 * This class exists to enforce lifecycle constraints:
 * - VocoderRenderScheduler depends on VocoderInferenceService
 * - By encapsulating both, we guarantee correct initialization/shutdown order
 * - The scheduler cannot outlive the service (eliminates dangling pointer risk)
 * 
 * Ownership model:
 * - VocoderDomain OWNS both VocoderInferenceService and VocoderRenderScheduler
 * - External code borrows references through accessors
 * - Lifecycle is managed as a single unit
 * 
 * Thread-safe: Yes (delegates to internal components)
 * Concurrency: Serial synthesis (enforced by internal scheduler)
 */
class VocoderDomain {
public:
    struct Job {
        std::vector<float> f0;
        std::vector<float> energy;
        std::vector<float> mel;
        std::function<void(bool, const juce::String&, const std::vector<float>&)> onComplete;
    };

    VocoderDomain();
    ~VocoderDomain();

    /**
     * Initialize the vocoder domain
     * Creates inference service, then scheduler (correct dependency order)
     * @param modelDir Path to model directory
     * @return true if initialization successful
     */
    bool initialize(const std::string& modelDir);

    /**
     * Shutdown and cleanup
     * Stops scheduler first, then inference service (reverse of init order)
     */
    void shutdown();

    /**
     * Submit a rendering job
     */
    void submit(Job job);

    /**
     * Get current queue depth
     */
    int getQueueDepth() const;

    /**
     * Check if domain is running
     */
    bool isRunning() const;

    /**
     * Check if domain is initialized
     */
    bool isInitialized() const;

    /**
     * Get vocoder hop size
     */
    int getVocoderHopSize() const;

    /**
     * Get mel bins required by vocoder model
     */
    int getMelBins() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocoderDomain)
};

} // namespace OpenTune
