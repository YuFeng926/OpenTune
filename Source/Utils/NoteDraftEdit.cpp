#include "NoteDraftEdit.h"

#include "../Content/ContentPatchGeometry.h"
#include <algorithm>

namespace OpenTune {

NoteDraftEditPlan planNoteDraftEdit(const std::vector<Note>& baselineNotes,
                                    const std::vector<Note>& workingNotes)
{
    NoteDraftEditPlan plan;
    const auto notesContentEqual = [](const Note& left, const Note& right) {
        return left.endTime == right.endTime
            && left.pitch == right.pitch
            && left.pitchOffset == right.pitchOffset
            && left.retuneSpeed == right.retuneSpeed
            && left.pitchDriftScale == right.pitchDriftScale
            && left.vibratoDepth == right.vibratoDepth
            && left.vibratoRate == right.vibratoRate
            && left.eq == right.eq;
    };

    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;
    std::size_t beforeIndex = 0;
    std::size_t workingIndex = 0;
    while (beforeIndex < baselineNotes.size() || workingIndex < workingNotes.size()) {
        const bool hasBefore = beforeIndex < baselineNotes.size();
        const bool hasWorking = workingIndex < workingNotes.size();
        if (hasBefore && hasWorking
            && baselineNotes[beforeIndex].startTime == workingNotes[workingIndex].startTime) {
            if (!notesContentEqual(baselineNotes[beforeIndex], workingNotes[workingIndex])) {
                dirtyStartTime = std::min(dirtyStartTime, baselineNotes[beforeIndex].startTime);
                dirtyEndTime = std::max({ dirtyEndTime,
                                          baselineNotes[beforeIndex].endTime,
                                          workingNotes[workingIndex].endTime });
            }
            ++beforeIndex;
            ++workingIndex;
        } else if (!hasWorking
                   || (hasBefore && baselineNotes[beforeIndex].startTime
                       < workingNotes[workingIndex].startTime)) {
            dirtyStartTime = std::min(dirtyStartTime, baselineNotes[beforeIndex].startTime);
            dirtyEndTime = std::max(dirtyEndTime, baselineNotes[beforeIndex].endTime);
            ++beforeIndex;
        } else {
            dirtyStartTime = std::min(dirtyStartTime, workingNotes[workingIndex].startTime);
            dirtyEndTime = std::max(dirtyEndTime, workingNotes[workingIndex].endTime);
            ++workingIndex;
        }
    }

    if (dirtyEndTime <= dirtyStartTime)
        return plan;

    plan.status = NoteDraftEditStatus::Ready;
    plan.beforePatch.affectedRange = { dirtyStartTime, dirtyEndTime };
    plan.afterPatch.affectedRange = plan.beforePatch.affectedRange;
    plan.beforePatch.afterNotesInRange = filterNotesToRange(
        baselineNotes, dirtyStartTime, dirtyEndTime);
    plan.afterPatch.afterNotesInRange = filterNotesToRange(
        workingNotes, dirtyStartTime, dirtyEndTime);
    return plan;
}

} // namespace OpenTune
