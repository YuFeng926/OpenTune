#pragma once

#include <onnxruntime_cxx_api.h>
#include <memory>
#include <vector>
#include <string>
#include <functional>
#include "GameTypes.h"

namespace OpenTune {

/**
 * GameExtractor - 5-session ONNX pipeline for GAME MIDI extraction.
 *
 * Manages encoder, segmenter, estimator, dur2bd, bd2dur sessions.
 * NOT thread-safe — caller must serialize access (via InferenceGate).
 *
 * Lifecycle: created by GameInferenceService, sessions loaded lazily or eagerly.
 */
class GameExtractor {
public:
    struct ChunkResult {
        std::vector<ReferenceNote> notes;
    };

    GameExtractor(Ort::Env& env);
    ~GameExtractor();

    /**
     * Load all 5 ONNX sessions from model directory.
     * @param modelDir Directory containing encoder.onnx, segmenter.onnx, estimator.onnx, dur2bd.onnx, bd2dur.onnx
     * @return true if all sessions loaded successfully
     */
    bool loadSessions(const std::string& modelDir);

    /**
     * Release all ONNX sessions (free ~376MB).
     */
    void releaseSessions();

    /**
     * Check if all sessions are loaded.
     */
    bool isLoaded() const;

    /**
     * Extract reference notes from a single audio chunk.
     * @param audio Audio samples at 44100Hz
     * @param numSamples Number of samples
     * @param knownDurations VUV boundary durations (sum = audio duration)
     * @param config GAME inference parameters
     * @param progressCallback Optional progress [0,1]
     * @return Extracted reference notes for this chunk
     */
    ChunkResult extractChunk(
        const float* audio,
        size_t numSamples,
        const std::vector<float>& knownDurations,
        const GameConfig& config,
        std::function<void(float)> progressCallback = nullptr);

private:
    Ort::Env& env_;
    std::unique_ptr<Ort::Session> encoderSession_;
    std::unique_ptr<Ort::Session> segmenterSession_;
    std::unique_ptr<Ort::Session> estimatorSession_;
    std::unique_ptr<Ort::Session> dur2bdSession_;
    std::unique_ptr<Ort::Session> bd2durSession_;
    std::unique_ptr<Ort::MemoryInfo> memoryInfo_;

    static constexpr int kSampleRate = 44100;
    static constexpr float kTimestep = 0.01f;  // 10ms per frame

    Ort::SessionOptions createSessionOptions();

    // Pipeline steps
    struct EncoderOutput {
        std::vector<float> x_seg;   // [T, C]
        std::vector<float> x_est;   // [T, C]
        std::vector<bool> maskT;    // [T]
        int64_t T;
        int64_t C;
    };

    EncoderOutput runEncoder(const float* audio, size_t numSamples);
    std::vector<bool> runDur2Bd(const std::vector<float>& durations, const std::vector<bool>& maskT, int64_t T);
    std::vector<bool> runSegmenter(
        const std::vector<float>& x_seg, int64_t T, int64_t C,
        const std::vector<bool>& knownBoundaries,
        const std::vector<bool>& maskT,
        const GameConfig& config);
    struct Bd2DurOutput {
        std::vector<float> durations;   // [N]
        std::vector<bool> maskN;        // [N]
        int64_t N;
    };
    Bd2DurOutput runBd2Dur(const std::vector<bool>& boundaries, const std::vector<bool>& maskT, int64_t T);
    struct EstimatorOutput {
        std::vector<bool> presence;     // [N]
        std::vector<float> scores;      // [N] MIDI pitch
        int64_t N;
    };
    EstimatorOutput runEstimator(
        const std::vector<float>& x_est, int64_t T, int64_t C,
        const std::vector<bool>& boundaries,
        const std::vector<bool>& maskT,
        const std::vector<bool>& maskN,
        int64_t N,
        float presenceThreshold);
};

} // namespace OpenTune
