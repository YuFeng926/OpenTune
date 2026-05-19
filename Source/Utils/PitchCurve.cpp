#include "PitchCurve.h"
#include "PitchUtils.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace OpenTune {

namespace {

void insertSegmentSorted(std::vector<CorrectedSegment>& segments, CorrectedSegment&& seg)
{
    auto insertPos = std::lower_bound(segments.begin(), segments.end(), seg.startFrame,
        [](const CorrectedSegment& s, int frame) {
            return s.startFrame < frame;
        });
    segments.insert(insertPos, std::move(seg));
}

void clearSegmentsMatchingSourceInRangePreserveOutside(std::vector<CorrectedSegment>& segments,
                                                       int startFrame,
                                                       int endFrame,
                                                       CorrectedSegment::Source source)
{
    if (startFrame >= endFrame) {
        return;
    }

    std::vector<CorrectedSegment> kept;
    kept.reserve(segments.size() + 1);

    for (const auto& seg : segments) {
        if (seg.source != source) {
            kept.push_back(seg);
            continue;
        }

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

void insertNoteBasedSegmentPreservingNonNoteBasedSegments(std::vector<CorrectedSegment>& segments,
                                                          CorrectedSegment&& noteBasedSegment)
{
    std::vector<std::pair<int, int>> preservedRanges;
    for (const auto& seg : segments) {
        if (seg.source == CorrectedSegment::Source::NoteBased
            || seg.endFrame <= noteBasedSegment.startFrame
            || seg.startFrame >= noteBasedSegment.endFrame) {
            continue;
        }

        preservedRanges.emplace_back(std::max(seg.startFrame, noteBasedSegment.startFrame),
                                     std::min(seg.endFrame, noteBasedSegment.endFrame));
    }

    if (preservedRanges.empty()) {
        insertSegmentSorted(segments, std::move(noteBasedSegment));
        return;
    }

    std::sort(preservedRanges.begin(), preservedRanges.end());

    int cursor = noteBasedSegment.startFrame;
    for (const auto& [protectedStart, protectedEnd] : preservedRanges) {
        if (cursor < protectedStart) {
            CorrectedSegment piece = noteBasedSegment;
            piece.startFrame = cursor;
            piece.endFrame = protectedStart;
            const int offset = piece.startFrame - noteBasedSegment.startFrame;
            const int length = piece.endFrame - piece.startFrame;
            if (offset >= 0
                && length > 0
                && offset + length <= static_cast<int>(noteBasedSegment.f0Data.size())) {
                piece.f0Data.assign(noteBasedSegment.f0Data.begin() + offset,
                                    noteBasedSegment.f0Data.begin() + offset + length);
                insertSegmentSorted(segments, std::move(piece));
            }
        }

        cursor = std::max(cursor, protectedEnd);
    }

    if (cursor < noteBasedSegment.endFrame) {
        CorrectedSegment piece = noteBasedSegment;
        piece.startFrame = cursor;
        const int offset = piece.startFrame - noteBasedSegment.startFrame;
        const int length = piece.endFrame - piece.startFrame;
        if (offset >= 0
            && length > 0
            && offset + length <= static_cast<int>(noteBasedSegment.f0Data.size())) {
            piece.f0Data.assign(noteBasedSegment.f0Data.begin() + offset,
                                noteBasedSegment.f0Data.begin() + offset + length);
            insertSegmentSorted(segments, std::move(piece));
        }
    }
}

float smootherstep(float t) noexcept
{
    t = juce::jlimit(0.0f, 1.0f, t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

double noteTransitionFrameAt(const Note& leftNote,
                             const Note& rightNote,
                             double framePerSecond) noexcept
{
    return 0.5 * (leftNote.endTime + rightNote.startTime) * framePerSecond;
}

float noteBoundaryShiftSemitoneOffset(const std::vector<Note>& notes,
                                      const std::vector<size_t>& relevantNoteIndices,
                                      const std::vector<float>& noteAnchorMidis,
                                      size_t activeNoteIndex,
                                      float activeOffsetSemitones,
                                      int frame,
                                      double framePerSecond,
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
                                                 noteTransitionFrameAt(notes[leftNoteIndex], notes[activeNoteIndex], framePerSecond),
                                                 true);
        if (bridged.has_value()) {
            return *bridged;
        }
    }

    if (position + 1 < relevantNoteIndices.size()) {
        const size_t rightNoteIndex = relevantNoteIndices[position + 1];
        const auto bridged = transitionOffsetFor(rightNoteIndex,
                                                 noteTransitionFrameAt(notes[activeNoteIndex], notes[rightNoteIndex], framePerSecond),
                                                 false);
        if (bridged.has_value()) {
            return *bridged;
        }
    }

    return activeOffsetSemitones;
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

                callback(segStart, it->f0Data.data() + offset, length);
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

                callback(segStart, it->f0Data.data() + offset, length);
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

    const int hopSize = oldSnapshot->getHopSize();
    const double sampleRate = oldSnapshot->getSampleRate();
    if (hopSize <= 0 || sampleRate <= 0.0 || audioSampleRate <= 0.0) {
        return;
    }

    const auto calculationRange = expandNoteBasedCorrectionRange(startFrame, endFrame, maxFrame);
    if (calculationRange.isEmpty()) {
        return;
    }
    const int calculationStartFrame = calculationRange.startFrame;
    const int calculationEndFrame = calculationRange.endFrameExclusive;

    auto correctedSegments = oldSnapshot->getCorrectedSegments();
    clearSegmentsMatchingSourceInRangePreserveOutside(correctedSegments,
                                                      calculationStartFrame,
                                                      calculationEndFrame,
                                                      CorrectedSegment::Source::NoteBased);

    struct NoteCorrectionInfo {
        float anchorPitch = 0.0f;
        float anchorMidi = 0.0f;
        float rotationRad = 0.0f;
        float timeCenterSeconds = 0.0f;
    };

    std::vector<NoteCorrectionInfo> noteInfos(notes.size());
    std::vector<float> noteAnchorMidis(notes.size(), 0.0f);

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
        info.timeCenterSeconds = static_cast<float>((note.startTime + note.endTime) * 0.5);

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
        float f0 = originalF0[i];
        if (f0 <= 0.0f) {
            correctedF0Buffer[i - calculationStartFrame] = 0.0f;
            continue;
        }

        int64_t audioSamplePos = static_cast<int64_t>(std::llround(static_cast<double>(i) * static_cast<double>(hopSize) * audioSampleRate / sampleRate));
        double timeSeconds = static_cast<double>(audioSamplePos) / audioSampleRate;

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
                float tSec = static_cast<float>(timeSeconds);
                float x = tSec - noteInfos[activeNoteIndex].timeCenterSeconds;
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
                                                                                     framePerSecond,
                                                                                     frameRetuneSpeed);
                float shiftRatio = std::pow(2.0f, dynamicOffsetSemitones / 12.0f);
                shiftedF0 = baseF0 * shiftRatio;
            }

            correctedF0Buffer[i - calculationStartFrame] = PitchUtils::mixRetune(shiftedF0, targetF0, frameRetuneSpeed);
        } else {
            correctedF0Buffer[i - calculationStartFrame] = f0;
        }
    }

    CorrectedSegment newSeg(calculationStartFrame, calculationEndFrame, correctedF0Buffer, CorrectedSegment::Source::NoteBased);
    newSeg.retuneSpeed = retuneSpeed;
    newSeg.vibratoDepth = vibratoDepth;
    newSeg.vibratoRate = vibratoRate;

    insertNoteBasedSegmentPreservingNonNoteBasedSegments(correctedSegments, std::move(newSeg));

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
    insertSegmentSorted(correctedSegments, std::move(newSeg));

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
