#pragma once

/**
 * Pure range-scoped geometry for content note/segment patches.
 *
 * Operates only on notes / PitchCorrectionSegment / frame or second ranges:
 * it knows nothing about ContentKey, DomainKind, owners, revisions, render or
 * Undo. The processor remains the orchestrator that reads the snapshot, calls
 * these helpers, dispatches to the owner by DomainKind and completes the local
 * mutation.
 */

#include "../Utils/Note.h"
#include "../Utils/PitchCurve.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace OpenTune {

// Notes overlapping the affected range in the seconds domain: a note is kept
// when [startTime, endTime) strictly intersects [rangeStartSeconds, rangeEndSeconds).
inline std::vector<Note> filterNotesToRange(const std::vector<Note>& notes,
                                            double rangeStartSeconds,
                                            double rangeEndSeconds)
{
    std::vector<Note> filtered;
    for (const auto& note : notes) {
        if (note.endTime > rangeStartSeconds && note.startTime < rangeEndSeconds)
            filtered.push_back(note);
    }
    return filtered;
}

// 唯一段切片原语：f0Data 恒按 [startFrame, endFrame) 裁切；baseF0Data 在覆盖到
// 切片末帧时按同偏移裁切，保证切片后两数组仍与帧区间对齐（调用方传入的帧区间
// 必须落在段自身的有效数据范围内）。
inline PitchCorrectionSegment sliceCorrectionSegment(const PitchCorrectionSegment& segment,
                                                     int startFrame, int endFrame)
{
    PitchCorrectionSegment piece;
    piece.startFrame = startFrame;
    piece.endFrame = endFrame;
    piece.parameterSnapshot = segment.parameterSnapshot;
    piece.source = segment.source;
    const int offset = startFrame - segment.startFrame;
    const int end = endFrame - segment.startFrame;
    piece.f0Data.assign(segment.f0Data.begin() + offset, segment.f0Data.begin() + end);
    if (static_cast<size_t>(end) <= segment.baseF0Data.size())
        piece.baseF0Data.assign(segment.baseF0Data.begin() + offset, segment.baseF0Data.begin() + end);
    return piece;
}

// Filters incoming segments to the affected frame range and clips boundary
// crossings through the shared slice primitive, so each clipped segment keeps
// the f0Data/baseF0Data samples that belong to its retained frame span.
// Segments fully outside the range or empty after clipping are dropped.
inline std::vector<PitchCorrectionSegment> clipSegmentsToFrameRange(
    const std::vector<PitchCorrectionSegment>& segments,
    int rangeStartFrame,
    int rangeEndFrameExclusive)
{
    std::vector<PitchCorrectionSegment> filtered;
    for (const auto& seg : segments) {
        if (seg.endFrame <= rangeStartFrame || seg.startFrame >= rangeEndFrameExclusive)
            continue;

        const int clipStart = std::max(seg.startFrame, rangeStartFrame);
        const int clipEnd = std::min(seg.endFrame, rangeEndFrameExclusive);
        if (clipEnd <= clipStart)
            continue;

        filtered.push_back(sliceCorrectionSegment(seg, clipStart, clipEnd));
    }
    return filtered;
}

// 扩展帧区间使其完整覆盖所有与之相交的手工段（手绘/线锚）：范围提交与撤销
// 重放以扩展后的范围替换段，手工段整体穿过提交/撤销机器，不被范围边界切分。
inline F0FrameRange expandRangeToCoverWholeManualSegments(
    const std::vector<PitchCorrectionSegment>& segments, F0FrameRange range)
{
    for (const auto& segment : segments) {
        if (segment.endFrame <= range.startFrame || segment.startFrame >= range.endFrameExclusive)
            continue;
        if (isManualCorrectionSource(segment.source)) {
            range.startFrame = std::min(range.startFrame, segment.startFrame);
            range.endFrameExclusive = std::max(range.endFrameExclusive, segment.endFrame);
        }
    }
    return range;
}

// Replaces the segments inside the affected frame range with newSegments:
// segments entirely outside the range are kept as-is, boundary-crossing old
// segments are split through the shared slice primitive so their outside parts
// are preserved (f0Data/baseF0Data offsets maintained), then newSegments are
// inserted and the result is sorted by startFrame.
inline std::vector<PitchCorrectionSegment> replaceSegmentsInFrameRange(
    const std::vector<PitchCorrectionSegment>& existingSegments,
    const std::vector<PitchCorrectionSegment>& newSegments,
    int rangeStartFrame,
    int rangeEndFrameExclusive)
{
    std::vector<PitchCorrectionSegment> mergedSegments;
    mergedSegments.reserve(existingSegments.size() + newSegments.size());

    for (const auto& seg : existingSegments) {
        // Segment entirely outside range -> keep as-is
        if (seg.endFrame <= rangeStartFrame || seg.startFrame >= rangeEndFrameExclusive) {
            mergedSegments.push_back(seg);
            continue;
        }

        // Segment crosses range boundary -> split and preserve outside parts
        // Left part: segment starts before range
        if (seg.startFrame < rangeStartFrame)
            mergedSegments.push_back(sliceCorrectionSegment(seg, seg.startFrame, rangeStartFrame));

        // Right part: segment ends after range
        if (seg.endFrame > rangeEndFrameExclusive)
            mergedSegments.push_back(sliceCorrectionSegment(seg, rangeEndFrameExclusive, seg.endFrame));
    }

    // Callers already filtered the new segments to range-overlapping only.
    mergedSegments.insert(mergedSegments.end(), newSegments.begin(), newSegments.end());

    std::sort(mergedSegments.begin(), mergedSegments.end(),
              [](const PitchCorrectionSegment& a, const PitchCorrectionSegment& b) {
                  return a.startFrame < b.startFrame;
              });
    return mergedSegments;
}

} // namespace OpenTune
