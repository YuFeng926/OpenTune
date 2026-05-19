/**
 * SileroVadExtractor — Silero VAD v5 ONNX implementation of IVadExtractor.
 *
 * Spec: openspec/changes/vocal-time-stretch/specs/silero-vad-extractor/spec.md
 *
 * Architecture:
 *   - Loads `silero_vad.onnx` via ONNX Runtime (CPU).  Model file is expected
 *     under `models/silero_vad.onnx` (Git LFS).
 *   - Stateful: maintains LSTM hidden state `h` + `c` (2x [2, batch=1, 64])
 *     across chunks within a single extractVad() call.
 *   - Resamples input to 16 kHz internally (r8brain).
 *   - Output frame rate: 31.25 fps (hop = 512 samples @ 16 kHz).
 *
 * Phase H scaffolding (vocal-time-stretch §4.1):
 *   This file ships a HEADER-ONLY scaffold + the core ONNX glue stub.
 *   Until the Silero model is committed (§2.7/2.8) and the `extractVad`
 *   tensor pipeline is wired, `initialize()` returns false and the
 *   service-layer code degrades cleanly via factory null check.
 */
#pragma once

#include "IVadExtractor.h"

#include <atomic>
#include <memory>
#include <string>

namespace Ort { class Session; class Env; }

namespace OpenTune {

class SileroVadExtractor : public IVadExtractor {
public:
    SileroVadExtractor();
    ~SileroVadExtractor() override;

    /**
     * Load the Silero ONNX session.  Returns false if the model file is
     * missing, malformed, or ONNX Runtime fails to initialize.  Callers
     * should treat false as "VAD unavailable; degrade to F0-only V/U fusion".
     *
     * @param modelPath Absolute path to silero_vad.onnx.  Typically resolved
     *                   via `ModelPathResolver` (bundle / installed / user override).
     */
    bool initialize(const std::string& modelPath);

    /**
     * Whether `initialize()` succeeded.  False means extractVad will return
     * empty.
     */
    bool isReady() const noexcept { return ready_.load(std::memory_order_acquire); }

    // IVadExtractor overrides
    std::vector<float> extractVad(
        const float* audio,
        size_t length,
        int sampleRate,
        std::function<void(float)> progressCallback = nullptr) override;

    int getHopSize() const override { return 512; }                  // 32 ms @ 16 kHz
    int getTargetSampleRate() const override { return 16000; }
    VadModelType getModelType() const override { return VadModelType::SileroV5; }
    std::string  getName() const override { return "Silero VAD v5"; }
    size_t       getModelSize() const override { return 1800000; }  // ~1.8 MB

private:
    std::unique_ptr<Ort::Session> session_;
    std::shared_ptr<Ort::Env> env_;
    std::atomic<bool> ready_{false};
};

} // namespace OpenTune
