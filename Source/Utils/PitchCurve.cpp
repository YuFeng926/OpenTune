#include "PitchCurve.h"
#include "PitchUtils.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace OpenTune {

namespace {

void insertSegmentSorted(std::vector<PitchCorrectionSegment>& segments, PitchCorrectionSegment&& seg)
{
    auto insertPos = std::lower_bound(segments.begin(), segments.end(), seg.startFrame,
        [](const PitchCorrectionSegment& s, int frame) {
            return s.startFrame < frame;
        });
    segments.insert(insertPos, std::move(seg));
}

void clearSegmentsInRangePreserveOutside(std::vector<PitchCorrectionSegment>& segments, int startFrame, int endFrame)
{
    if (startFrame >= endFrame) {
        return;
    }

    std::vector<PitchCorrectionSegment> kept;
    kept.reserve(segments.size() + 1);

    for (const auto& seg : segments) {
        if (seg.endFrame <= startFrame || seg.startFrame >= endFrame) {
            kept.push_back(seg);
            continue;
        }

        if (seg.startFrame < startFrame) {
            PitchCorrectionSegment left = seg;
            left.endFrame = startFrame;
            const int leftLen = left.endFrame - left.startFrame;
            if (leftLen > 0 && leftLen <= static_cast<int>(seg.f0Data.size())) {
                left.f0Data.assign(seg.f0Data.begin(), seg.f0Data.begin() + leftLen);
                kept.push_back(std::move(left));
            }
        }

        if (seg.endFrame > endFrame) {
            PitchCorrectionSegment right = seg;
            right.startFrame = endFrame;
            const int offset = right.startFrame - seg.startFrame;
            const int rightLen = right.endFrame - right.startFrame;
            if (offset >= 0 && rightLen > 0 && offset + rightLen <= static_cast<int>(seg.f0Data.size())) {
                right.f0Data.assign(seg.f0Data.begin() + offset, seg.f0Data.begin() + offset + rightLen);
                kept.push_back(std::move(right));
            }
        }
    }

    segments.swap(kept);
}

float smootherstep(float t) noexcept
{
    t = juce::jlimit(0.0f, 1.0f, t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

double noteTransitionFrameAt(const Note& leftNote,
                             const Note& rightNote,
                             double secondsPerFrame) noexcept
{
    return 0.5 * (leftNote.endTime + rightNote.startTime) / secondsPerFrame;
}

float noteBoundaryShiftSemitoneOffset(const std::vector<Note>& notes,
                                      const std::vector<size_t>& relevantNoteIndices,
                                      const std::vector<float>& noteAnchorMidis,
                                      size_t activeNoteIndex,
                                      float activeOffsetSemitones,
                                       int frame,
                                       double secondsPerFrame,
                                       float frameRetuneSpeed)
{
    const int maxBridgeFrames = PitchCurve::getCorrectedF0BoundaryContextFrames();
    const int bridgeFrames = static_cast<int>(std::lround(
        static_cast<float>(maxBridgeFrames) * (1.0f - juce::jlimit(0.0f, 1.0f, frameRetuneSpeed))));
    if (bridgeFrames <= 0) {
        return activeOffsetSemitones;
    }

    const auto activeIt = std::find(relevantNoteIndices.begin(), relevantNoteIndices.end(), activeNoteIndex);
    if (activeIt == relevantNoteIndices.end()) {
        return activeOffsetSemitones;
    }

    const auto transitionOffsetFor = [&](size_t otherNoteIndex,
                                         double boundaryFrame,
                                         bool activeIsRight) -> std::optional<float> {
        const float otherAnchorMidi = noteAnchorMidis[otherNoteIndex];
        const float activeAnchorMidi = noteAnchorMidis[activeNoteIndex];
        if (otherAnchorMidi <= 0.0f || activeAnchorMidi <= 0.0f) {
            return std::nullopt;
        }

        const double distance = std::abs((static_cast<double>(frame) + 0.5) - boundaryFrame);
        if (distance > static_cast<double>(bridgeFrames)) {
            return std::nullopt;
        }

        const float otherOffset = PitchUtils::freqToMidi(notes[otherNoteIndex].getAdjustedPitch()) - otherAnchorMidi;
        const float t = static_cast<float>((static_cast<double>(frame) + 0.5 - (boundaryFrame - bridgeFrames))
                                           / static_cast<double>(bridgeFrames * 2));
        const float w = smootherstep(t);
        return activeIsRight
            ? otherOffset + (activeOffsetSemitones - otherOffset) * w
            : activeOffsetSemitones + (otherOffset - activeOffsetSemitones) * w;
    };

    const auto position = static_cast<size_t>(std::distance(relevantNoteIndices.begin(), activeIt));
    if (position > 0) {
        const size_t leftNoteIndex = relevantNoteIndices[position - 1];
        const auto bridged = transitionOffsetFor(leftNoteIndex,
                                                 noteTransitionFrameAt(notes[leftNoteIndex], notes[activeNoteIndex], secondsPerFrame),
                                                 true);
        if (bridged.has_value()) {
            return *bridged;
        }
    }

    if (position + 1 < relevantNoteIndices.size()) {
        const size_t rightNoteIndex = relevantNoteIndices[position + 1];
        const auto bridged = transitionOffsetFor(rightNoteIndex,
                                                 noteTransitionFrameAt(notes[activeNoteIndex], notes[rightNoteIndex], secondsPerFrame),
                                                 false);
        if (bridged.has_value()) {
            return *bridged;
        }
    }

    return activeOffsetSemitones;
}

} // namespace

bool PitchCurveSnapshot::hasCorrectionInRange(int startFrame, int endFrame) const {
    if (correctionSegments_.empty()) {
        return false;
    }

    auto it = std::lower_bound(correctionSegments_.begin(), correctionSegments_.end(), startFrame,
        [](const PitchCorrectionSegment& seg, int frame) {
            return seg.endFrame <= frame;
        });

    while (it != correctionSegments_.end() && it->startFrame < endFrame) {
        if (it->endFrame > startFrame) {
            return true;
        }
        ++it;
    }

    return false;
}

F0FrameRange PitchCurve::expandNoteBasedCorrectionRange(int startFrame, int endFrameExclusive, int frameCount) noexcept
{
    if (frameCount <= 0 || endFrameExclusive <= startFrame) {
        return {};
    }

    const int rangeStart = juce::jlimit(0, frameCount, startFrame);
    const int rangeEnd = juce::jlimit(0, frameCount, endFrameExclusive);
    if (rangeEnd <= rangeStart) {
        return {};
    }

    return {
        std::max(0, rangeStart - getCorrectedF0BoundaryContextFrames()),
        std::min(frameCount, rangeEnd + getCorrectedF0BoundaryContextFrames())
    };
}



void PitchCurve::applyCorrectionToRange(
    const std::vector<Note>& notes,
    int startFrame,
    int endFrame,
    float sourcePitchRatio,
    float retuneSpeed,
    float vibratoDepth,
    float vibratoRate,
    float pitchDriftScale)
{
    auto oldSnapshot = getSnapshot();
    const auto& originalF0 = oldSnapshot->getOriginalF0();
    
    if (originalF0.empty() || startFrame >= endFrame) {
        return;
    }

    const int maxFrame = static_cast<int>(originalF0.size());
    if (startFrame >= maxFrame) return;
    if (endFrame > maxFrame) endFrame = maxFrame;
    if (startFrame < 0) startFrame = 0;

    const int hopSize = oldSnapshot->getHopSize();
    const double sampleRate = oldSnapshot->getSampleRate();
    if (hopSize <= 0 || sampleRate <= 0.0) {
        return;
    }

    const auto calculationRange = expandNoteBasedCorrectionRange(startFrame, endFrame, maxFrame);
    if (calculationRange.isEmpty()) {
        return;
    }
    const int calculationStartFrame = calculationRange.startFrame;
    const int calculationEndFrame = calculationRange.endFrameExclusive;

    auto correctionSegments = oldSnapshot->getCorrectionSegments();
    clearSegmentsInRangePreserveOutside(correctionSegments, calculationStartFrame, calculationEndFrame);

    struct NoteCorrectionInfo {
        float anchorPitch = 0.0f;
        float anchorMidi = 0.0f;
        float rotationRad = 0.0f;
        double timeCenterSeconds = 0.0;
    };

    std::vector<NoteCorrectionInfo> noteInfos(notes.size());
    std::vector<float> noteAnchorMidis(notes.size(), 0.0f);

    const float radToDeg = 180.0f / juce::MathConstants<float>::pi;
    const float slopeAngleMinDeg = 10.0f;
    const float slopeAngleMaxDeg = 30.0f;
    const float slopeAt45DegSemitonesPerSecond = 7.0f;

    const F0Timeline f0tl(hopSize, sampleRate, maxFrame);
    const double secondsPerFrame = static_cast<double>(hopSize) / sampleRate;
    std::vector<size_t> relevantNoteIndices;
    for (size_t noteIndex = 0; noteIndex < notes.size(); ++noteIndex) {
        const auto& note = notes[noteIndex];

        size_t noteStartFrame = static_cast<size_t>(f0tl.frameAtOrBefore(note.startTime));
        size_t noteEndFrame = static_cast<size_t>(f0tl.exclusiveFrameAt(note.endTime));

        if (static_cast<int>(noteEndFrame) <= calculationStartFrame
            || static_cast<int>(noteStartFrame) >= calculationEndFrame) {
            continue;
        }

        relevantNoteIndices.push_back(noteIndex);

        NoteCorrectionInfo info;
        float anchorPitch = note.originalPitch;
        if (anchorPitch <= 0.0f) anchorPitch = note.pitch;
        info.anchorPitch = anchorPitch;
        info.anchorMidi = PitchUtils::freqToMidi(anchorPitch);
        info.timeCenterSeconds = (note.startTime + note.endTime) * 0.5;

        if (info.anchorMidi > 0.0f && noteStartFrame < noteEndFrame) {
            std::vector<float> voicedTimes;
            std::vector<float> voicedMidis;
            for (size_t f = noteStartFrame; f < noteEndFrame && f < originalF0.size(); ++f) {
                const float rawF0 = originalF0[f];
                const float f0 = rawF0 > 0.0f ? rawF0 * sourcePitchRatio : rawF0;
                if (f0 <= 0.0f) continue;
                const double tSec = f0tl.timeAtFrame(f);
                voicedTimes.push_back(static_cast<float>(tSec));
                voicedMidis.push_back(PitchUtils::freqToMidi(f0));
            }

            if (voicedTimes.size() >= 6) {
                size_t n = voicedTimes.size();
                size_t segCount = std::max<size_t>(3, n / 5);

                std::vector<float> earlyMidis(voicedMidis.begin(), voicedMidis.begin() + segCount);
                std::vector<float> lateMidis(voicedMidis.end() - segCount, voicedMidis.end());
                std::sort(earlyMidis.begin(), earlyMidis.end());
                std::sort(lateMidis.begin(), lateMidis.end());

                float earlyMidi = earlyMidis[earlyMidis.size() / 2];
                float lateMidi = lateMidis[lateMidis.size() / 2];

                float earlyTime = voicedTimes[segCount / 2];
                float lateTime = voicedTimes[n - segCount + (segCount / 2)];

                float deltaTime = lateTime - earlyTime;
                if (deltaTime > 0.0001f) {
                    float slope = (lateMidi - earlyMidi) / deltaTime;
                    float signedAngleRad = std::atan(slope / slopeAt45DegSemitonesPerSecond);
                    float absAngleDeg = std::abs(signedAngleRad * radToDeg);

                    if (absAngleDeg >= slopeAngleMinDeg && absAngleDeg <= slopeAngleMaxDeg) {
                        info.rotationRad = -signedAngleRad;
                    }
                }
            }
        }

        noteInfos[noteIndex] = info;
        noteAnchorMidis[noteIndex] = info.anchorMidi;
    }

    if (relevantNoteIndices.empty()) {
        return;
    }
    std::sort(relevantNoteIndices.begin(), relevantNoteIndices.end(),
        [&notes](size_t left, size_t right) {
            return notes[left].startTime < notes[right].startTime;
        });

    std::vector<float> correctedF0Buffer(calculationEndFrame - calculationStartFrame, 0.0f);

    for (int i = calculationStartFrame; i < calculationEndFrame; ++i) {
        const float rawF0 = originalF0[i];
        float f0 = rawF0 > 0.0f ? rawF0 * sourcePitchRatio : rawF0;
        if (f0 <= 0.0f) {
            correctedF0Buffer[i - calculationStartFrame] = 0.0f;
            continue;
        }

        const double timeSeconds = f0tl.timeAtFrame(i);

        const Note* activeNote = nullptr;
        size_t activeNoteIndex = 0;
        for (size_t relevantPosition = 0; relevantPosition < relevantNoteIndices.size(); ++relevantPosition) {
            const size_t idx = relevantNoteIndices[relevantPosition];
            const auto& note = notes[idx];
            if (timeSeconds >= note.startTime && timeSeconds < note.endTime) {
                activeNote = &note;
                activeNoteIndex = idx;
                break;
            }
        }

        if (activeNote) {
            float frameRetuneSpeed = retuneSpeed;
            if (activeNote->retuneSpeed >= 0.0f) {
                frameRetuneSpeed = activeNote->retuneSpeed;
            }

            float targetBaseF0 = activeNote->getAdjustedPitch();
            float targetF0 = targetBaseF0;

            float noteVibratoDepth = vibratoDepth;
            float noteVibratoRate = vibratoRate;
            if (activeNote->vibratoDepth >= 0.0f) noteVibratoDepth = activeNote->vibratoDepth;
            if (activeNote->vibratoRate >= 0.0f) noteVibratoRate = activeNote->vibratoRate;
            if (noteVibratoDepth > 0.0f) {
                double timeInNote = timeSeconds - activeNote->startTime;
                float depthSemitones = (noteVibratoDepth / 100.0f) * 1.0f;
                float lfoValue = depthSemitones * std::sin(2.0f * juce::MathConstants<float>::pi * noteVibratoRate * (float)timeInNote);
                targetF0 *= std::pow(2.0f, lfoValue / 12.0f);
            }

            float baseF0 = f0;
            if (noteInfos[activeNoteIndex].rotationRad != 0.0f) {
                const double tSec = timeSeconds;
                float x = static_cast<float>(tSec - noteInfos[activeNoteIndex].timeCenterSeconds);
                float y = PitchUtils::freqToMidi(f0) - noteInfos[activeNoteIndex].anchorMidi;
                float c = std::cos(noteInfos[activeNoteIndex].rotationRad);
                float s = std::sin(noteInfos[activeNoteIndex].rotationRad);
                float yRot = x * s + y * c;
                baseF0 = PitchUtils::midiToFreq(noteInfos[activeNoteIndex].anchorMidi + yRot);
            }

            float shiftedF0 = baseF0;
            if (noteInfos[activeNoteIndex].anchorPitch > 0.0f && targetBaseF0 > 0.0f) {
                const float activeOffsetSemitones =
                    PitchUtils::freqToMidi(targetBaseF0) - noteInfos[activeNoteIndex].anchorMidi;
                const float dynamicOffsetSemitones = noteBoundaryShiftSemitoneOffset(notes,
                                                                                     relevantNoteIndices,
                                                                                     noteAnchorMidis,
                                                                                     activeNoteIndex,
                                                                                     activeOffsetSemitones,
                                                                                      i,
                                                                                      secondsPerFrame,
                                                                                      frameRetuneSpeed);
                float shiftRatio = std::pow(2.0f, dynamicOffsetSemitones / 12.0f);
                shiftedF0 = baseF0 * shiftRatio;
            }

            // --- Pitch Drift: scale the deviation from the target base (mirror axis) ---
            {
                float framePitchDriftScale = pitchDriftScale;
                if (activeNote->pitchDriftScale != 1.0f) {
                    framePitchDriftScale = activeNote->pitchDriftScale;
                }
                if (framePitchDriftScale != 1.0f && activeNote->startTime < activeNote->endTime) {
                    // Deviation of the current curve from the target base, in semitones
                    const float devSemitones = PitchUtils::freqToMidi(shiftedF0) - PitchUtils::freqToMidi(targetF0);
                    // Scale around the target base as the mirror axis:
                    // 1.0 = original, 0.0 = back to targetF0 (vibrato retained), -1.0 = full mirror
                    shiftedF0 = PitchUtils::midiToFreq(PitchUtils::freqToMidi(targetF0) + devSemitones * framePitchDriftScale);
                }
            }

            correctedF0Buffer[i - calculationStartFrame] = PitchUtils::mixRetune(shiftedF0, targetF0, frameRetuneSpeed);
        } else {
            correctedF0Buffer[i - calculationStartFrame] = f0;
        }
    }

    PitchCorrectionSegment newSeg(calculationStartFrame, calculationEndFrame, correctedF0Buffer, PitchCorrectionSegment::Source::NoteBased);
    newSeg.retuneSpeed = retuneSpeed;
    newSeg.vibratoDepth = vibratoDepth;
    newSeg.vibratoRate = vibratoRate;
    newSeg.pitchDriftScale = pitchDriftScale;

    insertSegmentSorted(correctionSegments, std::move(newSeg));

    uint64_t newGen = incrementGeneration();
    auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
        oldSnapshot->getOriginalF0(),
        oldSnapshot->getOriginalEnergy(),
        std::move(correctionSegments),
        hopSize,
        sampleRate,
        newGen
    );
    std::atomic_store(&snapshot_, newSnapshot);
}

void PitchCurve::setManualCorrectionRange(int startFrame, int endFrame, const std::vector<float>& f0Data,
                                          PitchCorrectionSegment::Source source) {
    if (startFrame >= endFrame || f0Data.empty()) {
        return;
    }

    auto oldSnapshot = getSnapshot();
    auto correctionSegments = oldSnapshot->getCorrectionSegments();
    
    PitchCorrectionSegment newSeg(startFrame, endFrame, f0Data, source);
    clearSegmentsInRangePreserveOutside(correctionSegments, startFrame, endFrame);
    insertSegmentSorted(correctionSegments, std::move(newSeg));

    uint64_t newGen = incrementGeneration();
    auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
        oldSnapshot->getOriginalF0(),
        oldSnapshot->getOriginalEnergy(),
        std::move(correctionSegments),
        oldSnapshot->getHopSize(),
        oldSnapshot->getSampleRate(),
        newGen
    );
    std::atomic_store(&snapshot_, newSnapshot);
}

void PitchCurve::clearCorrectionRange(int startFrame, int endFrame) {
    if (startFrame >= endFrame) {
        return;
    }

    auto oldSnapshot = getSnapshot();
    auto correctionSegments = oldSnapshot->getCorrectionSegments();
    clearSegmentsInRangePreserveOutside(correctionSegments, startFrame, endFrame);

    uint64_t newGen = incrementGeneration();
    auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
        oldSnapshot->getOriginalF0(),
        oldSnapshot->getOriginalEnergy(),
        std::move(correctionSegments),
        oldSnapshot->getHopSize(),
        oldSnapshot->getSampleRate(),
        newGen
    );
    std::atomic_store(&snapshot_, newSnapshot);
}

} // namespace OpenTune
