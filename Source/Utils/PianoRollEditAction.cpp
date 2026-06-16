#include "PianoRollEditAction.h"
#include "Content/ContentEditCommands.h"
#include <limits>

namespace OpenTune {

PianoRollEditAction::PianoRollEditAction(std::shared_ptr<ContentEditCommands> commands,
                                         ContentKey key,
                                         juce::String description,
                                         std::vector<Note> oldNotes,
                                         std::vector<Note> newNotes,
                                         std::vector<CorrectedSegment> oldSegments,
                                         std::vector<CorrectedSegment> newSegments,
                                         int affectedStartFrame,
                                         int affectedEndFrame)
    : commands_(commands)
    , contentKey_(key)
    , description_(std::move(description))
    , oldNotes_(std::move(oldNotes))
    , newNotes_(std::move(newNotes))
    , oldSegments_(std::move(oldSegments))
    , newSegments_(std::move(newSegments))
    , affectedStartFrame_(affectedStartFrame)
    , affectedEndFrame_(affectedEndFrame)
{
    jassert(affectedStartFrame_ >= 0);
    jassert(affectedEndFrame_ >= affectedStartFrame_);
}

void PianoRollEditAction::undo()
{
    if (commands_ != nullptr)
        commands_->commitNotesAndSegments(contentKey_, oldNotes_, oldSegments_,
            ContentEditRangeFrames{affectedStartFrame_, affectedEndFrame_});
}

void PianoRollEditAction::redo()
{
    if (commands_ != nullptr)
        commands_->commitNotesAndSegments(contentKey_, newNotes_, newSegments_,
            ContentEditRangeFrames{affectedStartFrame_, affectedEndFrame_});
}

} // namespace OpenTune
