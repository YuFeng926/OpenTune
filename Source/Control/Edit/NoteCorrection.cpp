// SPDX-License-Identifier: AGPL-3.0-or-later
#include "NoteCorrection.h"

#include "Content/ContentEditCommands.h"
#include "Content/EditableContentSnapshot.h"
#include "PluginProcessor.h"
#include "Utils/PitchUtils.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

/** isFullyCorrectedInRange() of PianoRollComponent.cpp: every frame of the range is covered by a correction
    segment, walked as playback walks them (forEachCorrectionF0Span; a segment whose f0Data is too short covers
    nothing). */
bool fullyCorrectedInRange(const PitchCurveSnapshot& curve, int startFrame, int endFrameExclusive)
{
    if (startFrame < 0 || endFrameExclusive > static_cast<int>(curve.size()) || endFrameExclusive <= startFrame)
        return false;
    bool fullyCorrected = true;
    curve.forEachCorrectionF0Span(startFrame, endFrameExclusive, [&](int, const float* values, int) {
        if (values == nullptr)
            fullyCorrected = false;
    });
    return fullyCorrected;
}

F0Timeline timelineOf(const PitchCurveSnapshot& curve)
{
    return F0Timeline{curve.getHopSize(), curve.getSampleRate(), static_cast<int>(curve.size())};
}

/** A segment cut to [startFrame, endFrameExclusive), or nullopt when nothing of it lies inside or its f0Data is
    too short (the clipping contract of extractSegmentsInRange in PianoRollComponent.cpp). */
std::optional<PitchCorrectionSegment> cutSegment(const PitchCorrectionSegment& segment, int startFrame,
                                                 int endFrameExclusive)
{
    const int clipStart = std::max(segment.startFrame, startFrame);
    const int clipEnd = std::min(segment.endFrame, endFrameExclusive);
    if (clipEnd <= clipStart)
        return std::nullopt;
    const int offset = clipStart - segment.startFrame;
    if (offset + (clipEnd - clipStart) > static_cast<int>(segment.f0Data.size()))
        return std::nullopt;
    PitchCorrectionSegment cut = segment;
    cut.startFrame = clipStart;
    cut.endFrame = clipEnd;
    cut.f0Data.assign(segment.f0Data.begin() + offset, segment.f0Data.begin() + offset + (clipEnd - clipStart));
    return cut;
}

} // namespace

F0FrameRange controlNotesFrameRange(const PitchCurveSnapshot& curve, const std::vector<Note>& notes,
                                    const std::vector<std::size_t>& positions)
{
    double rangeStart = 1e30;
    double rangeEnd = -1e30;
    for (const auto position : positions) {
        if (position >= notes.size())
            continue;
        rangeStart = std::min(rangeStart, notes[position].startTime);
        rangeEnd = std::max(rangeEnd, notes[position].endTime);
    }
    if (rangeEnd <= rangeStart)
        return {};
    return timelineOf(curve).rangeForTimes(rangeStart, rangeEnd);
}

std::optional<ControlCorrectionCommit> controlNoteBasedCorrection(const EditableContentSnapshot& snapshot,
                                                                  const std::vector<Note>& notes,
                                                                  F0FrameRange editRange)
{
    if (snapshot.pitchCurve == nullptr || editRange.isEmpty())
        return std::nullopt;
    const auto f0tl = timelineOf(*snapshot.pitchCurve);
    // buildNoteBasedCorrectionState(): a scratch copy of the curve, baked by applyCorrectionToRange() with the
    // content's pitch-shift ratio.
    const NoteGeneratorParams fallback;
    auto scratch = PitchCurve::fromSnapshot(snapshot.pitchCurve);
    const auto corrected =
        scratch->applyCorrectionToRange(notes, editRange.startFrame, editRange.endFrameExclusive,
                                        static_cast<float>(snapshot.pitchShiftSettings.getPitchRatio()),
                                        fallback.retuneSpeed, fallback.vibratoDepth, fallback.vibratoRate);
    // commitNoteBasedCorrection(): the affected range and the segments that overlap it.
    ControlCorrectionCommit commit;
    commit.affectedRange = PitchCurve::expandNoteBasedCorrectionRange(editRange.startFrame, editRange.endFrameExclusive,
                                                                      f0tl.endFrameExclusive());
    if (commit.affectedRange.isEmpty() || corrected == nullptr)
        return std::nullopt;
    commit.notes = notes;
    for (const auto& segment : corrected->getCorrectionSegments())
        if (segment.startFrame < commit.affectedRange.endFrameExclusive
            && segment.endFrame > commit.affectedRange.startFrame)
            commit.segments.push_back(segment);
    return commit;
}

