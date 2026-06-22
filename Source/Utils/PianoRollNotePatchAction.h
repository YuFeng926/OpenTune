#pragma once

#include "UndoManager.h"
#include "Note.h"
#include "Content/ContentKey.h"
#include "Content/ContentEditCommands.h"
#include <memory>
#include <vector>

namespace OpenTune {

// Seconds-based note-only undo action. Stores before/after ContentNoteRangePatch
// and calls commitNotePatch() on undo/redo — no frame conversion, no segments.
// Separate from PianoRollEditAction which handles frame-based pitch+note edits.
class PianoRollNotePatchAction : public UndoAction {
public:
    PianoRollNotePatchAction(std::shared_ptr<ContentEditCommands> commands,
                              ContentKey key,
                              juce::String description,
                              ContentNoteRangePatch beforePatch,
                              ContentNoteRangePatch afterPatch);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

private:
    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String description_;
    ContentNoteRangePatch beforePatch_, afterPatch_;
};

} // namespace OpenTune
