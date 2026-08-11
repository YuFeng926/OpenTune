#include "F0KeyDetector.h"
#include "../Utils/PitchUtils.h"
#include <juce_core/juce_core.h>
#include <cmath>

namespace OpenTune {

// Krumhansl-Schmuckler (1990) — C major / C minor reference profiles
const std::array<float, 12> F0KeyDetector::kKSMajorProfile = {
    6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f
};
const std::array<float, 12> F0KeyDetector::kKSMinorProfile = {
    6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f
};

DetectedKey F0KeyDetector::detect(const std::vector<float>& f0Frequencies,
                                  const std::vector<float>& energies) const
{
    std::array<float, 12> histogram{};
    histogram.fill(0.0f);

    // F0 轨迹 → 能量加权 pitch class 直方图（三角核插值）
    for (size_t i = 0; i < f0Frequencies.size(); ++i) {
        const float f0 = f0Frequencies[i];
        if (!std::isfinite(f0) || f0 <= 0.0f)
            continue;

        const float midiNote = PitchUtils::freqToMidi(f0);
        float pitchClass = std::fmod(midiNote, 12.0f);
        if (pitchClass < 0.0f)
            pitchClass += 12.0f;

        const int lowerBin = static_cast<int>(pitchClass) % 12;
        const int upperBin = (lowerBin + 1) % 12;
        const float ratio = pitchClass - std::floor(pitchClass);

        const float weight = (!energies.empty() && i < energies.size())
            ? std::sqrt(juce::jlimit(0.0f, 1.0f, energies[i]))
            : 1.0f;

        histogram[static_cast<size_t>(lowerBin)] += (1.0f - ratio) * weight;
        histogram[static_cast<size_t>(upperBin)] += ratio * weight;
    }

    bool allZero = true;
    for (float v : histogram) {
        if (v > 0.0f) { allZero = false; break; }
    }
    if (allZero)
        return DetectedKey{};

    // 24 候选（12 root × Major/Minor）余弦相似度评分，记录 best 与 second best
    DetectedKey bestResult;
    float bestScore = -2.0f;
    float secondBestScore = -2.0f;

    for (int root = 0; root < 12; ++root) {
        std::array<float, 12> rotatedMajor;
        std::array<float, 12> rotatedMinor;
        rotateProfile(kKSMajorProfile, root, rotatedMajor);
        rotateProfile(kKSMinorProfile, root, rotatedMinor);

        const float majorScore = cosineSimilarity(histogram, rotatedMajor);
        const float minorScore = cosineSimilarity(histogram, rotatedMinor);

        const auto update = [&](float score, Scale scale) {
            if (score > bestScore) {
                secondBestScore = bestScore;
                bestScore = score;
                bestResult.root = static_cast<Key>(root);
                bestResult.scale = scale;
            } else if (score > secondBestScore) {
                secondBestScore = score;
            }
        };
        update(majorScore, Scale::Major);
        update(minorScore, Scale::Minor);
    }

    bestResult.confidence = juce::jlimit(0.0f, 1.0f, bestScore - secondBestScore);
    bestResult.origin = Origin::Automatic;
    return bestResult;
}

float F0KeyDetector::cosineSimilarity(const std::array<float, 12>& a,
                                      const std::array<float, 12>& b)
{
    float dot = 0.0f;
    float normA = 0.0f;
    float normB = 0.0f;
    for (int i = 0; i < 12; ++i) {
        dot += a[static_cast<size_t>(i)] * b[static_cast<size_t>(i)];
        normA += a[static_cast<size_t>(i)] * a[static_cast<size_t>(i)];
        normB += b[static_cast<size_t>(i)] * b[static_cast<size_t>(i)];
    }

    const float denom = std::sqrt(normA * normB);
    if (denom < 1.0e-10f) return 0.0f;
    return dot / denom;
}

void F0KeyDetector::rotateProfile(const std::array<float, 12>& base,
                                  int semitones,
                                  std::array<float, 12>& result)
{
    for (int i = 0; i < 12; ++i) {
        result[static_cast<size_t>(i)] = base[static_cast<size_t>((i - semitones + 12) % 12)];
    }
}

} // namespace OpenTune
