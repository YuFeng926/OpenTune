#include "PitchCurve.h"
#include "PitchUtils.h"
#include <algorithm>
#include <cmath>
#include <limits>
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

// 零相位FIR低通滤波器：使用汉宁窗，更好的阻带衰减
// 窗口大小为windowFrames帧，截止频率约 sampleRate/(2*windowFrames*hopSize) Hz
// 汉宁窗主瓣宽度3个bin，旁瓣衰减-31dB，适合分离drift(<3Hz)和vibrato(>5Hz)
// 无声帧(NaN)不参与平均，通过权重归零处理
std::vector<float> computeZeroPhaseDrift(const std::vector<float>& deviations, int windowFrames)
{
    const int n = static_cast<int>(deviations.size());
    if (n <= 0 || windowFrames <= 1) {
        return deviations;
    }
    
    // 确保奇数窗口，保证对称性
    const int win = (windowFrames % 2 == 0) ? windowFrames + 1 : windowFrames;
    const int halfWin = win / 2;
    
    // 预计算汉宁窗系数
    std::vector<float> hanning(win);
    for (int k = 0; k < win; ++k) {
        hanning[k] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265f * k / (win - 1)));
    }
    
    std::vector<float> drift(n);
    
    for (int i = 0; i < n; ++i) {
        float sum = 0.0f;
        float weightSum = 0.0f;
        
        for (int k = -halfWin; k <= halfWin; ++k) {
            int idx = i + k;
            // 对称镜像边界处理：超出范围时镜像反射
            if (idx < 0) idx = -idx;
            if (idx >= n) idx = 2 * n - idx - 2;
            idx = std::max(0, std::min(n - 1, idx));
            
            // 跳过无声帧(NaN)
            const float dev = deviations[idx];
            if (std::isnan(dev)) continue;
            
            const float w = hanning[k + halfWin];
            sum += dev * w;
            weightSum += w;
        }
        
        drift[i] = (weightSum > 0.0f) ? (sum / weightSum) : 0.0f;
    }
    
    return drift;
}

double noteTransitionFrameAt(const Note& leftNote,
                             const Note& rightNote,
                             double secondsPerFrame) noexcept
{
    return 0.5 * (leftNote.endTime + rightNote.startTime) / secondsPerFrame;
}

struct NoteTransitionSpan
{
    size_t leftNoteIndex = 0;
    size_t rightNoteIndex = 0;
    double boundaryFrame = 0.0;
    double halfWidthFrames = 0.0;
};

