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

// Range-scoped undo action: stores only the notes and segments within
// the affected frame range, not full vectors.  On undo/redo,
// commitNotesAndSegments() merges the range-scoped patch with the
// current stored data outside the range.
class PianoRollEditAction : public UndoAction {
public:
    PianoRollEditAction(std::shared_ptr<ContentEditCommands> commands,
                        ContentKey key,
                        juce::String description,
                        std::vector<Note> beforeNotesInRange,
                        std::vector<Note> afterNotesInRange,
                        std::vector<CorrectedSegment> beforeSegments,
                        std::vector<CorrectedSegment> afterSegments,
                        ContentEditRangeFrames affectedRange);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

    ContentKey getContentKey() const { return contentKey_; }
    int getAffectedStartFrame() const { return affectedRange_.startFrame; }
    int getAffectedEndFrame() const { return affectedRange_.endFrameExclusive - 1; }

private:
    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String description_;
    std::vector<Note> beforeNotes_, afterNotes_;
    std::vector<CorrectedSegment> beforeSegments_, afterSegments_;
    ContentEditRangeFrames affectedRange_;
};

} // namespace OpenTune
