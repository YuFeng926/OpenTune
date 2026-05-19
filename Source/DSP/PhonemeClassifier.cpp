#include "PhonemeClassifier.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

inline int nextPowerOfTwo(int x) noexcept
{
    int p = 1;
    while (p < x) p <<= 1;
    return p;
}

inline float linearToDbFS(float rms) noexcept
{
    return 20.0f * std::log10(std::max(rms, 1e-9f));
}

} // namespace

PhonemeClassifier::PhonemeClassifier() = default;
PhonemeClassifier::~PhonemeClassifier() = default;

void PhonemeClassifier::configure(const PhonemeClassifierConfig& cfg)
{
    config_ = cfg;

    // 30 ms window @ sampleRate = 3 * (sampleRate / frameRateHz)
    const int hop = cfg.sampleRate / std::max(1, cfg.frameRateHz);
    const int win = 3 * hop;
    const int fftSize = nextPowerOfTwo(win);

    if (fftSize != currentFftSize_) {
        const int order = static_cast<int>(std::round(std::log2(static_cast<double>(fftSize))));
        fft_ = std::make_unique<juce::dsp::FFT>(order);
        currentFftSize_ = fftSize;
    }

    if (win != currentWindowSize_) {
        hannWindow_.resize(static_cast<size_t>(win));
        for (int i = 0; i < win; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(win);
            hannWindow_[static_cast<size_t>(i)] =
                0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * t));
        }
        currentWindowSize_ = win;
    }
}

void PhonemeClassifier::computeFeatures(const float* audio, size_t numSamples,
                                         PhonemeClassifierFeatures& outFeatures)
{
    const int sr  = config_.sampleRate;
    const int hop = sr / std::max(1, config_.frameRateHz);
    const int win = 3 * hop;
    const int n_fft = currentFftSize_;
    const int n_bins = n_fft / 2 + 1;

    const int n_frames = static_cast<int>(std::ceil(static_cast<double>(numSamples) / hop));
    if (n_frames <= 0) {
        outFeatures = {};
        return;
    }

    outFeatures.rms.assign(static_cast<size_t>(n_frames), 0.0f);
    outFeatures.rmsDbFS.assign(static_cast<size_t>(n_frames), -200.0f);
    outFeatures.zcr.assign(static_cast<size_t>(n_frames), 0.0f);
    outFeatures.sc.assign(static_cast<size_t>(n_frames), 0.0f);
    outFeatures.hfr.assign(static_cast<size_t>(n_frames), 0.0f);

    // Pre-pad both ends with zeros so window-around-frame-0 fits.
    const int pad = win / 2;
    std::vector<float> padded(numSamples + 2 * pad, 0.0f);
    std::copy(audio, audio + numSamples, padded.begin() + pad);

    // FFT bin-frequency table
    std::vector<float> binFreq(static_cast<size_t>(n_bins));
    for (int k = 0; k < n_bins; ++k) {
        binFreq[static_cast<size_t>(k)] = static_cast<float>(k) * static_cast<float>(sr) / static_cast<float>(n_fft);
    }
    const int bandLoIdx = static_cast<int>(std::lower_bound(binFreq.begin(), binFreq.end(), config_.hfrBandLoHz) - binFreq.begin());
    const int bandHiIdx = static_cast<int>(std::lower_bound(binFreq.begin(), binFreq.end(), config_.hfrBandHiHz) - binFreq.begin());

    std::vector<float> fftBuffer(static_cast<size_t>(2 * n_fft), 0.0f);

    for (int t = 0; t < n_frames; ++t) {
        const int center = t * hop + pad;
        const int s = center - win / 2;
        const float* frame = padded.data() + s;

        // RMS
        double rmsSqSum = 0.0;
        for (int i = 0; i < win; ++i) {
            const float v = frame[i];
            rmsSqSum += static_cast<double>(v) * v;
        }
        const float rms = static_cast<float>(std::sqrt(rmsSqSum / win + 1e-12));
        outFeatures.rms[static_cast<size_t>(t)] = rms;
        outFeatures.rmsDbFS[static_cast<size_t>(t)] = linearToDbFS(rms);

        // Zero-crossing rate (per second)
        int crossings = 0;
        for (int i = 1; i < win; ++i) {
            const float prev = frame[i - 1];
            const float curr = frame[i];
            const bool prevPos = prev >= 0.0f;
            const bool currPos = curr >= 0.0f;
            if (prevPos != currPos) ++crossings;
        }
        outFeatures.zcr[static_cast<size_t>(t)] = static_cast<float>(crossings) / (static_cast<float>(win) / static_cast<float>(sr));

        // FFT-based features (SC, HFR)
        std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
        for (int i = 0; i < win; ++i) {
            fftBuffer[static_cast<size_t>(i)] = frame[i] * hannWindow_[static_cast<size_t>(i)];
        }
        fft_->performRealOnlyForwardTransform(fftBuffer.data());

        double specSum = 0.0;
        double specWeightedSum = 0.0;
        for (int k = 0; k < n_bins; ++k) {
            const float re = fftBuffer[static_cast<size_t>(2 * k)];
            const float im = fftBuffer[static_cast<size_t>(2 * k + 1)];
            const double mag = std::sqrt(static_cast<double>(re) * re + static_cast<double>(im) * im) + 1e-9;
            specSum += mag;
            specWeightedSum += static_cast<double>(binFreq[static_cast<size_t>(k)]) * mag;
        }
        outFeatures.sc[static_cast<size_t>(t)] = (specSum > 0.0) ? static_cast<float>(specWeightedSum / specSum) : 0.0f;

        // HFR = sum(magnitude over [hfrBandLo, hfrBandHi]) / sum(magnitude over [0, hfrBandHi])
        double hfEnergy = 0.0;
        double totalToHi = 0.0;
        for (int k = 0; k < bandHiIdx; ++k) {
            const float re = fftBuffer[static_cast<size_t>(2 * k)];
            const float im = fftBuffer[static_cast<size_t>(2 * k + 1)];
            const double mag = std::sqrt(static_cast<double>(re) * re + static_cast<double>(im) * im) + 1e-9;
            totalToHi += mag;
            if (k >= bandLoIdx) hfEnergy += mag;
        }
        outFeatures.hfr[static_cast<size_t>(t)] = (totalToHi > 0.0) ? static_cast<float>(hfEnergy / totalToHi) : 0.0f;
    }
}

