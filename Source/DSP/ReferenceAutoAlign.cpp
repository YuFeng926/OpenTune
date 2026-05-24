#include "ReferenceAutoAlign.h"

#include "../Utils/MaterializationState.h"

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

bool isReady(const AlignmentFeatures& features) noexcept
{
    return features.state == F0ExtractionState::Ready;
}

double clampToDuration(double value, double duration) noexcept
{
    return juce::jlimit(0.0, duration, value);
}

double sourceToTimeline(const ReferenceClipProjection& clip, double sourceSeconds)
{
    return clip.timelineStartSeconds + clip.timeGrid->tauForward(sourceSeconds);
}

double timelineToSource(const ReferenceClipProjection& clip, double timelineSeconds)
{
    return clip.timeGrid->tauInverse(timelineSeconds - clip.timelineStartSeconds);
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

const MaterializationStore::DerivedAnalysis::TemporalEvent* findNearestTemporalEvent(
    const std::vector<MaterializationStore::DerivedAnalysis::TemporalEvent>& events,
    double sourceSeconds)
{
    const MaterializationStore::DerivedAnalysis::TemporalEvent* best = nullptr;
    double bestDistance = std::numeric_limits<double>::max();

    for (const auto& event : events) {
        const double distance = std::abs(event.sourceSeconds - sourceSeconds);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &event;
        }
    }

    return best;
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
                     double affectedStartSeconds,
                     double affectedEndSeconds,
                     AlignmentPatch& patch)
{
    if (request.referenceFeatures.basicDerivedNotes.empty()
        || request.targetNotesBefore.empty()) {
        return false;
    }

    patch.notesAfter = request.targetNotesBefore;

    std::vector<CorrectedSegment> segmentsAfter;
    segmentsAfter.reserve(request.targetSegmentsBefore.size() + request.referenceFeatures.basicDerivedNotes.size());
    for (const auto& segment : request.targetSegmentsBefore) {
        const bool overlaps = segment.endFrame > patch.affectedStartFrame
                           && segment.startFrame < patch.affectedEndFrame;
        if (!overlaps) {
            segmentsAfter.push_back(segment);
        }
    }

    std::set<size_t> usedTargetNotes;
    bool changed = false;

    for (const auto& refNote : request.referenceFeatures.basicDerivedNotes) {
        const double refCenterSource = (refNote.startTime + refNote.endTime) * 0.5;
        const double refCenterTimeline = sourceToTimeline(request.reference, refCenterSource);
        if (refCenterTimeline < request.overlapStartTimelineSeconds
            || refCenterTimeline > request.overlapEndTimelineSeconds) {
            continue;
        }

        const double targetCenterSource = timelineToSource(request.target, refCenterTimeline);
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
    const auto& targetEvents = request.targetFeatures.temporalEvents;
    const auto& referenceEvents = request.referenceFeatures.temporalEvents;
    if (targetEvents.size() < 2 || referenceEvents.size() < 2) {
        return false;
    }

    const double duration = request.target.timeGrid->totalDurationSeconds();
    bool changed = false;

    for (const auto& targetEvent : targetEvents) {
        if (targetEvent.sourceSeconds <= 0.0 || targetEvent.sourceSeconds >= duration) {
            continue;
        }
        if (targetEvent.sourceSeconds < affectedStartSeconds
            || targetEvent.sourceSeconds > affectedEndSeconds) {
            continue;
        }

        const double targetTimeline = sourceToTimeline(request.target, targetEvent.sourceSeconds);
        if (targetTimeline < request.overlapStartTimelineSeconds
            || targetTimeline > request.overlapEndTimelineSeconds) {
            continue;
        }

        const double referenceGuessSource = timelineToSource(request.reference, targetTimeline);
        const auto* referenceEvent = findNearestTemporalEvent(referenceEvents, referenceGuessSource);
        if (referenceEvent == nullptr) {
            continue;
        }

        const double referenceTimeline = sourceToTimeline(request.reference, referenceEvent->sourceSeconds);
        const double desiredOutput = referenceTimeline - request.target.timelineStartSeconds;
        if (desiredOutput <= 0.0 || desiredOutput >= duration) {
            outError = "AUTO Ref produced a timing intent outside target duration";
            return false;
        }
        const double currentOutput = request.target.timeGrid->tauForward(targetEvent.sourceSeconds);
        if (std::abs(desiredOutput - currentOutput) < 1.0e-9) {
            continue;
        }

        TimeGridIntent intent;
        intent.targetSourceSeconds = targetEvent.sourceSeconds;
        intent.desiredOutputSeconds = desiredOutput;
        intent.confidence = std::max(targetEvent.confidence, referenceEvent->confidence);
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
    if (targetDuration <= 0.0 || referenceDuration <= 0.0) {
        fail(patch, AlignmentPatch::ErrorCode::InvalidRequest, "AUTO Ref requires positive clip durations");
        return patch;
    }

    if (request.target.timeGrid == nullptr || request.reference.timeGrid == nullptr) {
        fail(patch, AlignmentPatch::ErrorCode::TimeGridInvalid, "AUTO Ref requires target and reference TimeGrid");
        return patch;
    }

    const double overlapTargetOutputStart = clampToDuration(
        request.overlapStartTimelineSeconds - request.target.timelineStartSeconds,
        request.target.timeGrid->totalDurationSeconds());
    const double overlapTargetOutputEnd = clampToDuration(
        request.overlapEndTimelineSeconds - request.target.timelineStartSeconds,
        request.target.timeGrid->totalDurationSeconds());
    const double affectedStartSeconds = request.target.timeGrid->tauInverse(overlapTargetOutputStart);
    const double affectedEndSeconds = request.target.timeGrid->tauInverse(overlapTargetOutputEnd);

    patch.affectedStartFrame = timeToFrame(std::min(affectedStartSeconds, affectedEndSeconds));
    patch.affectedEndFrame = timeToFrame(std::max(affectedStartSeconds, affectedEndSeconds));
    if (patch.affectedEndFrame <= patch.affectedStartFrame) {
        fail(patch, AlignmentPatch::ErrorCode::NoOverlap, "AUTO Ref overlap maps to an empty target source range");
        return patch;
    }

    patch.notesAfter = request.targetNotesBefore;
    patch.correctedSegmentsAfter = request.targetSegmentsBefore;

    const bool hasPitchFeatures = !request.referenceFeatures.basicDerivedNotes.empty()
                               && !request.targetNotesBefore.empty();
    const bool hasTimeFeatures = request.referenceFeatures.temporalEvents.size() >= 2
                              && request.targetFeatures.temporalEvents.size() >= 2;
    if (!hasPitchFeatures && !hasTimeFeatures) {
        fail(patch, AlignmentPatch::ErrorCode::InsufficientFeatures, "AUTO Ref has neither pitch nor time features");
        return patch;
    }

    const bool pitchAttempted = hasPitchFeatures
                             && buildPitchPatch(request,
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