struct NoteCorrectionInfo
{
    float anchorPitch = 0.0f;
    float anchorMidi = 0.0f;
    float targetMidi = 0.0f;
    float retuneSpeed = 0.0f;
    float rotationRad = 0.0f;
    double timeCenterSeconds = 0.0;
};

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
        std::max(0, rangeStart - 2 * getCorrectedF0BoundaryContextFrames()),
        std::min(frameCount, rangeEnd + 2 * getCorrectedF0BoundaryContextFrames())
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

    std::vector<NoteCorrectionInfo> noteInfos(notes.size());

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
        info.targetMidi = PitchUtils::freqToMidi(note.getAdjustedPitch());
        info.retuneSpeed = juce::jlimit(
            0.0f, 1.0f, note.retuneSpeed >= 0.0f ? note.retuneSpeed : retuneSpeed);
        info.timeCenterSeconds = (note.startTime + note.endTime) * 0.5;

        if (info.anchorMidi > 0.0f && noteStartFrame < noteEndFrame) {
            std::vector<float> voicedTimes;
            std::vector<float> voicedMidis;
            for (int f = static_cast<int>(noteStartFrame);
                 f < static_cast<int>(noteEndFrame) && f < static_cast<int>(originalF0.size());
                 ++f) {
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
    }

    if (relevantNoteIndices.empty()) {
        return;
    }
    std::sort(relevantNoteIndices.begin(), relevantNoteIndices.end(),
        [&notes](size_t left, size_t right) {
            return notes[left].startTime < notes[right].startTime;
        });

    const double defaultHalfWidthFrames =
        static_cast<double>(getCorrectedF0BoundaryContextFrames());
    const auto calculateTransitionSpan = [&](size_t position)
        -> std::optional<NoteTransitionSpan> {
        if (position + 1 >= relevantNoteIndices.size()) {
            return std::nullopt;
        }

        const size_t leftIndex = relevantNoteIndices[position];
        const size_t rightIndex = relevantNoteIndices[position + 1];
        const double gapFrames =
            (notes[rightIndex].startTime - notes[leftIndex].endTime) / secondsPerFrame;
        if (gapFrames >= 2.0 * defaultHalfWidthFrames) {
            return std::nullopt;
        }

        const double boundaryFrame =
            noteTransitionFrameAt(notes[leftIndex], notes[rightIndex], secondsPerFrame);
        double halfWidthFrames = defaultHalfWidthFrames;

        if (position > 0) {
            const size_t previousLeftIndex = relevantNoteIndices[position - 1];
            const size_t previousRightIndex = relevantNoteIndices[position];
            const double previousGapFrames =
                (notes[previousRightIndex].startTime - notes[previousLeftIndex].endTime)
                / secondsPerFrame;
            if (previousGapFrames < 2.0 * defaultHalfWidthFrames) {
                const double previousBoundaryFrame = noteTransitionFrameAt(
                    notes[previousLeftIndex], notes[previousRightIndex], secondsPerFrame);
                halfWidthFrames = std::min(
                    halfWidthFrames,
                    boundaryFrame - 0.5 * (previousBoundaryFrame + boundaryFrame));
            }
        }

        if (position + 2 < relevantNoteIndices.size()) {
            const size_t nextLeftIndex = relevantNoteIndices[position + 1];
            const size_t nextRightIndex = relevantNoteIndices[position + 2];
            const double nextGapFrames =
                (notes[nextRightIndex].startTime - notes[nextLeftIndex].endTime)
                / secondsPerFrame;
            if (nextGapFrames < 2.0 * defaultHalfWidthFrames) {
                const double nextBoundaryFrame = noteTransitionFrameAt(
                    notes[nextLeftIndex], notes[nextRightIndex], secondsPerFrame);
                halfWidthFrames = std::min(
                    halfWidthFrames,
                    0.5 * (boundaryFrame + nextBoundaryFrame) - boundaryFrame);
            }
        }

        return NoteTransitionSpan{leftIndex, rightIndex, boundaryFrame, halfWidthFrames};
    };

    // 预计算每个音符的漂移分量d(t)：零相位汉宁窗FIR，完整音符作为分析域
    // 偏差相对anchorPitch（原始音高）计算，确保：
    // 1. midi(shiftedF0) - midi(targetBaseF0) = midi(f0) - midi(anchorPitch)（pitch shift是常量）
    // 2. drift工具缩放的是原始音高曲线的慢速分量，与移调无关
    struct NoteDriftInfo {
        std::vector<float> driftComponent;  // d(t) in semitones
        int noteStartFrame = 0;
        int noteEndFrame = 0;
    };
    std::vector<NoteDriftInfo> noteDriftInfos(notes.size());
    
    // 窗口大小：约500ms（~43帧@86Hz帧率），汉宁窗-3dB点约0.44/fs
    // 500ms窗口截止频率约2Hz，确保vibrato(>5Hz)不泄漏
    const int driftWindowFrames = std::max(5, static_cast<int>(std::lround(0.5 * sampleRate / hopSize)));
    
    for (size_t ni = 0; ni < notes.size(); ++ni) {
        const auto& note = notes[ni];
        const float noteAnchorPitch = note.originalPitch;
        if (noteAnchorPitch <= 0.0f || note.startTime >= note.endTime) continue;
        
        const int noteStartFrame = static_cast<int>(f0tl.frameAtOrBefore(note.startTime));
        const int noteEndFrame = static_cast<int>(f0tl.exclusiveFrameAt(note.endTime));
        // 使用完整音符范围，不裁剪到calculationRange，保证编辑顺序无关
        const int fullStart = std::max(0, noteStartFrame);
        const int fullEnd = std::min(maxFrame, noteEndFrame);
        if (fullEnd <= fullStart) continue;
        
        const int noteFrameCount = fullEnd - fullStart;
        std::vector<float> deviations(noteFrameCount);
        
        for (int f = fullStart; f < fullEnd; ++f) {
            const float rawF0 = originalF0[f];
            const float f0 = rawF0 > 0.0f ? rawF0 * sourcePitchRatio : rawF0;
            if (f0 <= 0.0f) {
                // 无声帧：标记为NaN，后续跳过不参与平均
                deviations[f - fullStart] = std::numeric_limits<float>::quiet_NaN();
                continue;
            }
            // 偏差相对anchorPitch（原始音高），不含vibrato和pitch shift
            deviations[f - fullStart] = PitchUtils::freqToMidi(f0) - PitchUtils::freqToMidi(noteAnchorPitch);
        }
        
        auto& info = noteDriftInfos[ni];
        info.driftComponent = computeZeroPhaseDrift(deviations, driftWindowFrames);
        info.noteStartFrame = fullStart;
        info.noteEndFrame = fullEnd;
    }

    std::vector<float> correctedF0Buffer(calculationEndFrame - calculationStartFrame, 0.0f);
    size_t activeNoteCursor = 0;
    size_t transitionPairCursor = 0;
    std::optional<NoteTransitionSpan> currentTransitionSpan;
    const auto updateCurrentTransitionSpan = [&]() {
        currentTransitionSpan.reset();
        while (transitionPairCursor + 1 < relevantNoteIndices.size()) {
            currentTransitionSpan = calculateTransitionSpan(transitionPairCursor);
            if (currentTransitionSpan.has_value()) {
                return;
            }
            ++transitionPairCursor;
        }
    };
    updateCurrentTransitionSpan();

    for (int i = calculationStartFrame; i < calculationEndFrame; ++i) {
        const float rawF0 = originalF0[i];
        float f0 = rawF0 > 0.0f ? rawF0 * sourcePitchRatio : rawF0;
        if (f0 <= 0.0f) {
            correctedF0Buffer[i - calculationStartFrame] = 0.0f;
            continue;
        }

        const double timeSeconds = f0tl.timeAtFrame(i);

        while (activeNoteCursor < relevantNoteIndices.size()
               && timeSeconds >= notes[relevantNoteIndices[activeNoteCursor]].endTime) {
            ++activeNoteCursor;
        }

        const Note* activeNote = nullptr;
        size_t activeNoteIndex = 0;
        if (activeNoteCursor < relevantNoteIndices.size()) {
            const size_t idx = relevantNoteIndices[activeNoteCursor];
            const auto& note = notes[idx];
            if (timeSeconds >= note.startTime && timeSeconds < note.endTime) {
                activeNote = &note;
                activeNoteIndex = idx;
            }
        }

        const double frameCenter = static_cast<double>(i) + 0.5;
        while (currentTransitionSpan.has_value()) {
            const auto& span = *currentTransitionSpan;
            const double spanEnd = span.boundaryFrame + span.halfWidthFrames;
            if (frameCenter < spanEnd) {
                break;
            }
            ++transitionPairCursor;
            updateCurrentTransitionSpan();
        }

        const NoteTransitionSpan* transitionSpan = nullptr;
        if (currentTransitionSpan.has_value()) {
            const auto& span = *currentTransitionSpan;
            const double spanStart = span.boundaryFrame - span.halfWidthFrames;
            if (frameCenter >= spanStart)
                transitionSpan = &span;
        }

        float targetMidi = 0.0f;
        float anchorMidi = 0.0f;
        float transitionRetuneSpeed = 0.0f;
        bool hasTransitionContext = false;
        if (transitionSpan != nullptr) {
            const auto& leftInfo = noteInfos[transitionSpan->leftNoteIndex];
            const auto& rightInfo = noteInfos[transitionSpan->rightNoteIndex];
            if (leftInfo.anchorMidi > 0.0f
                && rightInfo.anchorMidi > 0.0f
                && transitionSpan->halfWidthFrames > 0.0) {
                const float t = static_cast<float>(
                    (frameCenter - (transitionSpan->boundaryFrame - transitionSpan->halfWidthFrames))
                    / (transitionSpan->halfWidthFrames * 2.0));
                const float w = smootherstep(t);
                targetMidi = leftInfo.targetMidi + (rightInfo.targetMidi - leftInfo.targetMidi) * w;
                anchorMidi = leftInfo.anchorMidi + (rightInfo.anchorMidi - leftInfo.anchorMidi) * w;
                transitionRetuneSpeed = leftInfo.retuneSpeed
                    + (rightInfo.retuneSpeed - leftInfo.retuneSpeed) * w;
                hasTransitionContext = true;
            }
        }

        if (activeNote) {
            const auto& activeInfo = noteInfos[activeNoteIndex];
            if (!hasTransitionContext) {
                targetMidi = activeInfo.targetMidi;
                anchorMidi = activeInfo.anchorMidi;
                transitionRetuneSpeed = activeInfo.retuneSpeed;
            }

            float vibratoOffsetSemitones = 0.0f;
            float noteVibratoDepth = vibratoDepth;
            float noteVibratoRate = vibratoRate;
            if (activeNote->vibratoDepth >= 0.0f) noteVibratoDepth = activeNote->vibratoDepth;
            if (activeNote->vibratoRate >= 0.0f) noteVibratoRate = activeNote->vibratoRate;
            if (noteVibratoDepth > 0.0f) {
                const double timeInNote = timeSeconds - activeNote->startTime;
                const float depthSemitones = (noteVibratoDepth / 100.0f) * 1.0f;
                vibratoOffsetSemitones = depthSemitones * std::sin(
                    2.0f * juce::MathConstants<float>::pi * noteVibratoRate * static_cast<float>(timeInNote));
            }

            const float targetBaseF0 = PitchUtils::midiToFreq(targetMidi);
            const float targetF0 = PitchUtils::midiToFreq(targetMidi + vibratoOffsetSemitones);

            float baseF0 = f0;
            if (activeInfo.rotationRad != 0.0f) {
                const double tSec = timeSeconds;
                float x = static_cast<float>(tSec - activeInfo.timeCenterSeconds);
                float y = PitchUtils::freqToMidi(f0) - activeInfo.anchorMidi;
                float c = std::cos(activeInfo.rotationRad);
                float s = std::sin(activeInfo.rotationRad);
                float yRot = x * s + y * c;
                baseF0 = PitchUtils::midiToFreq(activeInfo.anchorMidi + yRot);
            }

            float shiftedF0 = baseF0;
            if (activeInfo.anchorPitch > 0.0f && targetBaseF0 > 0.0f) {
                const float sourceResidualSemitones =
                    PitchUtils::freqToMidi(baseF0) - anchorMidi;
                shiftedF0 = PitchUtils::midiToFreq(targetMidi + sourceResidualSemitones);
            }

            // --- Pitch Drift: scale only the slow drift component d(t), leave vibrato untouched ---
            {
                float framePitchDriftScale = pitchDriftScale;
                if (activeNote->pitchDriftScale != 1.0f) {
                    framePitchDriftScale = activeNote->pitchDriftScale;
                }
                if (framePitchDriftScale != 1.0f && activeNote->startTime < activeNote->endTime) {
                    // 使用预计算的零相位漂移分量d(t)
                    const auto& driftInfo = noteDriftInfos[activeNoteIndex];
                    if (i >= driftInfo.noteStartFrame && i < driftInfo.noteEndFrame) {
                        const int localFrame = i - driftInfo.noteStartFrame;
                        const float driftComponent = driftInfo.driftComponent[localFrame];
                        
                        // Deviation is measured around the current absolute
                        // transition target, after target/anchor interpolation.
                        const float devSemitones = PitchUtils::freqToMidi(shiftedF0) - targetMidi;
                        // 调制分量m(t) = devSemitones - d(t)
                        const float modulationComponent = devSemitones - driftComponent;
                        
                        // 缩放漂移分量，保留调制分量不变
                        // f'(t) = transitionTarget + s*d(t) + m(t)
                        const float scaledDev = driftComponent * framePitchDriftScale + modulationComponent;
                        shiftedF0 = PitchUtils::midiToFreq(targetMidi + scaledDev);
                    }
                }
            }

            correctedF0Buffer[i - calculationStartFrame] = PitchUtils::mixRetune(
                shiftedF0, targetF0, transitionRetuneSpeed);
        } else {
            if (hasTransitionContext) {
                const float sourceResidualSemitones =
                    PitchUtils::freqToMidi(f0) - anchorMidi;
                const float correctedMidi = targetMidi
                    + sourceResidualSemitones * (1.0f - transitionRetuneSpeed);
                correctedF0Buffer[i - calculationStartFrame] = PitchUtils::midiToFreq(correctedMidi);
                continue;
            }

            const NoteCorrectionInfo* nearestInfo = nullptr;
            float bestDist = 1e30f;
            const size_t firstCandidatePosition = activeNoteCursor > 0 ? activeNoteCursor - 1 : 0;
            for (size_t candidatePosition = firstCandidatePosition;
                 candidatePosition < relevantNoteIndices.size()
                     && candidatePosition <= activeNoteCursor;
                 ++candidatePosition) {
                const size_t idx = relevantNoteIndices[candidatePosition];
                const auto& note = notes[idx];
                const auto& info = noteInfos[idx];
                if (info.anchorPitch <= 0.0f || info.anchorMidi <= 0.0f)
                    continue;
                const float dist = (timeSeconds < note.startTime)
                    ? static_cast<float>(note.startTime - timeSeconds)
                    : static_cast<float>(timeSeconds - note.endTime);
                if (dist < bestDist) {
                    bestDist = dist;
                    nearestInfo = &info;
                }
            }

            if (nearestInfo != nullptr) {
                const float sourceResidualSemitones =
                    PitchUtils::freqToMidi(f0) - nearestInfo->anchorMidi;
                const float correctedMidi = nearestInfo->targetMidi
                    + sourceResidualSemitones * (1.0f - nearestInfo->retuneSpeed);
                correctedF0Buffer[i - calculationStartFrame] = PitchUtils::midiToFreq(correctedMidi);
            } else {
                correctedF0Buffer[i - calculationStartFrame] = f0;
            }
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
