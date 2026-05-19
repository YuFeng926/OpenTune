/**
 * VadInferenceService — CPU-only VAD extraction service.
 *
 * Mirrors F0InferenceService template (lazy load + idle release).  Adds
 * Silero VAD lifecycle management around an `IVadExtractor` instance.
 *
 * Spec: openspec/changes/vocal-time-stretch/specs/silero-vad-extractor/spec.md
 *
 * Phase H scaffolding (vocal-time-stretch §4.3):
 *   This service is wired into ModelFactory + VadExtractionService but
 *   gracefully no-ops when the Silero model is missing (initialize returns
 *   false; extract calls return empty vectors).
 */
#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <functional>
#include "IVadExtractor.h"

namespace OpenTune {

class VadInferenceService {
public:
    VadInferenceService();
    ~VadInferenceService();

    VadInferenceService(const VadInferenceService&) = delete;
    VadInferenceService& operator=(const VadInferenceService&) = delete;

    /**
     * Lazy-initialize with model directory.  Returns true if Silero model
     * was found + loaded.  Returns false (with no log spam) when model is
     * absent — PhonemeClassifier degrades to RMVPE-only.
     */
    bool initialize(const std::string& modelDir);

    /**
     * Release the underlying ONNX session immediately.
     */
    void shutdown();

    /**
     * Free the underlying extractor if it has been idle longer than
     * `idleTimeoutSeconds_` (default 30s).  Called by the inference worker
     * loop on each idle wakeup.
     */
    void releaseIdleModelIfNeeded();

    /**
     * Whether the extractor is loaded and usable.
     */
    bool isReady() const noexcept;

    /**
     * Extract VAD from audio.  Returns empty vector on miss.
     */
    std::vector<float> extractVad(const float* audio,
                                   size_t length,
                                   int sampleRate,
                                   std::function<void(float)> progressCallback = nullptr);

private:
    bool ensureLoaded();

    mutable std::shared_mutex extractorMutex_;
    std::unique_ptr<IVadExtractor> extractor_;
    std::string modelDir_;
    std::chrono::steady_clock::time_point lastUsedTime_;
    std::chrono::seconds idleTimeoutSeconds_{30};
    bool initializeAttempted_ = false;
    bool initializeSucceeded_ = false;
};

} // namespace OpenTune
