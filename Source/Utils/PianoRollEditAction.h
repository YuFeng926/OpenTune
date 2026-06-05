#pragma once

#include "UndoManager.h"
#include "Note.h"
#include "PitchCurve.h"
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

class MaterializationContentCommands;

class PianoRollEditAction : public UndoAction {
public:
    PianoRollEditAction(std::shared_ptr<MaterializationContentCommands> commands,
                        uint64_t materializationId,
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

    uint64_t getMaterializationId() const { return materializationId_; }
    int getAffectedStartFrame() const { return affectedStartFrame_; }
    int getAffectedEndFrame() const { return affectedEndFrame_; }

private:
    std::shared_ptr<MaterializationContentCommands> commands_;
    uint64_t materializationId_;
    juce::String description_;
    std::vector<Note> oldNotes_, newNotes_;
    std::vector<CorrectedSegment> oldSegments_, newSegments_;
    int affectedStartFrame_{0};
    int affectedEndFrame_{0};
};

} // namespace OpenTune
