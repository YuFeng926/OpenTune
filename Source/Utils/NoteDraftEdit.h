#pragma once

#include "../Content/ContentEditCommands.h"

namespace OpenTune {

enum class NoteDraftEditStatus
{
    Ready,
    NoChange
};

struct NoteDraftEditPlan
{
    NoteDraftEditStatus status = NoteDraftEditStatus::NoChange;
    ContentNoteRangePatch beforePatch;
    ContentNoteRangePatch afterPatch;
};

// Inputs are sorted by startTime; startTime is the note match key.
NoteDraftEditPlan planNoteDraftEdit(const std::vector<Note>& baselineNotes,
                                    const std::vector<Note>& workingNotes);

} // namespace OpenTune
