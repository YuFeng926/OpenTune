#include "ReferenceAutoAlign.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace OpenTune {

namespace {

double sourceToTimeline(const ReferenceClipProjection& clip,
                        const EffectiveTimeMap& timeMap,
                        double sourceSeconds)
{
    return clip.timelineStartSeconds
        + timeMap.tau(sourceSeconds)
        - timeMap.tau(clip.sourceStartSeconds);
}

AlignmentPatch makeNoMutation(AlignmentPatch patch)
{
    patch.success = false;
    patch.error = AlignmentPatch::ErrorCode::NoMutation;
    patch.pitchChanged = false;
    patch.diagnostics = "AUTO Ref produced no pitch changes";
    return patch;
}

struct ProjectedNote {
    size_t noteIndex{0};
    double timelineStart{0.0};
    double timelineEnd{0.0};
};

std::vector<ProjectedNote> projectNotes(const ReferenceAlignmentRequest& request,
                                        const ReferenceClipProjection& clip,
                                        const EffectiveTimeMap& timeMap,
                                        const std::vector<Note>& notes,
                                        double overlapStart,
                                        double overlapEnd)
{
    std::vector<ProjectedNote> projected;
    projected.reserve(notes.size());

    for (size_t noteIndex = 0; noteIndex < notes.size(); ++noteIndex) {
        const auto& note = notes[noteIndex];
        const double visibleSourceStart = std::max(note.startTime, clip.sourceStartSeconds);
        const double visibleSourceEnd = std::min(note.endTime, clip.sourceEndSeconds);
        if (visibleSourceEnd <= visibleSourceStart)
            continue;
        const double start = sourceToTimeline(clip, timeMap, visibleSourceStart);
        const double end = sourceToTimeline(clip, timeMap, visibleSourceEnd);
        const double tStart = std::min(start, end);
        const double tEnd = std::max(start, end);
        if (tEnd <= overlapStart || tStart >= overlapEnd)
            continue;
        projected.push_back({noteIndex, tStart, tEnd});
    }

    return projected;
}

} // namespace

