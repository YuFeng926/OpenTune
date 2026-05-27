#include "ReferenceAutoAlign.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace OpenTune {

namespace {

constexpr double kF0FramesPerSecond = 100.0;

int timeToFrame(double timeSeconds)
{
    return static_cast<int>(std::round(timeSeconds * kF0FramesPerSecond));
}

double frameToTime(int frame)
{
    return static_cast<double>(frame) / kF0FramesPerSecond;
}

bool isReady(const ReferenceFeatureSet& features) noexcept
{
    return features.isReady();
}

double clampToDuration(double value, double duration) noexcept
{
    return juce::jlimit(0.0, duration, value);
}

double sourceToTimeline(const ReferenceClipProjection& clip,
                        const EffectiveTimeMap& timeMap,
                        double sourceSeconds)
{
    return clip.timelineStartSeconds + timeMap.tau(sourceSeconds);
}

double timelineToSource(const ReferenceClipProjection& clip,
                        const EffectiveTimeMap& timeMap,
                        double timelineSeconds)
{
    return timeMap.tauInverse(timelineSeconds - clip.timelineStartSeconds);
}

size_t findNearestUnmatchedNote(const std::vector<Note>& notes,
                                double sourceSeconds,
                                double affectedStartSeconds,
                                double affectedEndSeconds,
                                const std::set<size_t>& used)
{
    size_t bestIndex = static_cast<size_t>(-1);
    double bestDistance = std::numeric_limits<double>::max();

    for (size_t i = 0; i < notes.size(); ++i) {
        if (used.find(i) != used.end()) {
            continue;
        }

        const auto& note = notes[i];
        const double center = (note.startTime + note.endTime) * 0.5;
        if (center < affectedStartSeconds || center > affectedEndSeconds) {
            continue;
        }

        const double distance = std::abs(center - sourceSeconds);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

const ReferenceTimingAnchor* findNearestTimingAnchor(
    const std::vector<ReferenceTimingAnchor>& anchors,
    double sourceSeconds,
    ReferenceTimingAnchorKind preferredKind)
{
    const ReferenceTimingAnchor* bestAny = nullptr;
    const ReferenceTimingAnchor* bestKindMatch = nullptr;
    double bestAnyDistance = std::numeric_limits<double>::max();
    double bestKindDistance = std::numeric_limits<double>::max();

    for (const auto& anchor : anchors) {
        const double distance = std::abs(anchor.sourceSeconds - sourceSeconds);
        if (distance < bestAnyDistance) {
            bestAnyDistance = distance;
            bestAny = &anchor;
        }
        if (anchor.kind == preferredKind && distance < bestKindDistance) {
            bestKindDistance = distance;
            bestKindMatch = &anchor;
        }
    }

    return bestKindMatch != nullptr ? bestKindMatch : bestAny;
}

bool noteEqualsForPatch(const Note& a, const Note& b)
{
    return a.startTime == b.startTime
        && a.endTime == b.endTime
        && a.pitch == b.pitch
        && a.originalPitch == b.originalPitch
        && a.pitchOffset == b.pitchOffset
        && a.retuneSpeed == b.retuneSpeed
        && a.vibratoDepth == b.vibratoDepth
        && a.vibratoRate == b.vibratoRate
        && a.velocity == b.velocity
        && a.isVoiced == b.isVoiced;
}

bool segmentsEqualForPatch(const std::vector<CorrectedSegment>& a,
                           const std::vector<CorrectedSegment>& b)
{
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); ++i) {
        const auto& left = a[i];
        const auto& right = b[i];
        if (left.startFrame != right.startFrame
            || left.endFrame != right.endFrame
            || left.source != right.source
            || left.retuneSpeed != right.retuneSpeed
            || left.vibratoDepth != right.vibratoDepth
            || left.vibratoRate != right.vibratoRate
            || left.f0Data != right.f0Data) {
            return false;
        }
    }

    return true;
}

void fail(AlignmentPatch& patch, AlignmentPatch::ErrorCode error, const juce::String& message)
{
    patch.success = false;
    patch.error = error;
    patch.diagnostics = message;
    patch.notesAfter.clear();
    patch.correctedSegmentsAfter.clear();
    patch.timingIntents.clear();
    patch.pitchChanged = false;
    patch.timingChanged = false;
}

bool buildPitchPatch(const ReferenceAlignmentRequest& request,
                     const std::vector<Note>& pitchSeedNotes,
                     double affectedStartSeconds,
                     double affectedEndSeconds,
                     AlignmentPatch& patch)
{
    if (request.referenceFeatures.pitch.notes.empty() || pitchSeedNotes.empty()) {
        return false;
    }

    patch.notesAfter = pitchSeedNotes;

    std::vector<CorrectedSegment> segmentsAfter;
    segmentsAfter.reserve(request.targetSegmentsBefore.size() + request.referenceFeatures.pitch.notes.size());
    for (const auto& segment : request.targetSegmentsBefore) {
        const bool overlaps = segment.endFrame > patch.affectedStartFrame
                           && segment.startFrame < patch.affectedEndFrame;
        if (!overlaps) {
            segmentsAfter.push_back(segment);
        }
    }

    std::set<size_t> usedTargetNotes;
    bool changed = false;

    for (const auto& refNote : request.referenceFeatures.pitch.notes) {
        const double refCenterSource = (refNote.startTime + refNote.endTime) * 0.5;
        const double refCenterTimeline = sourceToTimeline(request.reference,
                                                          request.referenceTimeMap,
                                                          refCenterSource);
        if (refCenterTimeline < request.overlapStartTimelineSeconds
            || refCenterTimeline > request.overlapEndTimelineSeconds) {
            continue;
        }

        const double targetCenterSource = timelineToSource(request.target,
                                                           request.targetTimeMap,
                                                           refCenterTimeline);
        const size_t targetIndex = findNearestUnmatchedNote(patch.notesAfter,
                                                            targetCenterSource,
                                                            affectedStartSeconds,
                                                            affectedEndSeconds,
                                                            usedTargetNotes);
        if (targetIndex == static_cast<size_t>(-1)) {
            continue;
        }

        auto& targetNote = patch.notesAfter[targetIndex];
        Note corrected = targetNote;
        corrected.pitch = refNote.pitch;
        corrected.originalPitch = refNote.originalPitch;
        corrected.pitchOffset = refNote.pitchOffset;
        corrected.retuneSpeed = refNote.retuneSpeed;
        corrected.vibratoDepth = refNote.vibratoDepth;
        corrected.vibratoRate = refNote.vibratoRate;
        corrected.selected = false;
        corrected.dirty = true;

        if (!noteEqualsForPatch(targetNote, corrected)) {
            changed = true;
        }
        targetNote = corrected;
        usedTargetNotes.insert(targetIndex);

        CorrectedSegment segment;
        segment.startFrame = timeToFrame(targetNote.startTime);
        segment.endFrame = timeToFrame(targetNote.endTime);
        segment.source = CorrectedSegment::Source::NoteBased;
        segment.retuneSpeed = refNote.retuneSpeed;
        segment.vibratoDepth = refNote.vibratoDepth;
        segment.vibratoRate = refNote.vibratoRate;
        segmentsAfter.push_back(std::move(segment));
    }

    std::sort(segmentsAfter.begin(), segmentsAfter.end(),
              [](const CorrectedSegment& a, const CorrectedSegment& b) {
                  return a.startFrame < b.startFrame;
              });

    if (!segmentsEqualForPatch(segmentsAfter, request.targetSegmentsBefore)) {
        changed = true;
    }

    patch.correctedSegmentsAfter = std::move(segmentsAfter);
    patch.pitchChanged = changed;
    return changed;
}

bool buildTimingIntents(const ReferenceAlignmentRequest& request,
                        double affectedStartSeconds,
                        double affectedEndSeconds,
                        AlignmentPatch& patch,
                        juce::String& outError)
{
    const auto& targetAnchors = request.targetFeatures.timing.anchors;
    const auto& referenceAnchors = request.referenceFeatures.timing.anchors;
    if (targetAnchors.empty() || referenceAnchors.empty()) {
        return false;
    }

    const double duration = request.targetTimeMap.totalDurationSeconds();
    bool changed = false;

    for (const auto& targetAnchor : targetAnchors) {
        if (targetAnchor.sourceSeconds <= 0.0 || targetAnchor.sourceSeconds >= duration) {
            continue;
        }
        if (targetAnchor.sourceSeconds < affectedStartSeconds
            || targetAnchor.sourceSeconds > affectedEndSeconds) {
            continue;
        }

        const double targetTimeline = sourceToTimeline(request.target,
                                                       request.targetTimeMap,
                                                       targetAnchor.sourceSeconds);
        if (targetTimeline < request.overlapStartTimelineSeconds
            || targetTimeline > request.overlapEndTimelineSeconds) {
            continue;
        }

        const double referenceGuessSource = timelineToSource(request.reference,
                                                             request.referenceTimeMap,
                                                             targetTimeline);
        const auto* referenceAnchor = findNearestTimingAnchor(referenceAnchors,
                                                              referenceGuessSource,
                                                              targetAnchor.kind);
        if (referenceAnchor == nullptr) {
            continue;
        }

        const double referenceTimeline = sourceToTimeline(request.reference,
                                                          request.referenceTimeMap,
                                                          referenceAnchor->sourceSeconds);
        const double desiredOutput = referenceTimeline - request.target.timelineStartSeconds;
        if (desiredOutput <= 0.0 || desiredOutput >= duration) {
            outError = "AUTO Ref produced a timing intent outside target duration";
            return false;
        }

        const double currentOutput = request.targetTimeMap.tau(targetAnchor.sourceSeconds);
        if (std::abs(desiredOutput - currentOutput) < 1.0e-9) {
            continue;
        }

        TimeGridIntent intent;
        intent.targetSourceSeconds = targetAnchor.sourceSeconds;
        intent.desiredOutputSeconds = desiredOutput;
        intent.confidence = std::max(targetAnchor.confidence, referenceAnchor->confidence);
        patch.timingIntents.push_back(intent);
        changed = true;
    }

    patch.timingChanged = changed;
    return changed;
}

} // namespace

