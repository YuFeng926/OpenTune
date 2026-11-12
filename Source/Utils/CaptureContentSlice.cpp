#include "CaptureContentSlice.h"
#include "F0Timeline.h"
#include "PlaybackSampleGeometry.h"
#include "TimeCoordinate.h"

#include <algorithm>
#include <cstdint>

namespace OpenTune {

std::vector<Note> sliceNotesToLocalRange(const std::vector<Note>& notes,
                                         double startSeconds,
                                         double endSeconds)
{
    std::vector<Note> slicedNotes;
    if (endSeconds <= startSeconds) {
        return slicedNotes;
    }

    for (const auto& note : notes) {
        const double overlapStart = std::max(note.startTime, startSeconds);
        const double overlapEnd = std::min(note.endTime, endSeconds);
        if (overlapEnd <= overlapStart) {
            continue;
        }

        Note slicedNote = note;
        slicedNote.startTime = overlapStart - startSeconds;
        slicedNote.endTime = overlapEnd - startSeconds;
        slicedNotes.push_back(slicedNote);
    }

    return normalizeStoredNotes(slicedNotes);
}

std::shared_ptr<PitchCurve> slicePitchCurveToFrameRange(
    const std::shared_ptr<const PitchCurveSnapshot>& pitchCurve,
    int startFrame,
    int endFrameExclusive)
{
    if (pitchCurve == nullptr || endFrameExclusive <= startFrame) {
        return nullptr;
    }

    const auto& snapshot = *pitchCurve;
    const int hopSize = snapshot.getHopSize();
    const double sampleRate = snapshot.getSampleRate();
    if (hopSize <= 0 || sampleRate <= 0.0) {
        return nullptr;
    }

    const auto& originalF0 = snapshot.getOriginalF0();
    if (startFrame < 0 || endFrameExclusive > static_cast<int>(originalF0.size())) {
        return nullptr;
    }

    auto slicedCurve = std::make_shared<PitchCurve>();
    slicedCurve->setHopSize(hopSize);
    slicedCurve->setSampleRate(sampleRate);

    slicedCurve->setOriginalF0(std::vector<float>(originalF0.begin() + startFrame, originalF0.begin() + endFrameExclusive));

    std::vector<F0FrameRange> slicedErasedRanges;
    for (const auto& range : snapshot.getErasedRanges()) {
        const int overlapStart = std::max(range.startFrame, startFrame);
        const int overlapEnd = std::min(range.endFrameExclusive, endFrameExclusive);
        if (overlapStart < overlapEnd)
            slicedErasedRanges.push_back({overlapStart - startFrame, overlapEnd - startFrame});
    }
    if (!slicedErasedRanges.empty())
        slicedCurve->replaceErasedRangesInRange(0, endFrameExclusive - startFrame, slicedErasedRanges);

    const auto& originalEnergy = snapshot.getOriginalEnergy();
    if (originalEnergy.size() >= static_cast<size_t>(endFrameExclusive)) {
        slicedCurve->setOriginalEnergy(std::vector<float>(originalEnergy.begin() + startFrame, originalEnergy.begin() + endFrameExclusive));
    }

    std::vector<PitchCorrectionSegment> slicedSegments;
    for (const auto& segment : snapshot.getCorrectionSegments()) {
        const int overlapStart = std::max(segment.startFrame, startFrame);
        const int overlapEnd = std::min(segment.endFrame, endFrameExclusive);
        if (overlapEnd <= overlapStart) {
            continue;
        }

        PitchCorrectionSegment slicedSegment = segment;
        const int originalOffsetStart = overlapStart - segment.startFrame;
        const int originalOffsetEnd = overlapEnd - segment.startFrame;
        slicedSegment.startFrame = overlapStart - startFrame;
        slicedSegment.endFrame = overlapEnd - startFrame;
        slicedSegment.f0Data.assign(segment.f0Data.begin() + originalOffsetStart,
                                    segment.f0Data.begin() + originalOffsetEnd);
        // baseF0Data 与原段 frame 布局对齐：仅在完整覆盖原段 frame 数时按同区间切片。
        if (segment.baseF0Data.size() >= static_cast<size_t>(segment.endFrame - segment.startFrame)) {
            slicedSegment.baseF0Data.assign(segment.baseF0Data.begin() + originalOffsetStart,
                                            segment.baseF0Data.begin() + originalOffsetEnd);
        }
        slicedSegments.push_back(std::move(slicedSegment));
    }
    slicedCurve->replaceCorrectionSegments(slicedSegments);
    return slicedCurve;
}

std::shared_ptr<PitchCurve> slicePitchCurveToLocalRange(
    const std::shared_ptr<const PitchCurveSnapshot>& pitchCurve,
    double startSeconds,
    double endSeconds)
{
    if (pitchCurve == nullptr || endSeconds <= startSeconds) {
        return nullptr;
    }

    const auto frameRange = f0FrameRangeForSeconds(*pitchCurve, startSeconds, endSeconds);
    if (!frameRange.has_value()) {
        return nullptr;
    }

    return slicePitchCurveToFrameRange(pitchCurve, frameRange->startFrame, frameRange->endFrameExclusive);
}

ContentState sliceContentState(const ContentState& source,
                               int64_t offsetSamples,
                               int64_t count,
                               double sampleRate)
{
    ContentState sliced;

    const int64_t totalSamples = source.audioBuffer != nullptr
        ? static_cast<int64_t>(source.audioBuffer->getNumSamples())
        : 0;
    const int64_t clampedStart = std::clamp<int64_t>(offsetSamples, 0, totalSamples);
    const int64_t clampedEnd = std::clamp<int64_t>(offsetSamples + count, clampedStart, totalSamples);
    if (source.audioBuffer != nullptr && clampedEnd > clampedStart) {
        auto bufferCopy = std::make_shared<juce::AudioBuffer<float>>(
            source.audioBuffer->getNumChannels(),
            static_cast<int>(clampedEnd - clampedStart));
        for (int channel = 0; channel < bufferCopy->getNumChannels(); ++channel) {
            bufferCopy->copyFrom(channel,
                                 0,
                                 *source.audioBuffer,
                                 channel,
                                 static_cast<int>(clampedStart),
                                 bufferCopy->getNumSamples());
        }
        sliced.audioBuffer = std::move(bufferCopy);
    }

    const double offsetSeconds = static_cast<double>(clampedStart) / sampleRate;
    const double endSeconds = static_cast<double>(clampedEnd) / sampleRate;

    sliced.notes = sliceNotesToLocalRange(source.notes, offsetSeconds, endSeconds);
    sliced.analysis.silentGaps.clear();
    for (const auto& gap : source.analysis.silentGaps) {
        const int64_t overlapStart = std::max<int64_t>(gap.startSample, offsetSamples);
        const int64_t overlapEnd = std::min<int64_t>(gap.endSampleExclusive, offsetSamples + count);
        if (overlapEnd <= overlapStart)
            continue;
        SilentGap slicedGap;
        slicedGap.startSample = overlapStart - offsetSamples;
        slicedGap.endSampleExclusive = overlapEnd - offsetSamples;
        slicedGap.minLevel_dB = gap.minLevel_dB;
        sliced.analysis.silentGaps.push_back(slicedGap);
    }

    std::shared_ptr<const PitchCurveSnapshot> sourceCurve = source.analysis.pitchCurve
        ? source.analysis.pitchCurve->getSnapshot()
        : nullptr;
    sliced.analysis.pitchCurve = slicePitchCurveToLocalRange(sourceCurve, offsetSeconds, endSeconds);
    sliced.analysis.originalF0State = sliced.analysis.pitchCurve != nullptr
        ? OriginalF0State::Ready
        : OriginalF0State::NotRequested;
    sliced.analysis.originalF0FailureReason.clear();

    sliced.volumeEnvelope = AutomationLane::sliceToLocalRange(source.volumeEnvelope, offsetSeconds, endSeconds);

    sliced.analysis.detectedKey = source.analysis.detectedKey;
    sliced.pitchShiftSettings = source.pitchShiftSettings;
    sliced.noteTopologyInitialized = source.noteTopologyInitialized;
    // 保留原始 native 共享描述；来源窗口按父权威起点加本次操作秒偏移更新，
    // canonical 原点只从绝对秒投影一次，不累加已取整的 canonical 增量。
    sliced.sourceWindow = source.sourceWindow;
    const double slicedStartSeconds = source.sourceWindow.sourceStartSeconds + offsetSeconds;
    sliced.sourceWindow.sourceStartSeconds = slicedStartSeconds;
    sliced.sourceWindow.sourceEndSeconds = slicedStartSeconds
        + static_cast<double>(std::max<int64_t>(0, clampedEnd - clampedStart)) / sampleRate;
    sliced.nativeSource = source.nativeSource;
    sliced.firstCanonicalSample = PlaybackSampleGeometry::projectToSample(
        slicedStartSeconds * TimeCoordinate::kRenderSampleRate);
    sliced.sampleRate = sampleRate;

    // 显式使用切片权威区间初始化时长网格。
    const double slicedDurationSeconds =
        static_cast<double>(std::max<int64_t>(0, clampedEnd - clampedStart)) / sampleRate;
    if (auto grid = TimeGridSnapshot::makeIdentity(slicedDurationSeconds))
        sliced.timeGrid = std::move(grid);

    return sliced;
}

} // namespace OpenTune
