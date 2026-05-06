#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include "GameTypes.h"
#include "../Utils/Error.h"

namespace Ort { struct Env; }

namespace OpenTune {

/**
 * GameInferenceService - GAME MIDI extraction service
 *
 * Responsibilities:
 * - Manage GameExtractor lifecycle (load on demand, release after 30s idle)
 * - Handle audio resampling to 44100Hz
 * - Fixed-window chunking (60s + 5s overlap) for long audio
 * - Merge chunk results
 *
 * Thread-safe: Yes (mutex for model access)
 * Audio thread: NEVER call from audio thread
 */
class GameInferenceService {
public:
    GameInferenceService(std::shared_ptr<Ort::Env> env);
    ~GameInferenceService();

    /**
     * Initialize with model directory containing GAME ONNX files.
     * Does NOT load sessions immediately (lazy load on first extract).
     */
    bool initialize(const std::string& modelDir);

    /**
     * Shutdown and release all resources.
     */
    void shutdown();

    /**
     * Extract reference notes from audio.
     * @param audio Audio samples (any sample rate)
     * @param length Number of samples
     * @param sampleRate Source sample rate
     * @param knownDurations VUV boundary durations (sum = audio duration in seconds)
     * @param config GAME parameters
     * @param progressCallback Optional progress [0,1]
     * @return Reference notes or error
     */
    Result<std::vector<ReferenceNote>> extractReferenceNotes(
        const float* audio,
        size_t length,
        int sampleRate,
        const std::vector<float>& knownDurations,
        const GameConfig& config,
        std::function<void(float)> progressCallback = nullptr);

    bool isInitialized() const;

    /**
     * Release idle model after 30s. Called periodically by processor.
     */
    void releaseIdleModelIfNeeded();

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GameInferenceService)
};

} // namespace OpenTune
