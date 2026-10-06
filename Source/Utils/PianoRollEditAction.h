#pragma once

#include "UndoManager.h"
#include "Note.h"
#include "PitchCurve.h"
#include "Content/ContentKey.h"
#include "Content/ContentEditCommands.h"
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

// Range-scoped undo action: stores only the notes, correction segments and
// optional OriginalF0 patch for the affected range. On undo/redo, the content
// command merges the patch with the current data outside that range.
class PianoRollEditAction : public UndoAction {
public:
    PianoRollEditAction(std::shared_ptr<ContentEditCommands> commands,
                        ContentKey key,
                        juce::String description,
                        std::vector<Note> beforeNotesInRange,
                        std::vector<Note> afterNotesInRange,
                        std::vector<PitchCorrectionSegment> beforeSegments,
                        std::vector<PitchCorrectionSegment> afterSegments,
                        ContentEditRangeFrames affectedRange,
                        std::vector<float> beforeOriginalF0InRange = {},
                        std::vector<float> afterOriginalF0InRange = {});

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

private:
    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String description_;
    std::vector<Note> beforeNotes_, afterNotes_;
    std::vector<PitchCorrectionSegment> beforeSegments_, afterSegments_;
    std::vector<float> beforeOriginalF0_, afterOriginalF0_;
    ContentEditRangeFrames affectedRange_;
};

} // namespace OpenTune
