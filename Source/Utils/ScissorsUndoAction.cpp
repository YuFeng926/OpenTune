#include "ScissorsUndoAction.h"

namespace OpenTune {

ScissorsUndoAction::ScissorsUndoAction(ContentKey key,
                                       juce::String description,
                                       std::vector<Note> beforeNotes,
                                       std::vector<Note> afterNotes,
                                       std::function<bool(const std::vector<Note>&)> applyNotes)
    : contentKey_(key)
    , description_(std::move(description))
    , beforeNotes_(std::move(beforeNotes))
    , afterNotes_(std::move(afterNotes))
    , applyNotes_(std::move(applyNotes))
{
}

void ScissorsUndoAction::undo()
{
    applyNotes_(beforeNotes_);
}

void ScissorsUndoAction::redo()
{
    applyNotes_(afterNotes_);
}

} // namespace OpenTune
