#include "PitchCurve.h"
#include "PitchUtils.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace OpenTune {

constexpr int kUnifiedTransitionFrames = 10;

namespace {

bool hasSegmentOverlap(const std::vector<CorrectedSegment>& segments, int rangeStart, int rangeEnd)
{
    if (rangeEnd <= rangeStart) {
        return false;
    }
    for (const auto& seg : segments) {
        if (seg.endFrame <= rangeStart || seg.startFrame >= rangeEnd) {
            continue;
        }
        return true;
    }
    return false;
}

void insertSegmentSorted(std::vector<CorrectedSegment>& segments, CorrectedSegment&& seg)
{
    auto insertPos = std::lower_bound(segments.begin(), segments.end(), seg.startFrame,
        [](const CorrectedSegment& s, int frame) {
            return s.startFrame < frame;
        });
    segments.insert(insertPos, std::move(seg));
}

void clearSegmentsInRangePreserveOutside(std::vector<CorrectedSegment>& segments, int startFrame, int endFrame)
{
    if (startFrame >= endFrame) {
        return;
    }

    std::vector<CorrectedSegment> kept;
    kept.reserve(segments.size() + 1);

    for (const auto& seg : segments) {
        if (seg.endFrame <= startFrame || seg.startFrame >= endFrame) {
            kept.push_back(seg);
            continue;
        }

        if (seg.startFrame < startFrame) {
            CorrectedSegment left = seg;
            left.endFrame = startFrame;
            const int leftLen = left.endFrame - left.startFrame;
            if (leftLen > 0 && leftLen <= static_cast<int>(seg.f0Data.size())) {
                left.f0Data.assign(seg.f0Data.begin(), seg.f0Data.begin() + leftLen);
                kept.push_back(std::move(left));
            }
        }

        if (seg.endFrame > endFrame) {
            CorrectedSegment right = seg;
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

std::optional<CorrectedSegment> buildLeftTransitionSegment(
    const CorrectedSegment& centerSeg,
    const std::vector<float>& originalF0,
    const std::vector<CorrectedSegment>& existingSegments,
    int transitionFrames)
{
    if (centerSeg.f0Data.empty() || transitionFrames <= 0 || originalF0.empty()) {
        return std::nullopt;
    }

    const float boundaryF0 = centerSeg.f0Data.front();
    if (boundaryF0 <= 0.0f || centerSeg.startFrame <= 0) {
        return std::nullopt;
    }

    const int transStart = std::max(0, centerSeg.startFrame - transitionFrames);
    const int transEnd = centerSeg.startFrame;
    if (transEnd <= transStart || transEnd > static_cast<int>(originalF0.size())) {
        return std::nullopt;
    }

    if (hasSegmentOverlap(existingSegments, transStart, transEnd)) {
        return std::nullopt;
    }

    for (int f = transStart; f < transEnd; ++f) {
        if (originalF0[static_cast<size_t>(f)] <= 0.0f) {
            return std::nullopt;
        }
    }

    std::vector<float> transitionData;
    transitionData.reserve(static_cast<size_t>(transEnd - transStart));
    const float safeBoundary = std::max(boundaryF0, 1.0e-6f);
    const int len = transEnd - transStart;

    for (int i = 0; i < len; ++i) {
        const int f = transStart + i;
        const float orig = std::max(originalF0[static_cast<size_t>(f)], 1.0e-6f);
        const float t = (len > 1) ? static_cast<float>(i) / static_cast<float>(len - 1) : 0.5f;
        const float w = t * t * (3.0f - 2.0f * t); // Hermite smoothstep: 0→1, dw/dt=0 at endpoints
        const float logOrig = std::log2(orig);
        const float logBoundary = std::log2(safeBoundary);
        transitionData.push_back(std::pow(2.0f, logOrig + (logBoundary - logOrig) * w));
    }

    CorrectedSegment seg(transStart, transEnd, transitionData, centerSeg.source);
    seg.retuneSpeed = centerSeg.retuneSpeed;
    seg.vibratoDepth = centerSeg.vibratoDepth;
    seg.vibratoRate = centerSeg.vibratoRate;
    return seg;
}

std::optional<CorrectedSegment> buildRightTransitionSegment(
    const CorrectedSegment& centerSeg,
    const std::vector<float>& originalF0,
    const std::vector<CorrectedSegment>& existingSegments,
    int transitionFrames)
{
    if (centerSeg.f0Data.empty() || transitionFrames <= 0 || originalF0.empty()) {
        return std::nullopt;
    }

    const float boundaryF0 = centerSeg.f0Data.back();
    if (boundaryF0 <= 0.0f || centerSeg.endFrame >= static_cast<int>(originalF0.size())) {
        return std::nullopt;
    }

    const int transStart = centerSeg.endFrame;
    const int transEnd = std::min(centerSeg.endFrame + transitionFrames, static_cast<int>(originalF0.size()));
    if (transEnd <= transStart) {
        return std::nullopt;
    }

    if (hasSegmentOverlap(existingSegments, transStart, transEnd)) {
        return std::nullopt;
    }

    for (int f = transStart; f < transEnd; ++f) {
        if (originalF0[static_cast<size_t>(f)] <= 0.0f) {
            return std::nullopt;
        }
    }

    std::vector<float> transitionData;
    transitionData.reserve(static_cast<size_t>(transEnd - transStart));
    const float safeBoundary = std::max(boundaryF0, 1.0e-6f);
    const int len = transEnd - transStart;

    for (int i = 0; i < len; ++i) {
        const int f = transStart + i;
        const float orig = std::max(originalF0[static_cast<size_t>(f)], 1.0e-6f);
        const float t = (len > 1) ? static_cast<float>(i) / static_cast<float>(len - 1) : 0.5f;
        const float w = 1.0f - t * t * (3.0f - 2.0f * t); // Inverse smoothstep: 1→0, dw/dt=0 at endpoints
        const float logOrig = std::log2(orig);
        const float logBoundary = std::log2(safeBoundary);
        transitionData.push_back(std::pow(2.0f, logOrig + (logBoundary - logOrig) * w));
    }

    CorrectedSegment seg(transStart, transEnd, transitionData, centerSeg.source);
    seg.retuneSpeed = centerSeg.retuneSpeed;
    seg.vibratoDepth = centerSeg.vibratoDepth;
    seg.vibratoRate = centerSeg.vibratoRate;
    return seg;
}

void insertSegmentWithUnifiedTransitions(
    std::vector<CorrectedSegment>& segments,
    const std::vector<float>& originalF0,
    CorrectedSegment&& centerSeg,
    int transitionFrames)
{
    std::optional<CorrectedSegment> leftTransition = buildLeftTransitionSegment(centerSeg, originalF0, segments, transitionFrames);
    std::optional<CorrectedSegment> rightTransition = buildRightTransitionSegment(centerSeg, originalF0, segments, transitionFrames);

    if (leftTransition.has_value()) {
        insertSegmentSorted(segments, std::move(leftTransition.value()));
    }
    insertSegmentSorted(segments, std::move(centerSeg));
    if (rightTransition.has_value()) {
        insertSegmentSorted(segments, std::move(rightTransition.value()));
    }
}

} // namespace

bool PitchCurveSnapshot::hasCorrectionInRange(int startFrame, int endFrame) const {
    if (correctedSegments_.empty()) {
        return false;
    }

    auto it = std::lower_bound(correctedSegments_.begin(), correctedSegments_.end(), startFrame,
        [](const CorrectedSegment& seg, int frame) {
            return seg.endFrame <= frame;
        });

    while (it != correctedSegments_.end() && it->startFrame < endFrame) {
        if (it->endFrame > startFrame) {
            return true;
        }
        ++it;
    }

    return false;
}

void PitchCurveSnapshot::renderF0Range(int startFrame, int endFrame,
                                       std::function<void(int, const float*, int)> callback) const {
    if (startFrame >= endFrame || startFrame < 0) {
        return;
    }

    const int maxFrame = static_cast<int>(originalF0_.size());
    if (endFrame > maxFrame) {
        endFrame = maxFrame;
    }
    if (startFrame >= maxFrame) {
        return;
    }

    auto it = std::lower_bound(correctedSegments_.begin(), correctedSegments_.end(), startFrame,
        [](const CorrectedSegment& seg, int frame) {
            return seg.endFrame <= frame;
        });

    int currentPos = startFrame;
    std::vector<float> tempBuffer;

    while (currentPos < endFrame) {
        if (it != correctedSegments_.end() && it->startFrame < endFrame) {
            if (currentPos < it->startFrame) {
                int gapEnd = std::min(it->startFrame, endFrame);
                int gapLength = gapEnd - currentPos;
                callback(currentPos, originalF0_.data() + currentPos, gapLength);
                currentPos = gapEnd;
            }

            if (currentPos < it->endFrame && currentPos < maxFrame) {
                int segStart = std::max(currentPos, it->startFrame);
                int segEnd = std::min(endFrame, std::min(it->endFrame, maxFrame));
                int offset = segStart - it->startFrame;
                int length = segEnd - segStart;

                if (length <= 0) {
                    ++it;
                    continue;
                }

                if (static_cast<size_t>(offset + length) > it->f0Data.size()) {
                    ++it;
                    continue;
                }

                if (it->retuneSpeed >= 0.0f && it->source == CorrectedSegment::Source::LineAnchor) {
                    tempBuffer.resize(static_cast<size_t>(length));
                    for (int i = 0; i < length; ++i) {
                        int frameIdx = segStart + i;
                        float targetF0 = it->f0Data[static_cast<size_t>(offset + i)];
                        float originalF0 = originalF0_[static_cast<size_t>(frameIdx)];
                        if (originalF0 > 0.0f && targetF0 > 0.0f) {
                            tempBuffer[static_cast<size_t>(i)] = PitchUtils::mixRetune(originalF0, targetF0, it->retuneSpeed);
                        } else {
                            tempBuffer[static_cast<size_t>(i)] = targetF0;
                        }
                    }
                    callback(segStart, tempBuffer.data(), length);
                } else {
                    callback(segStart, it->f0Data.data() + offset, length);
                }
                currentPos = segEnd;
            }

            ++it;
        } else {
            int length = endFrame - currentPos;
            callback(currentPos, originalF0_.data() + currentPos, length);
            currentPos = endFrame;
        }
    }
}

void PitchCurveSnapshot::renderCorrectedOnlyRange(int startFrame, int endFrame,
                                                  std::function<void(int, const float*, int)> callback) const {
    if (startFrame >= endFrame || startFrame < 0) {
        return;
    }

    const int maxFrame = static_cast<int>(originalF0_.size());
    if (endFrame > maxFrame) {
        endFrame = maxFrame;
    }
    if (startFrame >= maxFrame) {
        return;
    }

    auto it = std::lower_bound(correctedSegments_.begin(), correctedSegments_.end(), startFrame,
        [](const CorrectedSegment& seg, int frame) {
            return seg.endFrame <= frame;
        });

    int currentPos = startFrame;
    std::vector<float> tempBuffer;

    while (currentPos < endFrame) {
        if (it != correctedSegments_.end() && it->startFrame < endFrame) {
            if (currentPos < it->startFrame) {
                const int gapEnd = std::min(it->startFrame, endFrame);
                const int gapLength = gapEnd - currentPos;
                tempBuffer.assign(static_cast<size_t>(gapLength), 0.0f);
                callback(currentPos, tempBuffer.data(), gapLength);
                currentPos = gapEnd;
            }

            if (currentPos < it->endFrame && currentPos < maxFrame) {
                const int segStart = std::max(currentPos, it->startFrame);
                const int segEnd = std::min(endFrame, std::min(it->endFrame, maxFrame));
                const int offset = segStart - it->startFrame;
                const int length = segEnd - segStart;

                if (length <= 0) {
                    ++it;
                    continue;
                }

                if (static_cast<size_t>(offset + length) > it->f0Data.size()) {
                    ++it;
                    continue;
                }

                if (it->retuneSpeed >= 0.0f && it->source == CorrectedSegment::Source::LineAnchor) {
                    tempBuffer.resize(static_cast<size_t>(length));
                    for (int i = 0; i < length; ++i) {
                        int frameIdx = segStart + i;
                        float targetF0 = it->f0Data[static_cast<size_t>(offset + i)];
                        float originalF0 = originalF0_[static_cast<size_t>(frameIdx)];
                        if (originalF0 > 0.0f && targetF0 > 0.0f) {
                            tempBuffer[static_cast<size_t>(i)] = PitchUtils::mixRetune(originalF0, targetF0, it->retuneSpeed);
                        } else {
                            tempBuffer[static_cast<size_t>(i)] = targetF0;
                        }
                    }
                    callback(segStart, tempBuffer.data(), length);
                } else {
                    callback(segStart, it->f0Data.data() + offset, length);
                }
                currentPos = segEnd;
            }

            ++it;
        } else {
            const int length = endFrame - currentPos;
            tempBuffer.assign(static_cast<size_t>(length), 0.0f);
            callback(currentPos, tempBuffer.data(), length);
            currentPos = endFrame;
        }
    }
}



void PitchCurve::applyCorrectionToRange(
    const std::vector<Note>& notes,
    int startFrame,
    int endFrame,
    float retuneSpeed,
    float vibratoDepth,
    float vibratoRate,
    double audioSampleRate)
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

    auto correctedSegments = oldSnapshot->getCorrectedSegments();
    clearSegmentsInRangePreserveOutside(correctedSegments, startFrame, endFrame);

    const int hopSize = oldSnapshot->getHopSize();
    const double sampleRate = oldSnapshot->getSampleRate();
    if (hopSize <= 0 || sampleRate <= 0.0 || audioSampleRate <= 0.0) {
        return;
    }

    struct NoteCorrectionInfo {
        float anchorPitch = 0.0f;
        float anchorMidi = 0.0f;
        float offsetSemitones = 0.0f;
        float rotationRad = 0.0f;
        float timeCenterSeconds = 0.0f;
    };

    std::vector<NoteCorrectionInfo> noteInfos(notes.size());

    const float radToDeg = 180.0f / juce::MathConstants<float>::pi;
    const float slopeAngleMinDeg = 10.0f;
    const float slopeAngleMaxDeg = 30.0f;
    const float slopeAt45DegSemitonesPerSecond = 7.0f;

    const double framePerSecond = sampleRate / static_cast<double>(hopSize);
    std::vector<size_t> relevantNoteIndices;
    for (size_t noteIndex = 0; noteIndex < notes.size(); ++noteIndex) {
        const auto& note = notes[noteIndex];

        size_t noteStartFrame = static_cast<size_t>(std::max(0, static_cast<int>(std::floor(note.startTime * framePerSecond))));
        size_t noteEndFrame = static_cast<size_t>(std::max(0, static_cast<int>(std::ceil(note.endTime * framePerSecond))));

        if (static_cast<int>(noteEndFrame) <= startFrame || static_cast<int>(noteStartFrame) >= endFrame) {
            continue;
        }

        relevantNoteIndices.push_back(noteIndex);

        NoteCorrectionInfo info;
        float anchorPitch = note.originalPitch;
        if (anchorPitch <= 0.0f) anchorPitch = note.pitch;
        info.anchorPitch = anchorPitch;
        info.anchorMidi = PitchUtils::freqToMidi(anchorPitch);
        info.timeCenterSeconds = static_cast<float>((note.startTime + note.endTime) * 0.5);

        float targetBaseF0 = note.getAdjustedPitch();
        if (targetBaseF0 > 0.0f && anchorPitch > 0.0f) {
            info.offsetSemitones = PitchUtils::freqToMidi(targetBaseF0) - PitchUtils::freqToMidi(anchorPitch);
        }

        if (info.anchorMidi > 0.0f && noteStartFrame < noteEndFrame) {
            std::vector<float> voicedTimes;
            std::vector<float> voicedMidis;
            for (size_t f = noteStartFrame; f < noteEndFrame && f < originalF0.size(); ++f) {
                float f0 = originalF0[f];
                if (f0 <= 0.0f) continue;
                float tSec = static_cast<float>(static_cast<double>(f) * static_cast<double>(hopSize) / sampleRate);
                voicedTimes.push_back(tSec);
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
    }

    if (relevantNoteIndices.empty()) {
        return;
    }

    std::vector<float> correctedF0Buffer(endFrame - startFrame, 0.0f);
    std::vector<int> activeNotePerFrame(endFrame - startFrame, -1);

    for (int i = startFrame; i < endFrame; ++i) {
        float f0 = originalF0[i];
        if (f0 <= 0.0f) {
            correctedF0Buffer[i - startFrame] = 0.0f;
            continue;
        }

        int64_t audioSamplePos = static_cast<int64_t>(std::llround(static_cast<double>(i) * static_cast<double>(hopSize) * audioSampleRate / sampleRate));
        double timeSeconds = static_cast<double>(audioSamplePos) / audioSampleRate;

        const Note* activeNote = nullptr;
        size_t activeNoteIndex = 0;
        for (size_t idx : relevantNoteIndices) {
            const auto& note = notes[idx];
            if (timeSeconds >= note.startTime && timeSeconds < note.endTime) {
                activeNote = &note;
                activeNoteIndex = idx;
                break;
            }
        }

        if (activeNote) {
            activeNotePerFrame[i - startFrame] = static_cast<int>(activeNoteIndex);

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
            if (activeNoteIndex < noteInfos.size() && noteInfos[activeNoteIndex].rotationRad != 0.0f) {
                float tSec = static_cast<float>(timeSeconds);
                float x = tSec - noteInfos[activeNoteIndex].timeCenterSeconds;
                float y = PitchUtils::freqToMidi(f0) - noteInfos[activeNoteIndex].anchorMidi;
                float c = std::cos(noteInfos[activeNoteIndex].rotationRad);
                float s = std::sin(noteInfos[activeNoteIndex].rotationRad);
                float yRot = x * s + y * c;
                baseF0 = PitchUtils::midiToFreq(noteInfos[activeNoteIndex].anchorMidi + yRot);
            }

            float shiftedF0 = baseF0;
            if (activeNoteIndex < noteInfos.size() && noteInfos[activeNoteIndex].anchorPitch > 0.0f && targetBaseF0 > 0.0f) {
                float shiftRatio = std::pow(2.0f, noteInfos[activeNoteIndex].offsetSemitones / 12.0f);
                shiftedF0 = baseF0 * shiftRatio;
            }

            float frameRetuneSpeed = retuneSpeed;
            if (activeNote->retuneSpeed >= 0.0f) {
                frameRetuneSpeed = activeNote->retuneSpeed;
            }

            correctedF0Buffer[i - startFrame] = PitchUtils::mixRetune(shiftedF0, targetF0, frameRetuneSpeed);
        } else {
            correctedF0Buffer[i - startFrame] = f0;
        }
    }

    CorrectedSegment newSeg(startFrame, endFrame, correctedF0Buffer, CorrectedSegment::Source::NoteBased);
    newSeg.retuneSpeed = retuneSpeed;
    newSeg.vibratoDepth = vibratoDepth;
    newSeg.vibratoRate = vibratoRate;

    insertSegmentWithUnifiedTransitions(correctedSegments, originalF0, std::move(newSeg), kUnifiedTransitionFrames);

    uint64_t newGen = incrementGeneration();
    auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
        oldSnapshot->getOriginalF0(),
        oldSnapshot->getOriginalEnergy(),
        std::move(correctedSegments),
        hopSize,
        sampleRate,
        newGen
    );
    std::atomic_store(&snapshot_, newSnapshot);
}

void PitchCurve::setManualCorrectionRange(int startFrame, int endFrame, const std::vector<float>& f0Data,
                                          CorrectedSegment::Source source) {
    if (startFrame >= endFrame || f0Data.empty()) {
        return;
    }

    auto oldSnapshot = getSnapshot();
    auto correctedSegments = oldSnapshot->getCorrectedSegments();
    
    CorrectedSegment newSeg(startFrame, endFrame, f0Data, source);
    clearSegmentsInRangePreserveOutside(correctedSegments, startFrame, endFrame);
    insertSegmentWithUnifiedTransitions(correctedSegments, oldSnapshot->getOriginalF0(), std::move(newSeg), kUnifiedTransitionFrames);

    uint64_t newGen = incrementGeneration();
    auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
        oldSnapshot->getOriginalF0(),
        oldSnapshot->getOriginalEnergy(),
        std::move(correctedSegments),
        oldSnapshot->getHopSize(),
        oldSnapshot->getSampleRate(),
        newGen
    );
    std::atomic_store(&snapshot_, newSnapshot);
}

void PitchCurve::setManualCorrectionRange(int startFrame, int endFrame, const std::vector<float>& f0Data,
                                          CorrectedSegment::Source source, float retuneSpeed) {
    if (startFrame >= endFrame || f0Data.empty()) {
        return;
    }

    auto oldSnapshot = getSnapshot();
    auto correctedSegments = oldSnapshot->getCorrectedSegments();
    
    CorrectedSegment newSeg(startFrame, endFrame, f0Data, source);
    newSeg.retuneSpeed = retuneSpeed;
    clearSegmentsInRangePreserveOutside(correctedSegments, startFrame, endFrame);
    insertSegmentWithUnifiedTransitions(correctedSegments, oldSnapshot->getOriginalF0(), std::move(newSeg), kUnifiedTransitionFrames);

    uint64_t newGen = incrementGeneration();
    auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
        oldSnapshot->getOriginalF0(),
        oldSnapshot->getOriginalEnergy(),
        std::move(correctedSegments),
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
    auto correctedSegments = oldSnapshot->getCorrectedSegments();
    clearSegmentsInRangePreserveOutside(correctedSegments, startFrame, endFrame);

    uint64_t newGen = incrementGeneration();
    auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
        oldSnapshot->getOriginalF0(),
        oldSnapshot->getOriginalEnergy(),
        std::move(correctedSegments),
        oldSnapshot->getHopSize(),
        oldSnapshot->getSampleRate(),
        newGen
    );
    std::atomic_store(&snapshot_, newSnapshot);
}

} // namespace OpenTune
