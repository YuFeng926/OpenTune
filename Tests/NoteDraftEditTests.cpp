#include "Utils/NoteDraftEdit.h"
#include <iostream>
#include <vector>

namespace {

using namespace OpenTune;

Note makeNote(double start, double end, float pitch)
{
    Note note;
    note.startTime = start;
    note.endTime = end;
    note.pitch = pitch;
    note.originalPitch = pitch + 10.0f;
    note.pitchOffset = 0.25f;
    note.retuneSpeed = 0.42f;
    note.vibratoDepth = 0.11f;
    note.vibratoRate = 5.3f;
    note.pitchDriftScale = 0.8f;
    return note;
}

bool sameNote(const Note& left, const Note& right)
{
    return left.startTime == right.startTime
        && left.endTime == right.endTime
        && left.pitch == right.pitch
        && left.originalPitch == right.originalPitch
        && left.pitchOffset == right.pitchOffset
        && left.retuneSpeed == right.retuneSpeed
        && left.vibratoDepth == right.vibratoDepth
        && left.vibratoRate == right.vibratoRate
        && left.outputGainDb == right.outputGainDb
        && left.pitchDriftScale == right.pitchDriftScale
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

bool sameRange(const ContentEditRangeSeconds& left, const ContentEditRangeSeconds& right)
{
    return left.startSeconds == right.startSeconds
        && left.endSeconds == right.endSeconds;
}

bool checkInsertion()
{
    const std::vector<Note> baseline = { makeNote(0.0, 1.0, 220.0f) };
    const std::vector<Note> working = {
        baseline[0],
        makeNote(2.0, 4.0, 440.0f)
    };
    const auto plan = planNoteDraftEdit(baseline, working);
    const ContentEditRangeSeconds expectedRange { 2.0, 4.0 };
    return plan.status == NoteDraftEditStatus::Ready
        && sameRange(plan.beforePatch.affectedRange, expectedRange)
        && sameRange(plan.afterPatch.affectedRange, expectedRange)
        && plan.beforePatch.afterNotesInRange.empty()
        && sameNotes(plan.afterPatch.afterNotesInRange, { working[1] });
}

bool checkDeletion()
{
    const std::vector<Note> baseline = {
        makeNote(2.0, 4.0, 440.0f),
        makeNote(5.0, 6.0, 493.88f)
    };
    const std::vector<Note> working = { baseline[1] };
    const auto plan = planNoteDraftEdit(baseline, working);
    const ContentEditRangeSeconds expectedRange { 2.0, 4.0 };
    return plan.status == NoteDraftEditStatus::Ready
        && sameRange(plan.beforePatch.affectedRange, expectedRange)
        && sameRange(plan.afterPatch.affectedRange, expectedRange)
        && sameNotes(plan.beforePatch.afterNotesInRange, { baseline[0] })
        && plan.afterPatch.afterNotesInRange.empty();
}

bool checkComparedFields()
{
    const Note baseline = makeNote(2.0, 3.0, 220.0f);
    std::vector<Note> changedNotes;

    auto changed = baseline;
    changed.endTime = 4.0;
    changedNotes.push_back(changed);
    changed = baseline;
    changed.pitch = 330.0f;
    changedNotes.push_back(changed);
    changed = baseline;
    changed.pitchOffset = 1.25f;
    changedNotes.push_back(changed);
    changed = baseline;
    changed.retuneSpeed = 0.7f;
    changedNotes.push_back(changed);
    changed = baseline;
    changed.pitchDriftScale = 0.4f;
    changedNotes.push_back(changed);
    changed = baseline;
    changed.vibratoDepth = 0.4f;
    changedNotes.push_back(changed);
    changed = baseline;
    changed.vibratoRate = 7.0f;
    changedNotes.push_back(changed);
    changed = baseline;
    changed.eq = EqSettings {};
    changedNotes.push_back(changed);

    for (const auto& after : changedNotes) {
        const auto plan = planNoteDraftEdit({ baseline }, { after });
        const ContentEditRangeSeconds expectedRange {
            baseline.startTime,
            std::max(baseline.endTime, after.endTime)
        };
        if (plan.status != NoteDraftEditStatus::Ready
            || !sameRange(plan.beforePatch.affectedRange, expectedRange)
            || !sameRange(plan.afterPatch.affectedRange, expectedRange)
            || !sameNotes(plan.beforePatch.afterNotesInRange, { baseline })
            || !sameNotes(plan.afterPatch.afterNotesInRange, { after }))
            return false;
    }
    return true;
}

bool checkNoChangeIgnoresUncomparedFields()
{
    const Note baseline = makeNote(2.0, 3.0, 220.0f);
    Note working = baseline;
    working.originalPitch = 880.0f;
    working.outputGainDb = -6.0f;
    working.isVoiced = false;
    working.dirty = true;

    const auto plan = planNoteDraftEdit({ baseline }, { working });
    return plan.status == NoteDraftEditStatus::NoChange
        && sameRange(plan.beforePatch.affectedRange, {})
        && sameRange(plan.afterPatch.affectedRange, {})
        && plan.beforePatch.afterNotesInRange.empty()
        && plan.afterPatch.afterNotesInRange.empty();
}

bool checkStartTimeIsMatchKey()
{
    const Note baseline = makeNote(1.0, 2.0, 220.0f);
    Note working = baseline;
    working.startTime = 1.5;
    working.endTime = 2.5;

    const auto plan = planNoteDraftEdit({ baseline }, { working });
    const ContentEditRangeSeconds expectedRange { 1.0, 2.5 };
    return plan.status == NoteDraftEditStatus::Ready
        && sameRange(plan.beforePatch.affectedRange, expectedRange)
        && sameRange(plan.afterPatch.affectedRange, expectedRange)
        && sameNotes(plan.beforePatch.afterNotesInRange, { baseline })
        && sameNotes(plan.afterPatch.afterNotesInRange, { working });
}

} // namespace

int main()
{
    if (!checkInsertion()) {
        std::cerr << "Note draft insertion diff failed\n";
        return 1;
    }
    if (!checkDeletion()) {
        std::cerr << "Note draft deletion diff failed\n";
        return 1;
    }
    if (!checkComparedFields()) {
        std::cerr << "Note draft compared-field diff failed\n";
        return 1;
    }
    if (!checkNoChangeIgnoresUncomparedFields()) {
        std::cerr << "Note draft NoChange field semantics failed\n";
        return 1;
    }
    if (!checkStartTimeIsMatchKey()) {
        std::cerr << "Note draft startTime matching semantics failed\n";
        return 1;
    }
    return 0;
}
