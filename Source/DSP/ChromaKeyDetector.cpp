#include "ChromaKeyDetector.h"
#include "../Utils/AppLogger.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace OpenTune {

// ============================================================================
// DetectedKey static methods
// ============================================================================

juce::String DetectedKey::keyToString(Key key) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int idx = static_cast<int>(key);
    if (idx >= 0 && idx < 12)
        return names[idx];
    return "C";
}

juce::String DetectedKey::scaleToString(Scale scale) {
    static const char* names[] = {
        "Major", "Minor", "Chromatic", "Harmonic Minor",
        "Dorian", "Mixolydian", "Pentatonic Major", "Pentatonic Minor"
    };
    const int idx = static_cast<int>(scale);
    if (idx >= 0 && idx < static_cast<int>(sizeof(names) / sizeof(names[0])))
        return names[idx];
    return "Major";
}

// ============================================================================
// Key Profiles
// ============================================================================

// Krumhansl-Schmuckler (1990) — C major / C minor reference profiles
const std::array<float, 12> ChromaKeyDetector::kKSMajorProfile = {
    6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f
};
const std::array<float, 12> ChromaKeyDetector::kKSMinorProfile = {
    6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f
};

// Temperley (2001, "Music and Probability") — C major / C minor reference profiles
const std::array<float, 12> ChromaKeyDetector::kTemperleyMajorProfile = {
    5.0f, 2.0f, 3.5f, 2.0f, 4.5f, 4.0f, 2.0f, 4.5f, 2.0f, 3.5f, 1.5f, 4.0f
};
const std::array<float, 12> ChromaKeyDetector::kTemperleyMinorProfile = {
    5.0f, 2.0f, 3.5f, 4.5f, 2.0f, 4.0f, 2.0f, 4.5f, 3.5f, 2.0f, 1.5f, 4.0f
};

// ============================================================================
// Construction / Destruction
// ============================================================================

ChromaKeyDetector::ChromaKeyDetector() {
    fft_ = std::make_unique<juce::dsp::FFT>(kFftOrder);
    window_ = std::make_unique<juce::dsp::WindowingFunction<float>>(
        static_cast<size_t>(kFftSize),
        juce::dsp::WindowingFunction<float>::hann);
    fftBuffer_.resize(static_cast<size_t>(kFftSize * 2), 0.0f);
    chroma_.fill(0.0f);
}

ChromaKeyDetector::~ChromaKeyDetector() {
}

// ============================================================================
// Public API
// ============================================================================

DetectedKey ChromaKeyDetector::detect(const float* audio, int numSamples, int sampleRate) {
    if (audio == nullptr || numSamples <= 0 || sampleRate <= 0) {
        return DetectedKey{};
    }

    chroma_.fill(0.0f);
    computeChroma(audio, numSamples, sampleRate);
    return matchBestKey();
}

// ============================================================================
// Chroma Extraction
// ============================================================================

void ChromaKeyDetector::computeChroma(const float* audio, int numSamples, int sampleRate) {
    const double durationSec = static_cast<double>(numSamples) / static_cast<double>(sampleRate);

    // 对超过 5 分钟的音频，截取首尾各 30 秒分析
    if (durationSec > kMaxAnalysisDurationSec) {
        const int truncateSamples = static_cast<int>(kTruncateAnalysisSec * sampleRate);
        if (truncateSamples * 2 < numSamples) {
            // 分析前 30 秒
            computeChroma(audio, truncateSamples, sampleRate);
            // 分析后 30 秒（累加到同一个 chroma 向量）
            computeChroma(audio + (numSamples - truncateSamples), truncateSamples, sampleRate);
            return;
        }
    }

    const float freqResolution = static_cast<float>(sampleRate) / static_cast<float>(kFftSize);

    // 预计算频率 bin → pitch class 映射
    const int numBins = kFftSize / 2 + 1;
    const int minBin = static_cast<int>(std::ceil(kMinFreqHz / freqResolution));
    const int maxBin = std::min(numBins - 1, static_cast<int>(std::floor(kMaxFreqHz / freqResolution)));

    // STFT 帧循环
    for (int frameStart = 0; frameStart + kFftSize <= numSamples; frameStart += kHopSize) {
        // 复制音频帧到 FFT buffer
        std::copy(audio + frameStart, audio + frameStart + kFftSize, fftBuffer_.data());

        // 零填充后半部分（FFT buffer 是 2*kFftSize）
        std::fill(fftBuffer_.begin() + kFftSize, fftBuffer_.end(), 0.0f);

        // 应用 Hann 窗
        window_->multiplyWithWindowingTable(fftBuffer_.data(), static_cast<size_t>(kFftSize));

        // 执行 FFT（仅频率域结果）
        fft_->performFrequencyOnlyForwardTransform(fftBuffer_.data());

        // 将幅度折叠到 12 个 pitch class
        for (int bin = minBin; bin <= maxBin; ++bin) {
            const float magnitude = fftBuffer_[static_cast<size_t>(bin)];
            if (magnitude <= 0.0f) continue;

            const float freq = static_cast<float>(bin) * freqResolution;
            if (freq < kMinFreqHz || freq > kMaxFreqHz) continue;

            // 频率 → pitch class (C=0, C#=1, ..., B=11)
            // 使用 A440 作为参考：A = 9
            const float midiNote = 12.0f * std::log2(freq / 440.0f) + 69.0f;
            const float pitchClass = std::fmod(midiNote, 12.0f);
            const float normalizedPc = pitchClass < 0.0f ? pitchClass + 12.0f : pitchClass;

            // 线性插值到相邻两个 pitch class bin（三角核方法）
            const int lowerBin = static_cast<int>(normalizedPc) % 12;
            const int upperBin = (lowerBin + 1) % 12;
            const float ratio = normalizedPc - std::floor(normalizedPc);

            // 使用能量（幅度的平方）加权
            const float energy = magnitude * magnitude;
            chroma_[static_cast<size_t>(lowerBin)] += (1.0f - ratio) * energy;
            chroma_[static_cast<size_t>(upperBin)] += ratio * energy;
        }
    }
}

