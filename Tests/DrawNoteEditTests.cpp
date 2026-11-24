#include "Utils/DrawNoteEdit.h"
#include "Utils/PitchUtils.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace OpenTune;

Note makeNote(double start, double end, float pitch)
{
    Note note;
    note.startTime = start;
    note.endTime = end;
    note.pitch = pitch;
    note.originalPitch = pitch;
    note.pitchOffset = 0.25f;
    note.retuneSpeed = 0.42f;
    note.vibratoDepth = 0.11f;
    note.vibratoRate = 5.3f;
    note.pitchDriftScale = 0.8f;
    note.dirty = true;
    return note;
}

Note makeDrawnNote(double start, double end, float pitch)
{
    Note note;
    note.startTime = start;
    note.endTime = end;
    note.pitch = pitch;
    note.originalPitch = pitch;
    note.retuneSpeed = 0.42f;
    note.vibratoDepth = 0.11f;
    note.vibratoRate = 5.3f;
    note.dirty = true;
    return note;
}

bool near(float left, float right)
{
    return std::abs(left - right) < 0.001f;
}

bool sameNote(const Note& left, const Note& right)
{
    return left.startTime == right.startTime
        && left.endTime == right.endTime
        && near(left.pitch, right.pitch)
        && near(left.originalPitch, right.originalPitch)
        && near(left.pitchOffset, right.pitchOffset)
        && near(left.retuneSpeed, right.retuneSpeed)
        && near(left.vibratoDepth, right.vibratoDepth)
        && near(left.vibratoRate, right.vibratoRate)
        && near(left.pitchDriftScale, right.pitchDriftScale)
        && near(left.outputGainDb, right.outputGainDb)
        && left.isVoiced == right.isVoiced
        && left.dirty == right.dirty
        && left.eq == right.eq;
}

bool sameNotes(const std::vector<Note>& left, const std::vector<Note>& right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!sameNote(left[index], right[index]))
            return false;
    }
    return true;
}

EditableContentSnapshot makeSnapshot(std::vector<Note> notes = {})
{
    EditableContentSnapshot snapshot;
    snapshot.notesRevision = 12;
    snapshot.notes = std::move(notes);
    return snapshot;
}

DrawNoteEditInput makeInput(double start, double end, float pitch = 440.0f)
{
    return { start, end, pitch, 0.42f, 0.11f, 5.3f };
}

bool checkNoOverlapAndNoteOnlyPatch()
{
    auto snapshot = makeSnapshot({ makeNote(0.0, 1.0, 220.0f), makeNote(4.0, 5.0, 246.0f) });
    const auto plan = planDrawNoteEdit(snapshot, makeInput(2.0, 3.0), snapshot.notesRevision);
    const std::vector<Note> expected = {
        makeNote(0.0, 1.0, 220.0f),
        makeDrawnNote(2.0, 3.0, 440.0f),
        makeNote(4.0, 5.0, 246.0f)
    };
    return plan.status == DrawNotePlanStatus::Ready
        && !plan.pitchCurveEdited
        && sameNotes(plan.afterNotes, expected)
        && plan.beforePatch.affectedRange.startSeconds == 2.0
        && plan.beforePatch.affectedRange.endSeconds == 3.0
        && plan.beforePatch.afterNotesInRange.empty()
        && sameNotes(plan.afterPatch.afterNotesInRange, { expected[1] });
}

bool checkNoChangeWhenComparedFieldsMatch()
{
    auto existing = makeDrawnNote(2.0, 3.0, 440.0f);
    existing.originalPitch = 220.0f;
    existing.dirty = false;
    const auto snapshot = makeSnapshot({ existing });
    const auto plan = planDrawNoteEdit(snapshot, makeInput(2.0, 3.0), snapshot.notesRevision);

    return plan.status == DrawNotePlanStatus::NoChange
        && plan.afterNotes.size() == 1
        && plan.afterNotes[0].startTime == 2.0
        && plan.afterNotes[0].endTime == 3.0
        && plan.beforePatch.affectedRange.startSeconds == 0.0
        && plan.beforePatch.affectedRange.endSeconds == 0.0
        && plan.beforePatch.afterNotesInRange.empty()
        && plan.afterPatch.affectedRange.startSeconds == 0.0
        && plan.afterPatch.affectedRange.endSeconds == 0.0
        && plan.afterPatch.afterNotesInRange.empty();
}

