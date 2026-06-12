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

class PianoRollEditAction : public UndoAction {
public:
    PianoRollEditAction(std::shared_ptr<ContentEditCommands> commands,
                        ContentKey key,
                        juce::String description,
                        std::vector<Note> oldNotes,
                        std::vector<Note> newNotes,
                        std::vector<CorrectedSegment> oldSegments,
                        std::vector<CorrectedSegment> newSegments,
                        int affectedStartFrame,
                        int affectedEndFrame);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

    ContentKey getContentKey() const { return contentKey_; }
    int getAffectedStartFrame() const { return affectedStartFrame_; }
    int getAffectedEndFrame() const { return affectedEndFrame_; }

private:
    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String description_;
    std::vector<Note> oldNotes_, newNotes_;
    std::vector<CorrectedSegment> oldSegments_, newSegments_;
    int affectedStartFrame_{0};
    int affectedEndFrame_{0};
};

} // namespace OpenTune