// ============================================================================
// Key Profile Matching
// ============================================================================

DetectedKey ChromaKeyDetector::matchBestKey() const {
    // 检查 chroma 是否全零（无有效音频内容）
    float chromaSum = 0.0f;
    for (float v : chroma_) chromaSum += v;
    if (chromaSum <= 0.0f) {
        return DetectedKey{};
    }

    DetectedKey bestResult;
    float bestScore = -2.0f;

    // 对 12 key × 2 scale (Major + Minor) = 24 种组合评分
    // 使用 K-S 和 Temperley 双 profile 集成（取平均 Pearson r）
    for (int root = 0; root < 12; ++root) {
        // Major
        {
            std::array<float, 12> ksRotated;
            std::array<float, 12> tempRotated;
            rotateProfile(kKSMajorProfile, root, ksRotated);
            rotateProfile(kTemperleyMajorProfile, root, tempRotated);

            const float ksScore = pearsonCorrelation(chroma_, ksRotated);
            const float tempScore = pearsonCorrelation(chroma_, tempRotated);
            const float ensembleScore = (ksScore + tempScore) * 0.5f;

            if (ensembleScore > bestScore) {
                bestScore = ensembleScore;
                bestResult.root = static_cast<Key>(root);
                bestResult.scale = Scale::Major;
            }
        }

        // Minor (Natural)
        {
            std::array<float, 12> ksRotated;
            std::array<float, 12> tempRotated;
            rotateProfile(kKSMinorProfile, root, ksRotated);
            rotateProfile(kTemperleyMinorProfile, root, tempRotated);

            const float ksScore = pearsonCorrelation(chroma_, ksRotated);
            const float tempScore = pearsonCorrelation(chroma_, tempRotated);
            const float ensembleScore = (ksScore + tempScore) * 0.5f;

            if (ensembleScore > bestScore) {
                bestScore = ensembleScore;
                bestResult.root = static_cast<Key>(root);
                bestResult.scale = Scale::Minor;
            }
        }
    }

    // Pearson r 范围是 [-1, 1]，归一化到 [0, 1] 作为 confidence
    bestResult.confidence = juce::jlimit(0.0f, 1.0f, (bestScore + 1.0f) * 0.5f);

    return bestResult;
}

float ChromaKeyDetector::pearsonCorrelation(const std::array<float, 12>& a,
                                             const std::array<float, 12>& b) {
    // 计算均值
    float meanA = 0.0f;
    float meanB = 0.0f;
    for (int i = 0; i < 12; ++i) {
        meanA += a[static_cast<size_t>(i)];
        meanB += b[static_cast<size_t>(i)];
    }
    meanA /= 12.0f;
    meanB /= 12.0f;

    // 计算 Pearson r = Σ(a-ā)(b-b̄) / sqrt(Σ(a-ā)² · Σ(b-b̄)²)
    float num = 0.0f;
    float denomA = 0.0f;
    float denomB = 0.0f;
    for (int i = 0; i < 12; ++i) {
        const float da = a[static_cast<size_t>(i)] - meanA;
        const float db = b[static_cast<size_t>(i)] - meanB;
        num += da * db;
        denomA += da * da;
        denomB += db * db;
    }

    const float denom = std::sqrt(denomA * denomB);
    if (denom < 1.0e-10f) return 0.0f;

    return num / denom;
}

void ChromaKeyDetector::rotateProfile(const std::array<float, 12>& base,
                                       int semitones,
                                       std::array<float, 12>& result) {
    for (int i = 0; i < 12; ++i) {
        result[static_cast<size_t>(i)] = base[static_cast<size_t>((i - semitones + 12) % 12)];
    }
}

} // namespace OpenTune