bool checkOverlapSplitsAndBoundaryContact()
{
    const auto leftPlan = planDrawNoteEdit(
        makeSnapshot({ makeNote(0.0, 3.0, 220.0f) }), makeInput(2.0, 4.0), 12);
    if (!sameNotes(leftPlan.afterNotes, {
            makeNote(0.0, 2.0, 220.0f), makeDrawnNote(2.0, 4.0, 440.0f) }))
        return false;

    const auto rightPlan = planDrawNoteEdit(
        makeSnapshot({ makeNote(3.0, 6.0, 220.0f) }), makeInput(2.0, 4.0), 12);
    if (!sameNotes(rightPlan.afterNotes, {
            makeDrawnNote(2.0, 4.0, 440.0f), makeNote(4.0, 6.0, 220.0f) }))
        return false;

    const auto bothPlan = planDrawNoteEdit(
        makeSnapshot({ makeNote(0.0, 8.0, 220.0f) }), makeInput(2.0, 6.0), 12);
    if (!sameNotes(bothPlan.afterNotes, {
            makeNote(0.0, 2.0, 220.0f),
            makeDrawnNote(2.0, 6.0, 440.0f),
            makeNote(6.0, 8.0, 220.0f) }))
        return false;

    const auto touchingPlan = planDrawNoteEdit(
        makeSnapshot({ makeNote(0.0, 2.0, 220.0f) }), makeInput(2.0, 4.0), 12);
    return sameNotes(touchingPlan.afterNotes, {
            makeNote(0.0, 2.0, 220.0f), makeDrawnNote(2.0, 4.0, 440.0f) })
        && touchingPlan.beforePatch.afterNotesInRange.empty();
}

bool checkPipFallbackAndQuantization()
{
    auto fallback = planDrawNoteEdit(makeSnapshot(), makeInput(1.0, 2.0, 445.0f), 12);
    if (!near(fallback.insertedNote.pitch, 445.0f)
        || !near(fallback.insertedNote.originalPitch, 445.0f)
        || fallback.insertedNote.pitchOffset != 0.0f)
        return false;

    auto unvoicedSnapshot = makeSnapshot();
    unvoicedSnapshot.pitchCurve = std::make_shared<const PitchCurveSnapshot>(
        std::vector<float>(4, 0.0f), std::vector<float>(4, 0.0f),
        std::vector<PitchCorrectionSegment>{}, 1, 1.0);
    const auto unvoiced = planDrawNoteEdit(
        unvoicedSnapshot, makeInput(1.0, 2.0, 445.0f), 12);
    if (!near(unvoiced.insertedNote.pitch, 445.0f)
        || !near(unvoiced.insertedNote.originalPitch, 445.0f)
        || unvoiced.insertedNote.pitchOffset != 0.0f)
        return false;

    auto snapshot = makeSnapshot();
    snapshot.pitchShiftSettings.semitone = 12;
    snapshot.pitchCurve = std::make_shared<const PitchCurveSnapshot>(
        std::vector<float>{ 0.0f, 440.0f, 440.0f, 660.0f, 880.0f },
        std::vector<float>(5, 0.0f),
        std::vector<PitchCorrectionSegment>{}, 1, 1.0);
    const auto plan = planDrawNoteEdit(snapshot, makeInput(1.0, 5.0, 1320.0f), 12);
    const float targetMidi = PitchUtils::freqToMidi(1320.0f);
    return near(plan.insertedNote.originalPitch, 1320.0f)
        && near(plan.insertedNote.pitch, PitchUtils::midiToFreq(88.0f))
        && near(plan.insertedNote.pitchOffset, targetMidi - 88.0f)
        && near(plan.insertedNote.retuneSpeed, 0.42f)
        && near(plan.insertedNote.vibratoDepth, 0.11f)
        && near(plan.insertedNote.vibratoRate, 5.3f)
        && plan.insertedNote.dirty;
}

