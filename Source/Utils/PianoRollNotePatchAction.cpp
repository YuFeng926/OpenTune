#include "PianoRollNotePatchAction.h"

namespace OpenTune {

PianoRollNotePatchAction::PianoRollNotePatchAction(std::shared_ptr<ContentEditCommands> commands,
                                                   ContentKey key,
                                                   juce::String description,
                                                   ContentNoteRangePatch beforePatch,
                                                   ContentNoteRangePatch afterPatch)
    : commands_(commands)
    , contentKey_(key)
    , description_(std::move(description))
    , beforePatch_(beforePatch)
    , afterPatch_(afterPatch)
{
}

void PianoRollNotePatchAction::undo()
{
    commands_->commitNoteTopologyPatch(contentKey_, beforePatch_);
}

void PianoRollNotePatchAction::redo()
{
    commands_->commitNoteTopologyPatch(contentKey_, afterPatch_);
}

} // namespace OpenTune
