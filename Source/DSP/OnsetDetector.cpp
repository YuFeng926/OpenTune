#include "OnsetDetector.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace OpenTune {

namespace {

// Wrap an angle into the principal range (-π, π].
inline float principalArg(float angle) noexcept
{
    constexpr float twoPi = 2.0f * juce::MathConstants<float>::pi;
    angle = std::fmod(angle + juce::MathConstants<float>::pi, twoPi);
    if (angle < 0.0f) angle += twoPi;
    return angle - juce::MathConstants<float>::pi;
}

// Compute median + α·1.4826·MAD over a vector (robust adaptive threshold).
// Uses a copy to allow nth_element selection without disturbing input.
float medianValue(std::vector<float>& scratch)
{
    if (scratch.empty()) return 0.0f;
    const size_t mid = scratch.size() / 2;
    std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.end());
    return scratch[mid];
}

} // namespace

OnsetDetector::OnsetDetector() = default;
OnsetDetector::~OnsetDetector() = default;

void OnsetDetector::configure(const OnsetDetectorConfig& cfg)
{
    config_ = cfg;
    if (cfg.nFft != currentNFft_) {
        // juce::dsp::FFT order is the log2 of the FFT size (e.g., 2048 → order 11).
        const int order = static_cast<int>(std::round(std::log2(static_cast<double>(cfg.nFft))));
        fft_ = std::make_unique<juce::dsp::FFT>(order);
        currentNFft_ = cfg.nFft;

        // Periodic Hann window (size N, drop the last sample to make it
        // periodic — matches numpy's `np.hanning(N+1)[:-1]` and librosa's
        // default STFT window).
        hannWindow_.resize(static_cast<size_t>(cfg.nFft));
        for (int i = 0; i < cfg.nFft; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(cfg.nFft);
            hannWindow_[static_cast<size_t>(i)] =
                0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * t));
        }
    }
}

void OnsetDetector::computeStft(const float* audio, size_t numSamples,
                                std::vector<std::vector<float>>& outMag,
                                std::vector<std::vector<float>>& outPhase)
{
    const int n_fft = config_.nFft;
    const int hop   = config_.hopLength;
    const int n_bins = n_fft / 2 + 1;

    if (static_cast<int>(numSamples) < n_fft) {
        outMag.clear();
        outPhase.clear();
        return;
    }

    const int n_frames = 1 + (static_cast<int>(numSamples) - n_fft) / hop;
    outMag.assign(static_cast<size_t>(n_frames), std::vector<float>(static_cast<size_t>(n_bins), 0.0f));
    outPhase.assign(static_cast<size_t>(n_frames), std::vector<float>(static_cast<size_t>(n_bins), 0.0f));

    // juce::dsp::FFT::performRealOnlyForwardTransform expects buffer of size 2*n_fft;
    // input goes in first n_fft, output is interleaved [Re, Im, Re, Im, ...].
    std::vector<float> fftBuffer(static_cast<size_t>(2 * n_fft), 0.0f);

    for (int t = 0; t < n_frames; ++t) {
        const int s = t * hop;
        std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
        for (int i = 0; i < n_fft; ++i) {
            fftBuffer[static_cast<size_t>(i)] = audio[s + i] * hannWindow_[static_cast<size_t>(i)];
        }
        fft_->performRealOnlyForwardTransform(fftBuffer.data());

        // After forward transform, frequency-domain data is interleaved in fftBuffer
        // for the first n_bins complex pairs.
        for (int k = 0; k < n_bins; ++k) {
            const float re = fftBuffer[static_cast<size_t>(2 * k)];
            const float im = fftBuffer[static_cast<size_t>(2 * k + 1)];
            outMag[static_cast<size_t>(t)][static_cast<size_t>(k)] = std::sqrt(re * re + im * im);
            outPhase[static_cast<size_t>(t)][static_cast<size_t>(k)] = std::atan2(im, re);
        }
    }
}

