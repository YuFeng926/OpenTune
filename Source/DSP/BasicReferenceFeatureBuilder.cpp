#include "BasicReferenceFeatureBuilder.h"

#include "../Utils/MaterializationState.h"
#include "../Utils/TimeCoordinate.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

constexpr double kMinAnchorSpacingSeconds = 0.150;
constexpr double kMinNoteDurationSeconds = 0.100;
constexpr float kPitchTransitionCents = 80.0f;

bool isVoiced(float f0) noexcept
{
    return std::isfinite(f0) && f0 > 0.0f;
}

bool appendAnchor(std::vector<MaterializationStore::DerivedAnalysis::TimeAnchor>& anchors,
                  double sourceSeconds,
                  float strength,
                  uint64_t& nextId)
{
    if (sourceSeconds <= 0.0) {
        return false;
    }
    if (!anchors.empty()
        && sourceSeconds - anchors.back().sourceSeconds < kMinAnchorSpacingSeconds) {
        return false;
    }

    MaterializationStore::DerivedAnalysis::TimeAnchor anchor;
    anchor.id = nextId++;
    anchor.sourceSeconds = sourceSeconds;
    anchor.strength = strength;
    anchors.push_back(anchor);
    return true;
}

void buildAnchorsFromF0(const std::vector<float>& f0,
                        double secondsPerFrame,
                        std::vector<MaterializationStore::DerivedAnalysis::TimeAnchor>& anchors)
{
    uint64_t nextId = 1;
    bool wasVoiced = false;
    float previousVoiced = 0.0f;

    for (size_t i = 0; i < f0.size(); ++i) {
        const float value = f0[i];
        const bool voiced = isVoiced(value);
        if (!voiced) {
            wasVoiced = false;
            continue;
        }

        const double sourceSeconds = static_cast<double>(i) * secondsPerFrame;
        if (!wasVoiced) {
            appendAnchor(anchors, sourceSeconds, 1.0f, nextId);
        } else if (previousVoiced > 0.0f) {
            const float cents = std::abs(1200.0f * std::log2(value / previousVoiced));
            if (cents >= 80.0f) {
                appendAnchor(anchors, sourceSeconds, 0.85f, nextId);
            }
        }

        wasVoiced = true;
        previousVoiced = value;
    }
}

float representativePitch(std::vector<float>& pitches)
{
    if (pitches.empty()) {
        return 0.0f;
    }

    std::sort(pitches.begin(), pitches.end());
    return pitches[pitches.size() / 2];
}

void commitNote(std::vector<Note>& notes,
                Note& current,
                std::vector<float>& pitches,
                double endSeconds)
{
    current.endTime = endSeconds;
    if (current.getDuration() >= kMinNoteDurationSeconds) {
        const float pitch = representativePitch(pitches);
        if (pitch > 0.0f) {
            current.pitch = pitch;
            current.originalPitch = pitch;
            current.selected = false;
            current.dirty = false;
            notes.push_back(current);
        }
    }

    current = Note{};
    pitches.clear();
}

std::vector<Note> buildNotesFromF0(const std::vector<float>& f0, double secondsPerFrame)
{
    std::vector<Note> notes;
    std::vector<float> pitches;
    pitches.reserve(256);

    Note current;
    bool inNote = false;
    float previousVoiced = 0.0f;

    for (size_t i = 0; i < f0.size(); ++i) {
        const float value = f0[i];
        const bool voiced = isVoiced(value);
        const double sourceSeconds = static_cast<double>(i) * secondsPerFrame;

        if (!voiced) {
            if (inNote) {
                commitNote(notes, current, pitches, sourceSeconds);
                inNote = false;
            }
            previousVoiced = 0.0f;
            continue;
        }

        if (!inNote) {
            current = Note{};
            current.startTime = sourceSeconds;
            current.isVoiced = true;
            pitches.clear();
            inNote = true;
        } else if (previousVoiced > 0.0f) {
            const float cents = std::abs(1200.0f * std::log2(value / previousVoiced));
            if (cents >= kPitchTransitionCents) {
                commitNote(notes, current, pitches, sourceSeconds);
                current = Note{};
                current.startTime = sourceSeconds;
                current.isVoiced = true;
                inNote = true;
            }
        }

        pitches.push_back(value);
        previousVoiced = value;
    }

    if (inNote) {
        commitNote(notes, current, pitches, static_cast<double>(f0.size()) * secondsPerFrame);
    }

    return notes;
}

} // namespace

MaterializationStore::DerivedAnalysis BasicReferenceFeatureBuilder::build(
    const MaterializationStore::MaterializationSnapshot& snapshot)
{
    MaterializationStore::DerivedAnalysis result;
    result.inputFingerprint = static_cast<int64_t>(snapshot.renderRevision);
    result.backendMode = 0;

    if (snapshot.audioBuffer == nullptr || snapshot.audioBuffer->getNumSamples() <= 0) {
        result.state = F0ExtractionState::Failed;
        result.errorMessage = "AUTO Ref analysis requires materialization audio";
        return result;
    }

    if (snapshot.pitchCurve == nullptr) {
        result.state = F0ExtractionState::Failed;
        result.errorMessage = "AUTO Ref analysis requires original F0";
        return result;
    }

    const auto pitchSnapshot = snapshot.pitchCurve->getSnapshot();
    if (pitchSnapshot == nullptr
        || pitchSnapshot->getOriginalF0().empty()
        || pitchSnapshot->getHopSize() <= 0
        || pitchSnapshot->getSampleRate() <= 0.0) {
        result.state = F0ExtractionState::Failed;
        result.errorMessage = "AUTO Ref analysis requires original F0";
        return result;
    }

    result.sourceDurationSeconds = TimeCoordinate::samplesToSeconds(
        snapshot.audioBuffer->getNumSamples(),
        TimeCoordinate::kRenderSampleRate);

    const auto& originalF0 = pitchSnapshot->getOriginalF0();
    const double secondsPerFrame =
        static_cast<double>(pitchSnapshot->getHopSize()) / pitchSnapshot->getSampleRate();
    result.basicDerivedNotes = buildNotesFromF0(originalF0, secondsPerFrame);
    buildAnchorsFromF0(originalF0, secondsPerFrame, result.basicDerivedAnchors);

    std::sort(result.basicDerivedAnchors.begin(), result.basicDerivedAnchors.end(),
              [](const auto& a, const auto& b) {
                  return a.sourceSeconds < b.sourceSeconds;
              });
    result.basicDerivedAnchors.erase(
        std::unique(result.basicDerivedAnchors.begin(), result.basicDerivedAnchors.end(),
                    [](const auto& a, const auto& b) {
                        return std::abs(a.sourceSeconds - b.sourceSeconds) < 1.0e-9;
                    }),
        result.basicDerivedAnchors.end());

    if (result.basicDerivedNotes.empty() && result.basicDerivedAnchors.size() < 2) {
        result.state = F0ExtractionState::Failed;
        result.errorMessage = "AUTO Ref analysis found no source-derived notes or temporal anchors";
        return result;
    }

    result.state = F0ExtractionState::Ready;
    result.analysisRevision = 1;
    return result;
}

} // namespace OpenTune