void PhonemeClassifier::applyMorphologyClosing(std::vector<PhonemeClass>& classes, float minDurationMs)
{
    const int frameRateHz = config_.frameRateHz;
    const int minFrames = std::max(1, static_cast<int>(std::round(minDurationMs / 1000.0f * frameRateHz)));
    const int n = static_cast<int>(classes.size());
    if (n == 0) return;

    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && classes[static_cast<size_t>(j)] == classes[static_cast<size_t>(i)]) ++j;
        const int runLen = j - i;
        if (runLen < minFrames && i > 0 && j < n) {
            // Replace short interior run with previous run's class
            const PhonemeClass prevClass = classes[static_cast<size_t>(i - 1)];
            for (int k = i; k < j; ++k) {
                classes[static_cast<size_t>(k)] = prevClass;
            }
        }
        i = j;
    }
}

PhonemeClassificationResult PhonemeClassifier::classify(const float* audio, size_t numSamples,
                                                        const float* rmvpeUvProb, size_t rmvpeUvProbLen,
                                                        const float* vadProb100fps, size_t vadProbLen)
{
    PhonemeClassificationResult result;

    if (audio == nullptr || numSamples == 0 || rmvpeUvProb == nullptr) {
        return result;
    }

    computeFeatures(audio, numSamples, result.features);
    const int n_audio_frames = static_cast<int>(result.features.rms.size());

    // Align lengths to common minimum
    int n = n_audio_frames;
    if (n > static_cast<int>(rmvpeUvProbLen)) n = static_cast<int>(rmvpeUvProbLen);
    if (vadProb100fps != nullptr && static_cast<int>(vadProbLen) < n) n = static_cast<int>(vadProbLen);
    if (n <= 0) return result;

    result.classes.assign(static_cast<size_t>(n), PhonemeClass::Silence);
    result.vScore.assign(static_cast<size_t>(n), 0.0f);

    for (int t = 0; t < n; ++t) {
        // Layer 1: V/U fusion.
        // ⚠️ Critical: rmvpeUvProb is the *unvoiced* probability (voiced ≈ 0,
        // unvoiced ≈ 1).  Use (1 - uv) as voicing component.  See
        // RMVPEExtractor.h enableUvCheck_ comment + DESIGN.md §11.1.
        const float uv_p = std::clamp(rmvpeUvProb[t], 0.0f, 1.0f);
        const float voiced_p_from_rmvpe = 1.0f - uv_p;
        const float vad_p = (vadProb100fps != nullptr) ? std::clamp(vadProb100fps[t], 0.0f, 1.0f) : 0.0f;

        const float v_score = config_.rmvpeWeight * voiced_p_from_rmvpe
                            + config_.vadWeight   * vad_p;
        result.vScore[static_cast<size_t>(t)] = v_score;

        if (v_score > config_.vuThreshold) {
            result.classes[static_cast<size_t>(t)] = PhonemeClass::Voiced;
            continue;
        }

        // Layer 2: within unvoiced — distinguish Sibilant vs Silence
        const float rmsDbFS = result.features.rmsDbFS[static_cast<size_t>(t)];
        if (rmsDbFS < config_.rmsSilenceDbFS) {
            result.classes[static_cast<size_t>(t)] = PhonemeClass::Silence;
            continue;
        }

        const float sc = result.features.sc[static_cast<size_t>(t)];
        const float hfr = result.features.hfr[static_cast<size_t>(t)];
        const float zcr = result.features.zcr[static_cast<size_t>(t)];

        const bool isSibilant = (hfr > config_.hfrThreshold)
                              && (sc  > config_.scThresholdHz)
                              && (zcr > config_.zcrThresholdPerSec);
        result.classes[static_cast<size_t>(t)] = isSibilant ? PhonemeClass::Sibilant
                                                            : PhonemeClass::Silence;
    }

    applyMorphologyClosing(result.classes, config_.morphologyClosingMs);

    return result;
}

} // namespace OpenTune
