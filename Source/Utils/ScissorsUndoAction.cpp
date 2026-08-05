#include "ScissorsUndoAction.h"

namespace OpenTune {

ScissorsUndoAction::ScissorsUndoAction(ContentKey key,
                                       juce::String description,
                                       std::vector<Note> beforeNotes,
                                       std::vector<Note> afterNotes,
                                       std::function<bool(const std::vector<Note>&)> applyNotes,
                                       std::function<void()> republishPlaybackSource)
    : contentKey_(key)
    , description_(std::move(description))
    , beforeNotes_(std::move(beforeNotes))
    , afterNotes_(std::move(afterNotes))
    , applyNotes_(std::move(applyNotes))
    , republishPlaybackSource_(std::move(republishPlaybackSource))
{
}

void ScissorsUndoAction::undo()
{
    applyNotes_(beforeNotes_);
    if (republishPlaybackSource_)
        republishPlaybackSource_();
}

void ScissorsUndoAction::redo()
{
    applyNotes_(afterNotes_);
    if (republishPlaybackSource_)
        republishPlaybackSource_();
}

} // namespace OpenTune
