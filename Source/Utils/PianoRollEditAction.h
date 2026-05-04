#pragma once

#include "UndoManager.h"
#include "Note.h"
#include "PitchCurve.h"
#include <vector>
#include <cstdint>

namespace OpenTune {

class OpenTuneAudioProcessor;

class PianoRollEditAction : public UndoAction {
public:
    PianoRollEditAction(OpenTuneAudioProcessor& processor,
                        uint64_t materializationId,
                        juce::String description,
                        std::vector<Note> oldNotes,
                        std::vector<Note> newNotes,
                        std::vector<CorrectedSegment> oldSegments,
                        std::vector<CorrectedSegment> newSegments);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

    uint64_t getMaterializationId() const { return materializationId_; }
    int getAffectedStartFrame() const { return affectedStartFrame_; }
    int getAffectedEndFrame() const { return affectedEndFrame_; }

private:
    OpenTuneAudioProcessor& processor_;
    uint64_t materializationId_;
    juce::String description_;
    std::vector<Note> oldNotes_, newNotes_;
    std::vector<CorrectedSegment> oldSegments_, newSegments_;
    int affectedStartFrame_{0};
    int affectedEndFrame_{0};
};

} // namespace OpenTune