void OnsetDetector::computeOdf(const std::vector<std::vector<float>>& mag,
                               const std::vector<std::vector<float>>& phase,
                               std::vector<float>& outOdf)
{
    const int n_frames = static_cast<int>(mag.size());
    outOdf.assign(static_cast<size_t>(n_frames), 0.0f);
    if (n_frames < 3) return;

    const int n_bins = static_cast<int>(mag[0].size());

    // Frames 0 and 1 ODF stays at 0 (boundary initialization)
    for (int t = 2; t < n_frames; ++t) {
        float gamma = 0.0f;
        for (int k = 0; k < n_bins; ++k) {
            const float predicted_phase = principalArg(2.0f * phase[static_cast<size_t>(t - 1)][static_cast<size_t>(k)]
                                                         - phase[static_cast<size_t>(t - 2)][static_cast<size_t>(k)]);
            const float prev_mag = mag[static_cast<size_t>(t - 1)][static_cast<size_t>(k)];

            // Predicted complex value: prev_mag * exp(j * predicted_phase)
            const float pred_re = prev_mag * std::cos(predicted_phase);
            const float pred_im = prev_mag * std::sin(predicted_phase);

            // Current complex value: mag[t][k] * exp(j * phase[t][k])
            const float curr_mag   = mag[static_cast<size_t>(t)][static_cast<size_t>(k)];
            const float curr_phase = phase[static_cast<size_t>(t)][static_cast<size_t>(k)];
            const float curr_re = curr_mag * std::cos(curr_phase);
            const float curr_im = curr_mag * std::sin(curr_phase);

            const float diff_re = curr_re - pred_re;
            const float diff_im = curr_im - pred_im;
            gamma += std::sqrt(diff_re * diff_re + diff_im * diff_im);
        }
        outOdf[static_cast<size_t>(t)] = gamma;
    }
}

void OnsetDetector::applyAdaptiveThreshold(const std::vector<float>& odf,
                                           std::vector<float>& outOdfNorm)
{
    const int n_frames = static_cast<int>(odf.size());
    outOdfNorm.assign(static_cast<size_t>(n_frames), 0.0f);
    if (n_frames == 0) return;

    const int half = config_.adaptiveWindowFrames / 2;
    std::vector<float> windowScratch;
    std::vector<float> deviationScratch;

    for (int t = 0; t < n_frames; ++t) {
        const int lo = std::max(0, t - half);
        const int hi = std::min(n_frames, t + half + 1);
        const int wn = hi - lo;
        if (wn <= 0) continue;

        windowScratch.assign(odf.begin() + lo, odf.begin() + hi);
        const float med = medianValue(windowScratch);

        deviationScratch.resize(static_cast<size_t>(wn));
        for (int i = 0; i < wn; ++i) {
            deviationScratch[static_cast<size_t>(i)] = std::abs(odf[static_cast<size_t>(lo + i)] - med);
        }
        const float mad = medianValue(deviationScratch);

        const float threshold = med + config_.alpha * 1.4826f * mad;
        const float val = odf[static_cast<size_t>(t)] - threshold;
        outOdfNorm[static_cast<size_t>(t)] = (val > 0.0f) ? val : 0.0f;
    }
}

void OnsetDetector::pickPeaks(const std::vector<float>& odfNorm,
                              std::vector<int>& outOnsetStftFrames)
{
    outOnsetStftFrames.clear();
    const int n_frames = static_cast<int>(odfNorm.size());
    if (n_frames < 3) return;

    int lastOnset = -config_.minDistanceFrames;
    for (int t = 1; t < n_frames - 1; ++t) {
        if (odfNorm[static_cast<size_t>(t)] <= 0.0f) continue;
        const bool isLocalMax = odfNorm[static_cast<size_t>(t)] >= odfNorm[static_cast<size_t>(t - 1)]
                              && odfNorm[static_cast<size_t>(t)] >  odfNorm[static_cast<size_t>(t + 1)];
        if (!isLocalMax) continue;
        if (t - lastOnset < config_.minDistanceFrames) continue;
        outOnsetStftFrames.push_back(t);
        lastOnset = t;
    }
}

OnsetDetectionResult OnsetDetector::detect(const float* audio, size_t numSamples)
{
    OnsetDetectionResult result;

    if (audio == nullptr || numSamples == 0 || fft_ == nullptr) {
        return result;
    }

    std::vector<std::vector<float>> mag, phase;
    computeStft(audio, numSamples, mag, phase);

    if (mag.empty()) {
        return result;
    }

    result.stftHopLength = config_.hopLength;
    result.stftFrameCount = static_cast<int>(mag.size());

    computeOdf(mag, phase, result.odfRaw);
    applyAdaptiveThreshold(result.odfRaw, result.odfNormalized);

    std::vector<int> onsetStftFrames;
    pickPeaks(result.odfNormalized, onsetStftFrames);

    // Map STFT frame index → 100 fps frame index:
    //   stftSampleStart = stftFrame * hopLength
    //   timeSeconds = stftSampleStart / sampleRate
    //   frame100fps = round(timeSeconds * 100)
    result.onsetFrames100fps.reserve(onsetStftFrames.size());
    for (int sf : onsetStftFrames) {
        const double timeSec = static_cast<double>(sf) * static_cast<double>(config_.hopLength)
                             / static_cast<double>(config_.sampleRate);
        const int frame100 = static_cast<int>(std::round(timeSec * 100.0));
        result.onsetFrames100fps.push_back(frame100);
    }

    return result;
}

} // namespace OpenTune