AlignmentPatch ReferenceAutoAlign::align(const ReferenceAlignmentRequest& request)
{
    AlignmentPatch patch;
    patch.targetMaterializationId = request.target.materializationId;

    if (request.target.materializationId == 0
        || request.reference.materializationId == 0
        || request.target.timelineEndSeconds <= request.target.timelineStartSeconds
        || request.reference.timelineEndSeconds <= request.reference.timelineStartSeconds) {
        fail(patch, AlignmentPatch::ErrorCode::InvalidRequest, "Invalid AUTO Ref request");
        return patch;
    }

    if (!isReady(request.targetFeatures)) {
        fail(patch, AlignmentPatch::ErrorCode::TargetAnalysisNotReady, "Target alignment features are not ready");
        return patch;
    }

    if (!isReady(request.referenceFeatures)) {
        fail(patch, AlignmentPatch::ErrorCode::ReferenceAnalysisNotReady, "Reference alignment features are not ready");
        return patch;
    }

    if (request.overlapEndTimelineSeconds <= request.overlapStartTimelineSeconds) {
        fail(patch, AlignmentPatch::ErrorCode::NoOverlap, "Target and reference placements do not overlap");
        return patch;
    }

    const double targetDuration = request.target.durationSeconds();
    const double referenceDuration = request.reference.durationSeconds();
    const double targetOutputDuration = request.targetTimeMap.totalDurationSeconds();
    const double referenceOutputDuration = request.referenceTimeMap.totalDurationSeconds();
    if (targetDuration <= 0.0 || referenceDuration <= 0.0
        || targetOutputDuration <= 0.0 || referenceOutputDuration <= 0.0) {
        fail(patch, AlignmentPatch::ErrorCode::InvalidRequest, "AUTO Ref requires positive clip durations");
        return patch;
    }

    const double overlapTargetOutputStart = clampToDuration(
        request.overlapStartTimelineSeconds - request.target.timelineStartSeconds,
        targetOutputDuration);
    const double overlapTargetOutputEnd = clampToDuration(
        request.overlapEndTimelineSeconds - request.target.timelineStartSeconds,
        targetOutputDuration);
    const double affectedStartSeconds = request.targetTimeMap.tauInverse(overlapTargetOutputStart);
    const double affectedEndSeconds = request.targetTimeMap.tauInverse(overlapTargetOutputEnd);

    patch.affectedStartFrame = timeToFrame(std::min(affectedStartSeconds, affectedEndSeconds));
    patch.affectedEndFrame = timeToFrame(std::max(affectedStartSeconds, affectedEndSeconds));
    if (patch.affectedEndFrame <= patch.affectedStartFrame) {
        fail(patch, AlignmentPatch::ErrorCode::NoOverlap, "AUTO Ref overlap maps to an empty target source range");
        return patch;
    }

    const auto& pitchSeedNotes = request.targetNotesBefore.empty()
        ? request.targetFeatures.pitch.notes
        : request.targetNotesBefore;
    patch.notesAfter = pitchSeedNotes;
    patch.correctedSegmentsAfter = request.targetSegmentsBefore;

    const bool hasPitchFeatures = request.referenceFeatures.hasPitchNotes() && !pitchSeedNotes.empty();
    const bool hasTimeFeatures = request.referenceFeatures.hasTimingAnchors()
                              && request.targetFeatures.hasTimingAnchors();
    if (!hasPitchFeatures && !hasTimeFeatures) {
        fail(patch, AlignmentPatch::ErrorCode::InsufficientFeatures, "AUTO Ref has neither pitch nor time features");
        return patch;
    }
    if (hasTimeFeatures && request.targetTimeGridBefore == nullptr) {
        fail(patch, AlignmentPatch::ErrorCode::TimeGridInvalid, "AUTO Ref timing path requires a target TimeGrid");
        return patch;
    }

    const bool pitchAttempted = hasPitchFeatures
                             && buildPitchPatch(request,
                                                pitchSeedNotes,
                                                frameToTime(patch.affectedStartFrame),
                                                frameToTime(patch.affectedEndFrame),
                                                patch);

    juce::String timeError;
    const bool timeAttempted = hasTimeFeatures
                            && buildTimingIntents(request,
                                                  frameToTime(patch.affectedStartFrame),
                                                  frameToTime(patch.affectedEndFrame),
                                                  patch,
                                                  timeError);
    if (hasTimeFeatures && !timeError.isEmpty()) {
        fail(patch, AlignmentPatch::ErrorCode::TimeGridInvalid, timeError);
        return patch;
    }

    if (!pitchAttempted && !timeAttempted) {
        fail(patch, AlignmentPatch::ErrorCode::NoMutation, "AUTO Ref produced no materialization changes");
        return patch;
    }

    patch.success = true;
    patch.error = AlignmentPatch::ErrorCode::None;
    patch.pitchChanged = pitchAttempted;
    patch.timingChanged = timeAttempted;
    return patch;
}

} // namespace OpenTune