AlignmentPatch ReferenceAutoAlign::align(const ReferenceAlignmentRequest& request)
{
    AlignmentPatch patch;
    patch.targetContentKey = request.target.contentKey;

    if (!request.target.contentKey.isValid()
        || !request.reference.contentKey.isValid()
        || request.target.timelineEndSeconds <= request.target.timelineStartSeconds
        || request.reference.timelineEndSeconds <= request.reference.timelineStartSeconds
        || request.target.sourceEndSeconds <= request.target.sourceStartSeconds
        || request.reference.sourceEndSeconds <= request.reference.sourceStartSeconds) {
        patch.error = AlignmentPatch::ErrorCode::InvalidRequest;
        patch.diagnostics = "Invalid AUTO Ref request";
        return patch;
    }

    if (!request.targetFeatures.isReady()) {
        patch.error = AlignmentPatch::ErrorCode::TargetAnalysisNotReady;
        patch.diagnostics = "Target pitch features are not ready";
        return patch;
    }

    if (!request.referenceFeatures.isReady()) {
        patch.error = AlignmentPatch::ErrorCode::ReferenceAnalysisNotReady;
        patch.diagnostics = "Reference pitch features are not ready";
        return patch;
    }

    if (request.overlapEndTimelineSeconds <= request.overlapStartTimelineSeconds) {
        patch.error = AlignmentPatch::ErrorCode::NoOverlap;
        patch.diagnostics = "Target and reference placements do not overlap";
        return patch;
    }

    const double targetDuration = request.target.durationSeconds();
    const double referenceDuration = request.reference.durationSeconds();
    if (targetDuration <= 0.0 || referenceDuration <= 0.0) {
        patch.error = AlignmentPatch::ErrorCode::InvalidRequest;
        patch.diagnostics = "AUTO Ref requires positive clip durations";
        return patch;
    }

    const double overlapStart = request.overlapStartTimelineSeconds - request.target.timelineStartSeconds;
    const double overlapEnd = request.overlapEndTimelineSeconds - request.target.timelineStartSeconds;

    const double targetOutputStart = request.targetTimeMap.tau(request.target.sourceStartSeconds);
    const double affectedStartSeconds = request.targetTimeMap.tauInverse(targetOutputStart + overlapStart);
    const double affectedEndSeconds = request.targetTimeMap.tauInverse(targetOutputStart + overlapEnd);
    patch.affectedSourceStartSeconds = std::min(affectedStartSeconds, affectedEndSeconds);
    patch.affectedSourceEndSeconds = std::max(affectedStartSeconds, affectedEndSeconds);

    if (patch.affectedSourceEndSeconds <= patch.affectedSourceStartSeconds) {
        patch.error = AlignmentPatch::ErrorCode::NoOverlap;
        patch.diagnostics = "AUTO Ref overlap maps to an empty target source range";
        return patch;
    }

    const auto& seedNotes = request.targetNotesBefore.empty()
        ? request.targetFeatures.pitch.notes
        : request.targetNotesBefore;

    if (!request.referenceFeatures.hasPitchNotes() || seedNotes.empty()) {
        patch.error = AlignmentPatch::ErrorCode::InsufficientFeatures;
        patch.diagnostics = "AUTO Ref has no pitch features";
        return patch;
    }

    patch.notesAfter = seedNotes;
    auto& notesAfter = patch.notesAfter;

    const auto projectedTargets = projectNotes(request,
        request.target, request.targetTimeMap, notesAfter,
        request.overlapStartTimelineSeconds, request.overlapEndTimelineSeconds);

    const auto projectedReferences = projectNotes(request,
        request.reference, request.referenceTimeMap,
        request.referenceFeatures.pitch.notes,
        request.overlapStartTimelineSeconds, request.overlapEndTimelineSeconds);

    if (projectedTargets.empty() || projectedReferences.empty()) {
        return makeNoMutation(std::move(patch));
    }

    size_t targetCursor = 0;
    bool changed = false;

    for (const auto& pRef : projectedReferences) {
        while (targetCursor < projectedTargets.size()
               && projectedTargets[targetCursor].timelineEnd <= pRef.timelineStart) {
            ++targetCursor;
        }

        size_t bestTarget = static_cast<size_t>(-1);
        double bestOverlap = 0.0;
        for (size_t cand = targetCursor;
             cand < projectedTargets.size()
                 && projectedTargets[cand].timelineStart < pRef.timelineEnd;
             ++cand) {
            const double overlap = std::min(projectedTargets[cand].timelineEnd, pRef.timelineEnd)
                                  - std::max(projectedTargets[cand].timelineStart, pRef.timelineStart);
            if (overlap > bestOverlap) {
                bestOverlap = overlap;
                bestTarget = cand;
            }
        }

        if (bestTarget == static_cast<size_t>(-1))
            continue;

        auto& targetNote = notesAfter[projectedTargets[bestTarget].noteIndex];
        const auto& referenceNote = request.referenceFeatures.pitch.notes[pRef.noteIndex];
        const float referencePitch = referenceNote.getAdjustedPitch();
        if (referencePitch <= 0.0f) {
            targetCursor = bestTarget + 1;
            continue;
        }

        if (targetNote.pitch != referencePitch || targetNote.pitchOffset != 0.0f) {
            targetNote.pitch = referencePitch;
            targetNote.pitchOffset = 0.0f;
            targetNote.dirty = true;
            changed = true;
        }
        targetCursor = bestTarget + 1;
    }

    if (!changed)
        return makeNoMutation(std::move(patch));

    patch.success = true;
    patch.error = AlignmentPatch::ErrorCode::None;
    patch.pitchChanged = true;
    return patch;
}

} // namespace OpenTune