ControlAutoSnapPlan controlPlanAutoSnap(const EditableContentSnapshot& snapshot,
                                        const std::optional<std::vector<std::size_t>>& targets,
                                        const ScaleSnapConfig& scaleSnap)
{
    ControlAutoSnapPlan plan;
    plan.notes = snapshot.notes;
    if (snapshot.pitchCurve == nullptr)
        return plan;
    const auto& curveSnap = *snapshot.pitchCurve;
    const auto f0tl = timelineOf(curveSnap);
    if (f0tl.isEmpty()) {
        plan.status = ControlAutoSnapPlan::Status::EmptyRange;
        return plan;
    }
    auto& notes = plan.notes;

    // The targets: the selected notes, or every note without a selection.
    const bool selectionActive = targets.has_value();
    std::vector<std::size_t> targetIndices;
    if (selectionActive) {
        for (const auto position : *targets)
            if (position < notes.size())
                targetIndices.push_back(position);
    } else {
        for (std::size_t i = 0; i < notes.size(); ++i)
            targetIndices.push_back(i);
    }
    if (targetIndices.empty())
        return plan;

    // The snap: the target's pitchOffset, from its base pitch only (the offset is not quantized). A note without
    // a base pitch is snapped as well: PitchUtils::freqToMidi() gives MIDI 0 for 0 Hz, so its offset becomes the
    // scale's quantization of MIDI 0, as the button sets it.
    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;
    bool anyPitchChanged = false;
    for (const auto index : targetIndices) {
        auto& note = notes[index];
        const float baseMidi = PitchUtils::freqToMidi(note.pitch);
        const float snappedOffset = scaleSnap.quantizeMidiToActiveScale(baseMidi) - baseMidi;
        if (std::abs(snappedOffset - note.pitchOffset) < 0.001f)
            continue;
        note.pitchOffset = snappedOffset;
        note.dirty = true;
        anyPitchChanged = true;
        dirtyStartTime = std::min(dirtyStartTime, note.startTime);
        dirtyEndTime = std::max(dirtyEndTime, note.endTime);
    }

    std::vector<Note> targetNotes;
    double targetMinStartTime = 1e30;
    double targetMaxEndTime = -1e30;
    for (const auto index : targetIndices) {
        targetMinStartTime = std::min(targetMinStartTime, notes[index].startTime);
        targetMaxEndTime = std::max(targetMaxEndTime, notes[index].endTime);
        targetNotes.push_back(notes[index]);
    }
    const auto notesRange = f0tl.rangeForTimes(targetMinStartTime, targetMaxEndTime);
    // NoChange: no target moves and the targets' frames are corrected without a gap.
    if (!anyPitchChanged && fullyCorrectedInRange(curveSnap, notesRange.startFrame, notesRange.endFrameExclusive))
        return plan;

    if (anyPitchChanged) {
        dirtyStartTime = std::min(dirtyStartTime, targetMinStartTime);
        dirtyEndTime = std::max(dirtyEndTime, targetMaxEndTime);
    } else {
        dirtyStartTime = targetMinStartTime;
        dirtyEndTime = targetMaxEndTime;
    }
    const auto editRange = f0tl.rangeForTimes(dirtyStartTime, dirtyEndTime);
    if (editRange.isEmpty()) {
        plan.status = ControlAutoSnapPlan::Status::EmptyRange;
        return plan;
    }

    // The correction of the target notes, with NoteGeneratorParams{} where the UI passes its knob positions.
    const NoteGeneratorParams fallback;
    auto scratchCurve = PitchCurve::fromSnapshot(snapshot.pitchCurve);
    const auto snap =
        scratchCurve->applyCorrectionToRange(targetNotes, editRange.startFrame, editRange.endFrameExclusive,
                                             static_cast<float>(snapshot.pitchShiftSettings.getPitchRatio()),
                                             fallback.retuneSpeed, fallback.vibratoDepth, fallback.vibratoRate);
    const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(
        editRange.startFrame, editRange.endFrameExclusive, f0tl.endFrameExclusive());

    // The segments of the affected range: without a selection the new segments cover all of it; with one, the
    // new segments cut to the target notes' frames and, on every other frame, the segments as they were. The two
    // never overlap (playback lets the first segment win).
    std::vector<PitchCorrectionSegment> segmentsInRange;
    if (!selectionActive) {
        for (const auto& seg : snap->getCorrectionSegments())
            if (seg.startFrame < affectedRange.endFrameExclusive && seg.endFrame > affectedRange.startFrame)
                segmentsInRange.push_back(seg);
    } else {
        // The union of the target notes' frames (frame i belongs to a note when i * secondsPerFrame lies in
        // [startTime, endTime), as PitchCurve.cpp assigns frames), sorted and merged.
        std::vector<F0FrameRange> noteFrames;
        for (const auto& note : targetNotes) {
            const int noteStart = f0tl.exclusiveFrameAt(note.startTime);
            const int noteEnd = f0tl.exclusiveFrameAt(note.endTime);
            if (noteEnd > noteStart)
                noteFrames.push_back(F0FrameRange{noteStart, noteEnd});
        }
        std::sort(noteFrames.begin(), noteFrames.end(),
                  [](const F0FrameRange& a, const F0FrameRange& b) { return a.startFrame < b.startFrame; });
        std::vector<F0FrameRange> targetFrames;
        for (const auto& frames : noteFrames) {
            if (targetFrames.empty() || targetFrames.back().endFrameExclusive < frames.startFrame)
                targetFrames.push_back(frames);
            else
                targetFrames.back().endFrameExclusive =
                    std::max(targetFrames.back().endFrameExclusive, frames.endFrameExclusive);
        }

        for (const auto& seg : snap->getCorrectionSegments()) {
            if (seg.endFrame <= affectedRange.startFrame || seg.startFrame >= affectedRange.endFrameExclusive)
                continue;
            for (const auto& frames : targetFrames)
                if (auto cut = cutSegment(seg, frames.startFrame, frames.endFrameExclusive))
                    segmentsInRange.push_back(std::move(*cut));
        }

        // The segments as they were on the frames that are no target's (interval difference).
        for (const auto& seg : curveSnap.getCorrectionSegments()) {
            if (seg.endFrame <= affectedRange.startFrame || seg.startFrame >= affectedRange.endFrameExclusive)
                continue;
            std::vector<F0FrameRange> spans{F0FrameRange{seg.startFrame, seg.endFrame}};
            for (const auto& frames : targetFrames) {
                std::vector<F0FrameRange> remainder;
                for (const auto& span : spans) {
                    if (frames.startFrame >= span.endFrameExclusive || frames.endFrameExclusive <= span.startFrame) {
                        remainder.push_back(span);
                        continue;
                    }
                    if (span.startFrame < frames.startFrame)
                        remainder.push_back(
                            F0FrameRange{span.startFrame, std::min(span.endFrameExclusive, frames.startFrame)});
                    if (frames.endFrameExclusive < span.endFrameExclusive)
                        remainder.push_back(
                            F0FrameRange{std::max(span.startFrame, frames.endFrameExclusive), span.endFrameExclusive});
                }
                spans = std::move(remainder);
                if (spans.empty())
                    break;
            }
            for (const auto& span : spans)
                if (auto kept = cutSegment(seg, span.startFrame, span.endFrameExclusive))
                    segmentsInRange.push_back(std::move(*kept));
        }
    }

    plan.status = ControlAutoSnapPlan::Status::Commit;
    plan.commit.affectedRange = affectedRange;
    plan.commit.notes = notes;
    plan.commit.segments = std::move(segmentsInRange);
    return plan;
}