bool checkSortedSequenceNormalization()
{
    const auto snapshot = makeSnapshot({ makeNote(0.0, 4.0, 220.0f), makeNote(3.0, 7.0, 246.0f) });
    const auto plan = planDrawNoteEdit(snapshot, makeInput(9.0, 10.0), 12);
    return plan.afterNotes.size() == 3
        && plan.afterNotes[0].startTime == 0.0 && plan.afterNotes[0].endTime == 3.0
        && plan.afterNotes[1].startTime == 3.0 && plan.afterNotes[1].endTime == 7.0
        && plan.afterNotes[2].startTime == 9.0 && plan.afterNotes[2].endTime == 10.0;
}

bool checkPitchRangePlanAndEmptyCurveNotePatch()
{
    auto snapshot = makeSnapshot({ makeNote(1.0, 2.0, 220.0f) });
    std::vector<float> originalF0(40, 440.0f);
    PitchCorrectionSegment existing(0, 40, std::vector<float>(40, 440.0f),
                                    PitchCorrectionSegment::Source::HandDraw);
    snapshot.pitchCurve = std::make_shared<const PitchCurveSnapshot>(
        originalF0, std::vector<float>(40, 0.0f),
        std::vector<PitchCorrectionSegment>{ existing }, 1, 1.0);
    const auto plan = planDrawNoteEdit(snapshot, makeInput(10.0, 12.0, 493.88f), 12);
    if (!plan.pitchCurveEdited
        || plan.affectedRange.startFrame != 0
        // 范围扩展到完整覆盖相交的手工段：HandDraw [0,40) 整体穿过提交机器。
        || plan.affectedRange.endFrameExclusive != 40
        || plan.beforeNotesInPitchRange.size() != 1
        || plan.beforeSegments.size() != 1
        || plan.beforeSegments[0].startFrame != 0
        || plan.beforeSegments[0].endFrame != 40
        || plan.beforeSegments[0].f0Data.size() != 40
        || plan.segmentsToCommit.empty())
        return false;

    auto emptyCurveSnapshot = makeSnapshot();
    emptyCurveSnapshot.pitchCurve = std::make_shared<const PitchCurveSnapshot>(
        std::vector<float>{}, std::vector<float>{}, std::vector<PitchCorrectionSegment>{}, 1, 1.0);
    const auto noteOnly = planDrawNoteEdit(emptyCurveSnapshot, makeInput(10.0, 12.0), 12);
    return !noteOnly.pitchCurveEdited
        && noteOnly.beforePatch.affectedRange.startSeconds == 10.0
        && noteOnly.beforePatch.affectedRange.endSeconds == 12.0
        && noteOnly.afterPatch.afterNotesInRange.size() == 1;
}

bool checkStaleRevision()
{
    const auto plan = planDrawNoteEdit(makeSnapshot(), makeInput(1.0, 2.0), 13);
    return plan.status == DrawNotePlanStatus::StaleRevision;
}

} // namespace

int main()
{
    if (!checkNoOverlapAndNoteOnlyPatch()) {
        std::cerr << "Draw note no-overlap/note-only patch failed\n";
        return 1;
    }
    if (!checkNoChangeWhenComparedFieldsMatch()) {
        std::cerr << "Draw note identical-content NoChange failed\n";
        return 1;
    }
    if (!checkOverlapSplitsAndBoundaryContact()) {
        std::cerr << "Draw note overlap splitting/boundary behavior failed\n";
        return 1;
    }
    if (!checkPipFallbackAndQuantization()) {
        std::cerr << "Draw note PIP construction failed\n";
        return 1;
    }
    if (!checkSortedSequenceNormalization()) {
        std::cerr << "Draw note sequence normalization failed\n";
        return 1;
    }
    if (!checkPitchRangePlanAndEmptyCurveNotePatch()) {
        std::cerr << "Draw note pitch-range/note-only plan failed\n";
        return 1;
    }
    if (!checkStaleRevision()) {
        std::cerr << "Draw note stale revision check failed\n";
        return 1;
    }
    return 0;
}
