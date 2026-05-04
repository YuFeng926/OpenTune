#include "PianoRollEditAction.h"
#include "../PluginProcessor.h"
#include <limits>

namespace OpenTune {

PianoRollEditAction::PianoRollEditAction(OpenTuneAudioProcessor& processor,
                                         uint64_t materializationId,
                                         juce::String description,
                                         std::vector<Note> oldNotes,
                                         std::vector<Note> newNotes,
                                         std::vector<CorrectedSegment> oldSegments,
                                         std::vector<CorrectedSegment> newSegments)
    : processor_(processor)
    , materializationId_(materializationId)
    , description_(std::move(description))
    , oldNotes_(std::move(oldNotes))
    , newNotes_(std::move(newNotes))
    , oldSegments_(std::move(oldSegments))
    , newSegments_(std::move(newSegments))
{
    // Compute affected frame range from union of old and new segments
    int minFrame = std::numeric_limits<int>::max();
    int maxFrame = 0;
    for (const auto& seg : oldSegments_) {
        minFrame = std::min(minFrame, seg.startFrame);
        maxFrame = std::max(maxFrame, seg.endFrame);
    }
    for (const auto& seg : newSegments_) {
        minFrame = std::min(minFrame, seg.startFrame);
        maxFrame = std::max(maxFrame, seg.endFrame);
    }
    affectedStartFrame_ = (minFrame == std::numeric_limits<int>::max()) ? 0 : minFrame;
    affectedEndFrame_ = maxFrame;
}

void PianoRollEditAction::undo()
{
    processor_.commitMaterializationNotesAndSegmentsById(materializationId_, oldNotes_, oldSegments_);
}

void PianoRollEditAction::redo()
{
    processor_.commitMaterializationNotesAndSegmentsById(materializationId_, newNotes_, newSegments_);
}

} // namespace OpenTune
