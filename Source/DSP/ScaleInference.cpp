#include "ScaleInference.h"
#include <cmath>

namespace OpenTune {

const std::array<float, 12> ScaleInference::majorProfile_ = {
    6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f
};

const std::array<float, 12> ScaleInference::minorProfile_ = {
    6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f
};

juce::String DetectedKey::keyToString(Key key) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[static_cast<int>(key)];
}

juce::String DetectedKey::scaleToString(Scale scale) {
    return scale == Scale::Major ? "Major" : "Minor";
}

ScaleInference::ScaleInference() {
    histogram_.fill(0.0f);

    for (int root = 0; root < 12; ++root) {
        rotateTemplate(majorProfile_, root, templates_[root]);
        rotateTemplate(minorProfile_, root, templates_[root + 12]);
    }
}

ScaleInference::~ScaleInference() {
}

void ScaleInference::processF0Data(const std::vector<float>& f0Frequencies) {
    processF0Data(f0Frequencies, {}, {});
}

void ScaleInference::processF0Data(const std::vector<float>& f0Frequencies,
                                   const std::vector<float>& confidences,
                                   const std::vector<float>& energies) {
    histogram_.fill(0.0f);

    float maxEnergy = 0.0f;
    for (float e : energies) {
        if (std::isfinite(e) && e > maxEnergy) {
            maxEnergy = e;
        }
    }

    for (size_t i = 0; i < f0Frequencies.size(); ++i) {
        float frequency = f0Frequencies[i];
        if (frequency < 50.0f || frequency > 2000.0f) {
            continue;
        }

        float confidenceWeight = 1.0f;
        if (!confidences.empty() && i < confidences.size()) {
            confidenceWeight = juce::jlimit(0.0f, 1.0f, confidences[i]);
            if (!std::isfinite(confidenceWeight) || confidenceWeight <= 0.0f) {
                continue;
            }
        }

        float energyWeight = 1.0f;
        if (!energies.empty() && i < energies.size()) {
            const float energy = energies[i];
            if (!std::isfinite(energy) || energy <= 0.0f || maxEnergy <= 0.0f) {
                continue;
            }
            const float normalized = juce::jlimit(0.0f, 1.0f, energy / maxEnergy);
            if (normalized <= 0.0f) {
                continue;
            }
            // 使用 sqrt 压缩动态范围，避免个别超高能量帧垄断统计。
            energyWeight = std::sqrt(normalized);
        }

        const float weight = confidenceWeight * energyWeight;
        if (weight <= 0.0f) {
            continue;
        }

        float cents = frequencyToCents(frequency);
        float normalizedCents = std::fmod(cents + 1200.0f, 1200.0f);

        int lowerBin = static_cast<int>(normalizedCents / 100.0f) % 12;
        int upperBin = (lowerBin + 1) % 12;
        float ratio = std::fmod(normalizedCents, 100.0f) / 100.0f;

        histogram_[lowerBin] += (1.0f - ratio) * weight;
        histogram_[upperBin] += ratio * weight;
    }
}

void ScaleInference::reset() {
    histogram_.fill(0.0f);
    candidateHoldTime_ = 0.0f;
}

DetectedKey ScaleInference::findBestMatch() const {
    DetectedKey result;
    float maxScore = -1.0f;
    float totalScore = 0.0f;

    for (int i = 0; i < 24; ++i) {
        float score = computeScore(histogram_, templates_[i]);
        totalScore += score;

        if (score > maxScore) {
            maxScore = score;
            result.root = static_cast<Key>(i % 12);
            result.scale = (i < 12) ? Scale::Major : Scale::Minor;
        }
    }

    result.confidence = (totalScore > 0.0f) ? maxScore / totalScore : 0.0f;
    return result;
}

void ScaleInference::updateWithNewF0(float frequency) {
    if (frequency < 50.0f || frequency > 2000.0f) {
        return;
    }

    float cents = frequencyToCents(frequency);
    float normalizedCents = std::fmod(cents + 1200.0f, 1200.0f);

    int lowerBin = static_cast<int>(normalizedCents / 100.0f) % 12;
    int upperBin = (lowerBin + 1) % 12;
    float ratio = std::fmod(normalizedCents, 100.0f) / 100.0f;

    histogram_[lowerBin] += (1.0f - ratio);
    histogram_[upperBin] += ratio;
}

DetectedKey ScaleInference::getCurrentDetection() const {
    return confirmedKey_;
}

void ScaleInference::setVotingDuration(float seconds) {
    votingDuration_ = seconds;
}

void ScaleInference::update(float deltaTime) {
    DetectedKey currentBest = findBestMatch();

    if (currentBest.root == candidateKey_.root && currentBest.scale == candidateKey_.scale) {
        candidateHoldTime_ += deltaTime;
        if (candidateHoldTime_ >= votingDuration_) {
            confirmedKey_ = candidateKey_;
        }
    } else {
        candidateKey_ = currentBest;
        candidateHoldTime_ = 0.0f;
    }
}

float ScaleInference::frequencyToCents(float frequency) const {
    return 1200.0f * std::log2(frequency / 440.0f) + 5700.0f;
}

int ScaleInference::frequencyToBin(float frequency) const {
    float cents = frequencyToCents(frequency);
    float normalizedCents = std::fmod(cents + 1200.0f, 1200.0f);
    return static_cast<int>(normalizedCents / 100.0f) % 12;
}

void ScaleInference::rotateTemplate(
    const std::array<float, 12>& base,
    int semitones,
    std::array<float, 12>& result
) const {
    for (int i = 0; i < 12; ++i) {
        result[i] = base[(i - semitones + 12) % 12];
    }
}

float ScaleInference::computeScore(
    const std::array<float, 12>& histogram,
    const std::array<float, 12>& templ
) const {
    float score = 0.0f;
    for (int i = 0; i < 12; ++i) {
        score += histogram[i] * templ[i];
    }
    return score;
}

} // namespace OpenTune