bool controlCommitCorrection(OpenTuneAudioProcessor& processor, ContentEditTransaction& transaction,
                             const ContentKey& key, const ControlCorrectionCommit& commit,
                             const juce::String& description)
{
    const auto snapshot = processor.getContentSnapshot(key);
    if (snapshot == nullptr || snapshot->pitchCurve == nullptr || commit.affectedRange.isEmpty())
        return false;
    // PianoRollComponent::commitEditedContentNotesAndSegments(): the incoming notes and segments cut to the
    // affected range, then ContentEditCommands::commitNotesAndSegments(), the range-scoped commit.
    const auto f0tl = timelineOf(*snapshot->pitchCurve);
    const double rangeStartSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(commit.affectedRange.startFrame);
    const double rangeEndSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(commit.affectedRange.endFrameExclusive);
    auto notes = ContentEditTransaction::notesInRange(commit.notes, rangeStartSec, rangeEndSec);
    auto segments = ContentEditTransaction::segmentsInRange(commit.segments, commit.affectedRange.startFrame,
                                                            commit.affectedRange.endFrameExclusive);
    if (!transaction.captureBefore())
        return false;
    const auto committed = processor.getContentCommands()->commitNotesAndSegments(
        key, std::move(notes), std::move(segments),
        ContentEditRangeFrames{commit.affectedRange.startFrame, commit.affectedRange.endFrameExclusive});
    if (committed == nullptr)
        return false;
    return transaction.recordRange(description, commit.affectedRange);
}

} // namespace OpenTune
