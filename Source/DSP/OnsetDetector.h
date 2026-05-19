/**
 * OnsetDetector — Bello 2005 complex-domain Onset Detection Function.
 *
 * Reference:
 *   Bello, Daudet, Abdallah, Duxbury, Davies, Sandler.
 *   "A Tutorial on Onset Detection in Music Signals."
 *   IEEE Trans. Speech Audio Processing 13(5), 2005.  §III.B.
 *
 * Algorithm summary:
 *   1. STFT (n_fft=2048, hop=441 ≈ 10 ms @ 44.1 kHz, periodic Hann window)
 *   2. Phase prediction: phase_pred[t,k] = principal(2·phase[t-1,k] - phase[t-2,k])
 *   3. Predicted spectrum: X_pred[t,k] = |X[t-1,k]| · exp(j·phase_pred[t,k])
 *   4. ODF: Γ[t] = Σ_k |X[t,k] - X_pred[t,k]|
 *   5. Adaptive threshold: median + α · 1.4826 · MAD over sliding window
 *   6. Local maxima above threshold → onset frames
 *
 * Output frame rate: 100 fps (mapped from STFT frames via hop/sr).
 *
 * Spec: openspec/changes/vocal-time-stretch/specs/vocal-onset-detector/spec.md
 * Design: research/p0_time_stretch/DESIGN.md v7 §3
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace OpenTune {

struct OnsetDetectorConfig {
    int sampleRate = 44100;
    int nFft = 2048;
    int hopLength = 441;            // ≈ 10 ms @ 44.1 kHz, aligns with 100 fps grid
    int adaptiveWindowFrames = 9;   // half=4 each side (full = 9)
    float alpha = 1.5f;             // robust z-score multiplier (median + α·1.4826·MAD)
    int minDistanceFrames = 3;      // ≈ 30 ms minimum spacing between onsets
};

/**
 * Onset detection result.
 *
 * - `odfRaw[t]` is the raw complex-domain ODF (length = numFrames at hop=441).
 * - `odfNormalized[t]` is the post-threshold ODF (max(0, odf - threshold)).
 * - `onsetFrames100fps[i]` is each onset's frame index in the 100 fps grid
 *   (frame index = round(stft_frame * hopLength * 100 / sampleRate)).
 */
struct OnsetDetectionResult {
    std::vector<float> odfRaw;
    std::vector<float> odfNormalized;
    std::vector<int>   onsetFrames100fps;
    int                stftHopLength = 0;     // for diagnostic
    int                stftFrameCount = 0;    // for diagnostic
};

class OnsetDetector {
public:
    OnsetDetector();
    ~OnsetDetector();

    void configure(const OnsetDetectorConfig& cfg);
    const OnsetDetectorConfig& getConfig() const noexcept { return config_; }

    /**
     * Detect onsets in mono audio.
     *
     * @param audio        Mono PCM, length = numSamples
     * @param numSamples   Sample count
     * @return             OnsetDetectionResult (always populated, even on small inputs)
     */
    OnsetDetectionResult detect(const float* audio, size_t numSamples);

private:
    // Compute STFT, populate magnitudes & phases.
    void computeStft(const float* audio, size_t numSamples,
                     std::vector<std::vector<float>>& outMag,
                     std::vector<std::vector<float>>& outPhase);

    // Compute complex-domain ODF from mag/phase.
    void computeOdf(const std::vector<std::vector<float>>& mag,
                    const std::vector<std::vector<float>>& phase,
                    std::vector<float>& outOdf);

    // Apply adaptive threshold (median + α·1.4826·MAD over sliding window).
    void applyAdaptiveThreshold(const std::vector<float>& odf,
                                std::vector<float>& outOdfNorm);

    // Pick local maxima respecting min-distance.
    void pickPeaks(const std::vector<float>& odfNorm,
                   std::vector<int>& outOnsetStftFrames);

    OnsetDetectorConfig config_;
    std::unique_ptr<juce::dsp::FFT> fft_;
    std::vector<float> hannWindow_;
    int currentNFft_ = 0;
};

} // namespace OpenTune
