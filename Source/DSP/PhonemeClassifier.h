/**
 * PhonemeClassifier — DSP-based three-class (Voiced / Sibilant / Silence) on 100 fps grid.
 *
 * Two-layer architecture (per spec):
 *   Layer 1 — V/U fusion (prevents UV semantic inversion bug, see §11.1):
 *     voiced_p_from_rmvpe = 1.0 - rmvpe_uv_prob
 *     v_score = 0.6 * voiced_p_from_rmvpe + 0.4 * silero_vad_prob
 *     is_voiced = v_score > 0.55
 *
 *   Layer 2 — Sibilant vs Silence within unvoiced:
 *     RMS < -50 dBFS                            → Silence
 *     HFR > 0.55 ∧ SC > 3 kHz ∧ ZCR > 2.5 k/s   → Sibilant
 *     else                                       → Silence (low-energy noise / breath)
 *
 *   Morphological closing: any run shorter than 50 ms (5 frames @ 100 fps)
 *   is replaced by the previous run's class (suppresses RMVPE jitter on
 *   vibrato peaks).
 *
 * v7 unification rule: PhonemeClass output is used for handle seeding (WordSegmenter)
 * and UI tinting only — NOT for differential stretch routing. All classes
 * stretch uniformly via Rubber Band.
 *
 * Spec: openspec/changes/vocal-time-stretch/specs/phoneme-classifier/spec.md
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace OpenTune {

enum class PhonemeClass : int8_t {
    Voiced   = 0,
    Sibilant = 1,
    Silence  = 2
};

struct PhonemeClassifierConfig {
    int sampleRate = 44100;
    int frameRateHz = 100;          // canonical grid alignment

    // Layer 1 (V/U fusion) thresholds:
    float vuThreshold        = 0.55f;
    float rmvpeWeight        = 0.6f;
    float vadWeight          = 0.4f;

    // Layer 2 (Sibilant detection) thresholds:
    float rmsSilenceDbFS     = -50.0f;
    float hfrThreshold       = 0.55f;
    float scThresholdHz      = 3000.0f;
    float zcrThresholdPerSec = 2500.0f;

    // HFR band:
    float hfrBandLoHz        = 4000.0f;
    float hfrBandHiHz        = 10000.0f;

    // Morphology:
    float morphologyClosingMs = 50.0f;
};

/**
 * Diagnostic features computed per frame (10 ms hop @ 100 fps, 30 ms window).
 * Useful for tuning thresholds + UI debugging.
 */
struct PhonemeClassifierFeatures {
    std::vector<float> rms;     // raw RMS (linear)
    std::vector<float> rmsDbFS; // RMS in dBFS
    std::vector<float> zcr;     // zero-crossing rate per second
    std::vector<float> sc;      // spectral centroid in Hz
    std::vector<float> hfr;     // HF energy ratio (4-10kHz / 0-10kHz)
};

/**
 * Classification result, aligned to 100 fps grid.
 *
 * `classes[t]` is one of PhonemeClass values.  Length is min of
 * (input audio length at 100 fps, rmvpeUvProb length, vadProb length).
 */
struct PhonemeClassificationResult {
    std::vector<PhonemeClass> classes;
    PhonemeClassifierFeatures features;  // pre-classification diagnostics
    std::vector<float> vScore;           // V/U fusion score per frame
};

class PhonemeClassifier {
public:
    PhonemeClassifier();
    ~PhonemeClassifier();

    void configure(const PhonemeClassifierConfig& cfg);
    const PhonemeClassifierConfig& getConfig() const noexcept { return config_; }

    /**
     * Classify each 100 fps frame as Voiced / Sibilant / Silence.
     *
     * @param audio          Mono PCM at sampleRate
     * @param numSamples     PCM sample count
     * @param rmvpeUvProb    RMVPE second tensor output ("uv" = unvoiced probability)
     *                       at 100 fps. Must be provided (not nullptr).
     * @param vadProb100fps  Silero VAD probability upsampled to 100 fps.
     *                       May be nullptr/empty (V/U fusion degenerates to
     *                       0.6 * voiced_p_from_rmvpe; threshold halved accordingly).
     */
    PhonemeClassificationResult classify(const float* audio, size_t numSamples,
                                         const float* rmvpeUvProb, size_t rmvpeUvProbLen,
                                         const float* vadProb100fps, size_t vadProbLen);

private:
    void computeFeatures(const float* audio, size_t numSamples,
                         PhonemeClassifierFeatures& outFeatures);
    void applyMorphologyClosing(std::vector<PhonemeClass>& classes,
                                float minDurationMs);

    PhonemeClassifierConfig config_;
    std::unique_ptr<juce::dsp::FFT> fft_;
    std::vector<float> hannWindow_;
    int currentFftSize_ = 0;
    int currentWindowSize_ = 0;
};

} // namespace OpenTune
