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

// Filters incoming segments to the affected frame range and clips boundary
// crossings, maintaining the f0Data offset so each clipped segment keeps the
// samples that belong to its retained frame span. Segments fully outside the
// range, empty after clipping, or inconsistent with f0Data are dropped.
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

        PitchCorrectionSegment clipped = seg;
        const int startOffset = clipStart - seg.startFrame;
        const int clipLen = clipEnd - clipStart;
        if (startOffset >= 0 && clipLen > 0
            && startOffset + clipLen <= static_cast<int>(seg.f0Data.size())) {
            clipped.startFrame = clipStart;
            clipped.endFrame = clipEnd;
            clipped.f0Data.assign(seg.f0Data.begin() + startOffset,
                                  seg.f0Data.begin() + startOffset + clipLen);
            filtered.push_back(std::move(clipped));
        }
    }
    return filtered;
}

// Replaces the segments inside the affected frame range with newSegments:
// segments entirely outside the range are kept as-is, boundary-crossing old
// segments are split so their outside parts are preserved (f0Data offsets
// maintained), then newSegments are inserted and the result is sorted by
// startFrame.
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
        if (seg.startFrame < rangeStartFrame) {
            PitchCorrectionSegment left = seg;
            left.endFrame = rangeStartFrame;
            const int leftLen = left.endFrame - left.startFrame;
            if (leftLen > 0 && leftLen <= static_cast<int>(seg.f0Data.size())) {
                left.f0Data.assign(seg.f0Data.begin(), seg.f0Data.begin() + leftLen);
                mergedSegments.push_back(std::move(left));
            }
        }

        // Right part: segment ends after range
        if (seg.endFrame > rangeEndFrameExclusive) {
            PitchCorrectionSegment right = seg;
            right.startFrame = rangeEndFrameExclusive;
            const int offset = right.startFrame - seg.startFrame;
            const int rightLen = right.endFrame - right.startFrame;
            if (offset >= 0 && rightLen > 0
                && offset + rightLen <= static_cast<int>(seg.f0Data.size())) {
                right.f0Data.assign(seg.f0Data.begin() + offset,
                                    seg.f0Data.begin() + offset + rightLen);
                mergedSegments.push_back(std::move(right));
            }
        }
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
