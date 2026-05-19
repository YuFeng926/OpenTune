/**
 * IVadExtractor — abstract interface for Voice Activity Detection extractors.
 *
 * Used by PhonemeClassifier (V/U fusion Layer 1).  Designed to mirror the
 * IF0Extractor contract so VadExtractionService / VadInferenceService can be
 * cloned from F0ExtractionService / F0InferenceService with minimal churn.
 *
 * Spec: openspec/changes/vocal-time-stretch/specs/silero-vad-extractor/spec.md
 *
 * @brief Phase H scaffolding (vocal-time-stretch §4.2):
 *   This interface is added now so downstream consumers (PhonemeClassifier,
 *   factory, service) can be wired to a concrete Silero implementation
 *   later.  Until the Silero ONNX model is committed via Git LFS
 *   (§2.7/2.8), all factory paths return null and PhonemeClassifier
 *   degrades to RMVPE-only V/U fusion (vad_p = 0).
 */
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace OpenTune {

enum class VadModelType {
    SileroV5 = 0   // Silero VAD v5 — 16 kHz, ~1.8 MB ONNX
};

struct VadModelInfo {
    VadModelType type;
    std::string  name;
    std::string  displayName;
    size_t       modelSizeBytes;
    bool         isAvailable;
};

class IVadExtractor {
public:
    virtual ~IVadExtractor() = default;

    /**
     * Extract VAD probability curve from PCM audio.
     *
     * @param audio       Raw PCM block at sampleRate
     * @param length      Sample count
     * @param sampleRate  Input sample rate (extractor handles resample to 16 kHz internally)
     * @return            VAD probability ∈ [0, 1] per frame at extractor's
     *                    native frame rate (typically 31.25 fps @ 16 kHz, 512 hop).
     *                    Empty on error.  Caller resamples to 100 fps via
     *                    nearest-neighbor for PhonemeClassifier consumption.
     */
    virtual std::vector<float> extractVad(
        const float* audio,
        size_t length,
        int sampleRate,
        std::function<void(float)> progressCallback = nullptr) = 0;

    /**
     * Native hop size at target sample rate.  For Silero VAD this is 512
     * samples @ 16 kHz (= 32 ms / 31.25 fps).
     */
    virtual int getHopSize() const = 0;

    /**
     * Target sample rate (Silero VAD: 16000).
     */
    virtual int getTargetSampleRate() const = 0;

    virtual VadModelType getModelType() const = 0;
    virtual std::string  getName() const = 0;
    virtual size_t       getModelSize() const = 0;
};

} // namespace OpenTune
